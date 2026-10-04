// Spinlocks, and the sleeping primitives built on the scheduler lock.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <lib/panic.h>
#include <sched/sync.h>

namespace {

#ifdef CERBERUS_DEBUG
// The rank check (lib/lock_order.h). Ranks are tracked per CPU, not per
// thread: a spinlock is held with interrupts off, so its holder cannot leave
// the CPU, and the scheduler lock, which is deliberately held across a
// thread switch, is released by whichever thread the CPU runs next.
void rank_push(const Spinlock* l) {
    if (!l->rank) return;
    PerCpu* c = percpu();
    for (u8 i = 0; i < c->held_count; i++) {
        if (c->held[i] >= l->rank)
            PANIC("lock order violation: taking a lock of rank %u while holding one of rank %u "
                  "(see kernel/lib/lock_order.h)",
                  l->rank, c->held[i]);
    }
    if (c->held_count == sizeof c->held) PANIC("lock order: too many locks held at once");
    c->held[c->held_count++] = l->rank;
}

void rank_pop(const Spinlock* l) {
    if (!l->rank) return;
    PerCpu* c = percpu();
    for (u8 i = c->held_count; i > 0; i--) {
        if (c->held[i - 1] != l->rank) continue;
        for (u8 j = i; j < c->held_count; j++) c->held[j - 1] = c->held[j];
        c->held_count--;
        return;
    }
    PANIC("lock order: releasing a lock of rank %u that this CPU does not hold", l->rank);
}
#else
inline void rank_push(const Spinlock*) {}
inline void rank_pop(const Spinlock*) {}
#endif

} // namespace

void Spinlock::acquire() {
    // After a panic the other CPUs are stopped wherever they were, possibly
    // holding locks; the panicking CPU must still be able to print.
    if (panic_in_progress()) return;
    u32 me = percpu_cpu_id() + 1;
    if (owner == me) PANIC("spinlock of rank %u taken twice by cpu %u", rank, me - 1);
    rank_push(this);
    while (__atomic_exchange_n(&locked, 1u, __ATOMIC_ACQUIRE)) {
        // Wait without hammering the cache line, and keep answering other
        // CPUs: one of them may be holding this very lock while it waits for
        // this CPU to flush its TLB.
        while (__atomic_load_n(&locked, __ATOMIC_RELAXED)) {
            cpu_relax();
            smp_poll();
            if (panic_in_progress()) return;
        }
    }
    owner = me;
}

void Spinlock::release() {
    if (panic_in_progress()) return;
    owner = 0;
    rank_pop(this);
    __atomic_store_n(&locked, 0u, __ATOMIC_RELEASE);
}

void Spinlock::lock() {
    u64 flags = interrupts_save();
    acquire();
    saved_flags = flags;
}

void Spinlock::unlock() {
    u64 flags = saved_flags;
    release();
    interrupts_restore(flags);
}

bool Spinlock::held_by_this_cpu() const { return owner == percpu_cpu_id() + 1; }

void Mutex::lock() {
    u64 irq = sched_lock();
    ASSERT(owner != thread_current());      // not recursive
    while (owner) sched_block_locked(waiters);
    owner = thread_current();
    sched_unlock(irq);
}

bool Mutex::try_lock() {
    u64 irq = sched_lock();
    bool got = !owner;
    if (got) owner = thread_current();
    sched_unlock(irq);
    return got;
}

void Mutex::unlock() {
    u64 irq = sched_lock();
    ASSERT_ALWAYS(owner == thread_current());
    owner = nullptr;
    sched_wake_one_locked(waiters);
    sched_unlock(irq);
}

void Semaphore::down() {
    u64 irq = sched_lock();
    while (count <= 0) sched_block_locked(waiters);
    count--;
    sched_unlock(irq);
}

bool Semaphore::down_ticks(u64 ticks) {
    u64 irq = sched_lock();
    u64 deadline = sched_ticks() + (ticks ? ticks : 1);
    while (count <= 0) {
        u64 now = sched_ticks();
        if (now >= deadline || !sched_block_locked_ticks(waiters, deadline - now)) {
            if (count > 0) break;
            sched_unlock(irq);
            return false;
        }
    }
    count--;
    sched_unlock(irq);
    return true;
}

bool Semaphore::down_interruptible() {
    u64 irq = sched_lock();
    while (count <= 0) {
        if (sched_block_interruptible_locked(&waiters, 0) == WaitResult::Interrupted && count <= 0) {
            sched_unlock(irq);
            return false;
        }
    }
    count--;
    sched_unlock(irq);
    return true;
}

bool Semaphore::try_down() {
    u64 irq = sched_lock();
    bool got = count > 0;
    if (got) count--;
    sched_unlock(irq);
    return got;
}

void Semaphore::up() {
    u64 irq = sched_lock();
    count++;
    sched_wake_one_locked(waiters);
    sched_unlock(irq);
}

void CondVar::wait(Mutex& m) {
    u64 irq = sched_lock();
    ASSERT_ALWAYS(m.owner == thread_current());
    // Release the mutex and go to sleep with no window in between: the
    // scheduler lock is held from here until this thread is off the CPU, so
    // nothing can signal before it is on the queue.
    m.owner = nullptr;
    sched_wake_one_locked(m.waiters);
    sched_block_locked(waiters);
    sched_unlock(irq);
    m.lock();
}

void CondVar::signal() { waiters.wake_one(); }
void CondVar::broadcast() { waiters.wake_all(); }

void RwLock::read_lock() {
    u64 irq = sched_lock();
    // Wait behind any waiting writer so a stream of readers cannot starve it.
    while (writing || writers_waiting) sched_block_locked(read_waiters);
    readers++;
    sched_unlock(irq);
}

void RwLock::read_unlock() {
    u64 irq = sched_lock();
    ASSERT_ALWAYS(readers > 0);
    if (--readers == 0) sched_wake_one_locked(write_waiters);
    sched_unlock(irq);
}

void RwLock::write_lock() {
    u64 irq = sched_lock();
    writers_waiting++;
    while (writing || readers) sched_block_locked(write_waiters);
    writers_waiting--;
    writing = true;
    sched_unlock(irq);
}

void RwLock::write_unlock() {
    u64 irq = sched_lock();
    ASSERT_ALWAYS(writing);
    writing = false;
    if (writers_waiting > 0) sched_wake_one_locked(write_waiters);
    else sched_wake_all_locked(read_waiters);
    sched_unlock(irq);
}
