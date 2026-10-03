// Phase 5 acceptance (SPEC §5 and §5A): 100,000 random allocate/free pairs
// of random sizes with every buffer's contents verified and no leak; a
// deliberate overrun caught by the red zone; plus alignment, zeroing,
// krealloc, kfree_sensitive, the large path and a rough timing. The halting
// checks (corrupted free list, write after free, double free) live in
// test_exceptions.cpp.
#include <drivers/refclock.h>
#include <kernel/ktest.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/pmm.h>

namespace {

u64 g_rng = 0xD1B54A32D192ED03ull;
u64 rng() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

constexpr usize SLOTS = 512;
struct Slot {
    u8* ptr;
    u32 size;
    u8 fill;
};
Slot g_slots[SLOTS];

// Mostly small sizes, some around the slab/page boundaries, a few large.
usize random_size() {
    u64 r = rng();
    switch (r % 16) {
    case 0: return 2049 + (r >> 8) % 6000;              // just past the slab limit
    case 1: return 8192 + (r >> 8) % 60000;             // several pages
    case 2: return 1 + (r >> 8) % 16;
    default: return 1 + (r >> 8) % 2048;
    }
}

bool filled_with(const u8* p, usize n, u8 value) {
    for (usize i = 0; i < n; i++)
        if (p[i] != value) return false;
    return true;
}

} // namespace

int ktest_heap(int, char**) {
    KheapStats hs0 = kheap_stats();
    PmmStats pm0 = pmm_stats();

    // --- basics: alignment, poison, zeroing, size 0 ---
    KTEST_CHECK(kmalloc(0) == nullptr);
    u8* a = (u8*)kmalloc(40);
    KTEST_CHECK(a && ((u64)a & 15) == 0 && ksize(a) == 64 && kheap_check(a));
#ifdef LUMEN_DEBUG
    KTEST_CHECK(filled_with(a, 40, 0xDE));
#endif
    u8* z = (u8*)kzalloc(300);
    KTEST_CHECK(z && filled_with(z, 300, 0));
    kfree(z);

    // --- deliberate overrun: one byte past the end lands in the red zone ---
#ifdef LUMEN_DEBUG
    u8 saved = a[40];
    a[40] = 0x41;
    KTEST_CHECK(!kheap_check(a));
    a[40] = saved;
    KTEST_CHECK(kheap_check(a));
    saved = a[-1];
    a[-1] = 0x41;
    KTEST_CHECK(!kheap_check(a));
    a[-1] = saved;
    kprintf("  red zones: a write one byte past the end, and one byte before the start, are both detected\n");
#else
    kprintf("  red zones: debug builds only\n");
#endif
    kfree(a);

    // --- krealloc ---
    u8* r = (u8*)krealloc(nullptr, 20);
    KTEST_CHECK(r);
    memset(r, 0x5A, 20);
    u8* r2 = (u8*)krealloc(r, 30);          // same 32-byte class: stays put
    KTEST_CHECK(r2 == r && filled_with(r2, 20, 0x5A) && kheap_check(r2));
    u8* r3 = (u8*)krealloc(r2, 5000);       // moves to the large path
    KTEST_CHECK(r3 && r3 != r2 && filled_with(r3, 20, 0x5A) && ((u64)r3 & (PAGE_SIZE - 1)) == 0);
    u8* r4 = (u8*)krealloc(r3, 10);         // and back to a small slab
    KTEST_CHECK(r4 && filled_with(r4, 10, 0x5A) && ksize(r4) == 16);
    KTEST_CHECK(krealloc(r4, 0) == nullptr);
    kprintf("  krealloc: grows in place within a size class, moves across classes, keeps the contents\n");

    // --- kfree_sensitive wipes before freeing ---
    u8* secret = (u8*)kmalloc(64);
    KTEST_CHECK(secret);
    memset(secret, 0x77, 64);
    kfree_sensitive(secret, 64);
    // Deliberate read of freed memory: the secret bytes must be gone.
    bool gone = true;
    for (usize i = 8; i < 64; i++)
        if (((volatile u8*)secret)[i] == 0x77) gone = false;
    KTEST_CHECK(gone);
    kprintf("  kfree_sensitive: the old contents are gone after the free\n");

    // --- large path: page-aligned, frames returned on free ---
    PmmStats before_large = pmm_stats();
    u8* big = (u8*)kmalloc(3 * MIB + 123);
    KTEST_CHECK(big && ((u64)big & (PAGE_SIZE - 1)) == 0 && ksize(big) == align_up(3 * MIB + 123, PAGE_SIZE));
    big[0] = 1;
    big[3 * MIB + 122] = 2;
    KTEST_CHECK(kheap_check(big));
    KTEST_CHECK(pmm_stats().used_frames >= before_large.used_frames + 769);
    kfree(big);
    kprintf("  large allocation: 3 MiB taken as whole pages and returned\n");

    // --- 100,000 random allocate/free pairs ---
    memset(g_slots, 0, sizeof g_slots);
    const u32 PAIRS = 100000;
    u32 allocs = 0, frees = 0, failed = 0;
    u64 bytes = 0;
    u64 t0 = refclock_now_us();
    while (allocs < PAIRS) {
        Slot& s = g_slots[rng() % SLOTS];
        if (s.ptr) {
            if (!filled_with(s.ptr, s.size, s.fill) || !kheap_check(s.ptr)) {
                kprintf("  corruption in a %u-byte allocation at %p\n", s.size, (void*)s.ptr);
                return 1;
            }
            kfree(s.ptr);
            s.ptr = nullptr;
            frees++;
        } else {
            s.size = (u32)random_size();
            s.fill = (u8)rng();
            s.ptr = (u8*)kmalloc(s.size);
            if (!s.ptr) {
                failed++;
                continue;
            }
            memset(s.ptr, s.fill, s.size);
            bytes += s.size;
            allocs++;
        }
    }
    for (Slot& s : g_slots) {
        if (!s.ptr) continue;
        if (!filled_with(s.ptr, s.size, s.fill) || !kheap_check(s.ptr)) {
            kprintf("  corruption in a %u-byte allocation at %p\n", s.size, (void*)s.ptr);
            return 1;
        }
        kfree(s.ptr);
        s.ptr = nullptr;
        frees++;
    }
    u64 t1 = refclock_now_us();
    KTEST_CHECK(failed == 0 && allocs == PAIRS && frees == PAIRS);
    kprintf("  %u random allocate/free pairs (%lu MiB in total, sizes 1 byte to 68 KiB): every buffer intact\n",
            PAIRS, (unsigned long)(bytes / MIB));
    kprintf("  timing: %lu ms for the whole run, fill and verify included\n", (unsigned long)((t1 - t0) / 1000));

    // --- allocator-only timing: one small size, no fill ---
    const u32 FAST = 200000;
    u64 f0 = refclock_now_us();
    for (u32 i = 0; i < FAST; i++) {
        void* p = kmalloc(64);
        if (!p) return 1;
        kfree(p);
    }
    u64 f1 = refclock_now_us();
    kprintf("  timing: %lu ns per kmalloc(64)+kfree pair (%s)\n", (unsigned long)((f1 - f0) * 1000 / FAST),
#ifdef LUMEN_DEBUG
            "debug build, with red zones and poisoning"
#else
            "release build"
#endif
    );

    // --- no leaks: the same number of live allocations and frames as at the start ---
    KheapStats hs1 = kheap_stats();
    PmmStats pm1 = pmm_stats();
    KTEST_CHECK(hs1.live_objects == hs0.live_objects);
    KTEST_CHECK(hs1.large_pages == hs0.large_pages);
    KTEST_CHECK(pm1.used_frames - pm0.used_frames == hs1.slab_pages - hs0.slab_pages);
    kprintf("  leaks: 0 (live allocations %lu before and after; %lu slab page(s) kept for reuse)\n",
            (unsigned long)hs1.live_objects, (unsigned long)(hs1.slab_pages - hs0.slab_pages));
    return 0;
}
