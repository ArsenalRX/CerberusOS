// See futex.h.
//
// The waiter is put on the queue *before* the word is read, so a waker that
// changes the word and then wakes cannot slip between the check and the
// sleep: either the waiter sees the new value, or the waker finds it queued.
#include <ipc/futex.h>
#include <mm/usercopy.h>
#include <mm/vmm.h>
#include <sched/sched.h>

namespace {

struct Key {
    u64 space;                  // 0 for shared memory
    u64 where;                  // physical address (shared) or virtual address (private)
    bool operator==(const Key& o) const { return space == o.space && where == o.where; }
};

struct Waiter {
    Key key;
    WaitQueue wq;
    bool woken;
    Waiter* next;
};

constexpr u32 BUCKETS = 64;
Waiter* g_buckets[BUCKETS];     // scheduler lock

u32 bucket_of(const Key& k) { return (u32)(((k.space >> 12) ^ (k.where >> 2)) * 0x9E3779B1u) % BUCKETS; }

// The word must be readable now (which also brings its page in).
Result<Key> key_for(vaddr_t addr) {
    if (addr & 3) return Error::Invalid;
    u32 probe;
    Result<void> r = copy_from_user(&probe, addr, sizeof probe);
    if (!r.ok()) return r.error();
    AddressSpace* space = thread_current()->process->space;
    Result<paddr_t> shared = space->device_phys(addr);
    if (shared.ok()) return Key{0, shared.value()};
    return Key{(u64)space, addr};
}

void unlink_locked(Waiter* w) {
    for (Waiter** link = &g_buckets[bucket_of(w->key)]; *link; link = &(*link)->next)
        if (*link == w) {
            *link = w->next;
            return;
        }
}

} // namespace

Result<void> futex_wait(vaddr_t addr, u32 val, u64 timeout_ms) {
    Result<Key> key = key_for(addr);
    if (!key.ok()) return key.error();
    Waiter w{key.value(), {}, false, nullptr};
    u64 irq = sched_lock();
    u32 b = bucket_of(w.key);
    w.next = g_buckets[b];
    g_buckets[b] = &w;
    sched_unlock(irq);

    u32 now = 0;
    Result<void> read = copy_from_user(&now, addr, sizeof now);
    Error err = !read.ok() ? read.error() : now != val ? Error::Again : Error::None;

    irq = sched_lock();
    if (err == Error::None && !w.woken) {
        u64 ticks = timeout_ms == FUTEX_FOREVER ? 0 : (timeout_ms + SCHED_TICK_MS - 1) / SCHED_TICK_MS + 1;
        WaitResult r = sched_block_interruptible_locked(&w.wq, ticks);
        if (!w.woken) err = r == WaitResult::Interrupted ? Error::Interrupted : Error::Timeout;
    }
    // A wake that arrived counts even if the value check failed: it was
    // meant for a waiter, and this one is no longer going to wait.
    if (!w.woken) unlink_locked(&w);
    else err = Error::None;
    sched_unlock(irq);
    if (err != Error::None) return err;
    return {};
}

Result<u32> futex_wake(vaddr_t addr, u32 count) {
    Result<Key> key = key_for(addr);
    if (!key.ok()) return key.error();
    u32 woken = 0;
    u64 irq = sched_lock();
    Waiter** link = &g_buckets[bucket_of(key.value())];
    while (*link && woken < count) {
        Waiter* w = *link;
        if (!(w->key == key.value())) {
            link = &w->next;
            continue;
        }
        *link = w->next;
        w->woken = true;
        sched_wake_all_locked(w->wq);
        woken++;
    }
    sched_unlock(irq);
    return woken;
}
