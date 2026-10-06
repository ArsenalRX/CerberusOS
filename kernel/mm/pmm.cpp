// Bitmap allocator. Bit set = frame in use (allocated or reserved). A
// rolling "next free" hint keeps single-frame allocation O(1) in the common
// case; multi-frame allocations scan from the start (first fit, as the spec
// asks) so they reuse low fragments before spreading out.
#include <arch/x86_64/cpu.h>
#include <boot/bootinfo.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <mm/early_map.h>
#include <mm/pmm.h>
#include <lib/lock_order.h>
#include <sched/sched.h>
#include <sched/sync.h>

namespace {

Spinlock g_lock = SPINLOCK_RANKED(lock_rank::PMM);    // the bitmap, the counters and the hint
u64* g_bitmap = nullptr;
u64 g_frames = 0;           // bitmap length in frames
u64 g_words = 0;
u64 g_usable = 0;
u64 g_used = 0;
u64 g_hint = 0;             // frame index to start single-frame searches from

inline bool test(u64 f) { return g_bitmap[f / 64] & (1ull << (f % 64)); }
inline void set(u64 f) { g_bitmap[f / 64] |= 1ull << (f % 64); }
inline void clear(u64 f) { g_bitmap[f / 64] &= ~(1ull << (f % 64)); }

void mark_range(paddr_t base, u64 length, bool used) {
    u64 first = align_up(base, PAGE_SIZE) / PAGE_SIZE;
    u64 last = align_down(base + length, PAGE_SIZE) / PAGE_SIZE;   // exclusive
    if (used) {
        first = align_down(base, PAGE_SIZE) / PAGE_SIZE;
        last = align_up(base + length, PAGE_SIZE) / PAGE_SIZE;
    }
    for (u64 f = first; f < last && f < g_frames; f++) {
        if (used) set(f);
        else clear(f);
    }
}

u64 find_run(usize count, u64 start) {
    u64 run = 0;
    for (u64 f = start; f < g_frames; f++) {
        if (test(f)) {
            run = 0;
            continue;
        }
        if (++run == count) return f + 1 - count;
    }
    return (u64)-1;
}

} // namespace

void pmm_init() {
    const BootInfo& bi = g_boot_info;
    u64 top = bi.total_bytes();
    g_frames = align_up(top, PAGE_SIZE) / PAGE_SIZE;
    g_words = (g_frames + 63) / 64;
    u64 bitmap_bytes = g_words * 8;

    // Find a usable region big enough for the bitmap (skip the first MiB:
    // legacy areas are better left alone).
    paddr_t bitmap_phys = 0;
    for (usize i = 0; i < bi.region_count; i++) {
        const MemoryRegion& r = bi.regions[i];
        if (r.type != MemoryType::Usable) continue;
        paddr_t base = max<paddr_t>(align_up(r.base, PAGE_SIZE), MIB);
        paddr_t end = r.base + r.length;
        if (base < end && end - base >= bitmap_bytes) {
            bitmap_phys = base;
            break;
        }
    }
    if (!bitmap_phys) PANIC("pmm: no usable region can hold a %lu-byte bitmap", (unsigned long)bitmap_bytes);
    g_bitmap = (u64*)hhdm_virt(bitmap_phys);

    // Everything reserved, then usable regions freed, then the bitmap reserved.
    memset(g_bitmap, 0xFF, bitmap_bytes);
    for (usize i = 0; i < bi.region_count; i++)
        if (bi.regions[i].type == MemoryType::Usable) mark_range(bi.regions[i].base, bi.regions[i].length, false);
    mark_range(0, PAGE_SIZE, true);                     // never hand out frame 0
    mark_range(bitmap_phys, bitmap_bytes, true);

    g_usable = 0;
    for (u64 f = 0; f < g_frames; f++)
        if (!test(f)) g_usable++;
    g_used = 0;
    g_hint = 0;

    kprintf("pmm: %lu frames tracked (%lu MiB), %lu usable (%lu MiB), bitmap %lu KiB at %#lx\n",
            (unsigned long)g_frames, (unsigned long)(g_frames * PAGE_SIZE / MIB), (unsigned long)g_usable,
            (unsigned long)(g_usable * PAGE_SIZE / MIB), (unsigned long)(bitmap_bytes / KIB),
            (unsigned long)bitmap_phys);
}

namespace {
paddr_t pool_take(bool refill);
} // namespace

paddr_t pmm_alloc(usize count) {
    if (!count || !g_bitmap) return PMM_NO_MEMORY;
    g_lock.lock();
    u64 f = (u64)-1;
    if (count == 1) {
        f = find_run(1, g_hint);
        if (f == (u64)-1) f = find_run(1, 0);
    } else {
        f = find_run(count, 0);
    }
    if (f == (u64)-1) {
        g_lock.unlock();
        // The bitmap is out, but the pre-zeroed pool may still hold frames
        // (they are reported as free): hand one of those out.
        return count == 1 ? pool_take(false) : PMM_NO_MEMORY;
    }
    for (u64 i = 0; i < count; i++) set(f + i);
    g_used += count;
    if (count == 1) g_hint = f + 1;
    g_lock.unlock();
    return f * PAGE_SIZE;
}

// Frames zeroed ahead of time by a background thread (B-015, the minor
// page fault over budget): a fault then takes a ready frame instead of
// clearing one, which also moves the hypervisor's first-touch cost out of
// the fault. The pool is refilled when it runs low.
namespace {
constexpr u32 ZERO_POOL = 512;
paddr_t g_zero_pool[ZERO_POOL];
u32 g_zero_count = 0;
Spinlock g_zero_lock = SPINLOCK_RANKED(lock_rank::UNRANKED);   // a leaf
WaitQueue g_zero_wake;
bool g_zero_thread = false;
bool g_zero_paused = false;         // tests that audit the bitmap stop the refills

void zero_thread(void*) {
    for (;;) {
        u32 have;
        {
            SpinGuard guard(g_zero_lock);
            have = g_zero_count;
        }
        while (have < ZERO_POOL) {
            paddr_t p = pmm_alloc(1);
            if (p == PMM_NO_MEMORY) break;
            memset(hhdm_virt(p), 0, PAGE_SIZE);
            bool kept;
            {
                SpinGuard guard(g_zero_lock);
                kept = !g_zero_paused && g_zero_count < ZERO_POOL;
                if (kept) g_zero_pool[g_zero_count++] = p;
                have = kept ? g_zero_count : ZERO_POOL;
            }
            if (!kept) {
                pmm_free(p, 1);
                break;
            }
        }
        g_zero_wake.wait_ticks(100);
    }
}
} // namespace

namespace {
// One frame from the pool, or PMM_NO_MEMORY; `refill` wakes the zeroing
// thread when the pool runs low.
paddr_t pool_take(bool refill) {
    paddr_t p = PMM_NO_MEMORY;
    u32 left = 0;
    {
        SpinGuard guard(g_zero_lock);
        if (g_zero_count) {
            p = g_zero_pool[--g_zero_count];
            left = g_zero_count;
        }
    }
    if (p != PMM_NO_MEMORY && refill && left < ZERO_POOL / 4 && g_zero_thread) g_zero_wake.wake_one();
    return p;
}
} // namespace

paddr_t pmm_alloc_zeroed(usize count) {
    if (count == 1) {
        paddr_t p = pool_take(true);
        if (p != PMM_NO_MEMORY) return p;
    }
    paddr_t p = pmm_alloc(count);
    if (p != PMM_NO_MEMORY) memset(hhdm_virt(p), 0, count * PAGE_SIZE);
    return p;
}

void pmm_start_zeroing() {
    Result<Thread*> t = kthread_create(zero_thread, nullptr, "zeroer", prio::LOW, nullptr, true);
    g_zero_thread = t.ok();
    if (!g_zero_thread) kprintf("pmm: no zeroing thread (frames are cleared on demand)\n");
}

u32 pmm_zeroed_ready() {
    SpinGuard guard(g_zero_lock);
    return g_zero_count;
}

void pmm_zeroing_pause(bool pause) {
    paddr_t drained[ZERO_POOL];
    u32 n = 0;
    {
        SpinGuard guard(g_zero_lock);
        g_zero_paused = pause;
        if (pause) {
            while (g_zero_count) drained[n++] = g_zero_pool[--g_zero_count];
        }
    }
    for (u32 i = 0; i < n; i++) pmm_free(drained[i], 1);
    if (!pause && g_zero_thread) g_zero_wake.wake_one();
}

void pmm_free(paddr_t addr, usize count) {
    if (!count) return;
    ASSERT_ALWAYS((addr % PAGE_SIZE) == 0);
    u64 f = addr / PAGE_SIZE;
    ASSERT_ALWAYS(f + count <= g_frames);
    g_lock.lock();
    for (u64 i = 0; i < count; i++) {
        ASSERT(test(f + i));            // double free
        clear(f + i);
    }
    g_used -= count;
    if (f < g_hint) g_hint = f;
    g_lock.unlock();
}

PmmStats pmm_stats() {
    PmmStats s{};
    g_lock.lock();
    s.total_frames = g_frames;
    s.usable_frames = g_usable;
    // Frames waiting in the pre-zeroed pool belong to nobody yet: they
    // count as free, so the pool's refills do not show up as use.
    u64 pooled = pmm_zeroed_ready();
    s.used_frames = g_used - pooled;
    s.free_frames = g_usable - g_used + pooled;
    s.reserved_frames = g_frames - g_usable;
    u64 run = 0, best = 0;
    for (u64 f = 0; f < g_frames; f++) {
        if (test(f)) run = 0;
        else if (++run > best) best = run;
    }
    s.largest_free_run = best;
    g_lock.unlock();
    return s;
}

u64 pmm_bitmap_checksum() {
    u64 h = 1469598103934665603ull;
    for (u64 i = 0; i < g_words; i++) {
        h ^= g_bitmap[i];
        h *= 1099511628211ull;
    }
    return h;
}
