// Phase 3 acceptance: 10,000 frames allocated in random-sized chunks, freed
// in a different random order, bitmap and stats back to the initial state;
// then all of memory allocated and freed.
#include <kernel/ktest.h>
#include <lib/kprintf.h>
#include <mm/early_map.h>
#include <mm/pmm.h>

namespace {

u64 g_rng = 0x9E3779B97F4A7C15ull;
u64 rng() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

constexpr usize N = 10000;
struct Chunk {
    paddr_t addr;
    u32 count;
};
Chunk g_chunks[N];

} // namespace

int ktest_pmm(int, char**) {
    // The pre-zeroed pool fills itself in the background; this test checks
    // the bitmap to the frame, so the pool is stopped and emptied first.
    pmm_zeroing_pause(true);
    PmmStats before = pmm_stats();
    u64 sum_before = pmm_bitmap_checksum();
    kprintf("  before: %lu free, %lu used, largest run %lu\n", (unsigned long)before.free_frames,
            (unsigned long)before.used_frames, (unsigned long)before.largest_free_run);

    // Allocate N chunks of 1..4 frames in a random order of sizes.
    u64 frames = 0;
    for (usize i = 0; i < N; i++) {
        u32 count = (u32)(rng() % 4) + 1;
        paddr_t p = pmm_alloc(count);
        KTEST_CHECK(p != PMM_NO_MEMORY);
        KTEST_CHECK(p % PAGE_SIZE == 0);
        g_chunks[i] = {p, count};
        frames += count;
    }
    PmmStats mid = pmm_stats();
    KTEST_CHECK(mid.used_frames == before.used_frames + frames);
    kprintf("  allocated %lu chunks, %lu frames\n", (unsigned long)N, (unsigned long)frames);

    // Shuffle (Fisher-Yates) and free in that order.
    for (usize i = N - 1; i > 0; i--) {
        usize j = (usize)(rng() % (i + 1));
        Chunk t = g_chunks[i];
        g_chunks[i] = g_chunks[j];
        g_chunks[j] = t;
    }
    for (usize i = 0; i < N; i++) pmm_free(g_chunks[i].addr, g_chunks[i].count);

    PmmStats after = pmm_stats();
    KTEST_CHECK(after.used_frames == before.used_frames);
    KTEST_CHECK(after.free_frames == before.free_frames);
    KTEST_CHECK(after.largest_free_run == before.largest_free_run);
    KTEST_CHECK(pmm_bitmap_checksum() == sum_before);
    kprintf("  freed in shuffled order: bitmap checksum and stats match\n");

    // Allocate every frame, one at a time, recording each in a list that is
    // itself carved out of the allocator first; then free everything.
    u64 list_frames = align_up(before.free_frames * 4, PAGE_SIZE) / PAGE_SIZE;
    paddr_t list_phys = pmm_alloc(list_frames);
    KTEST_CHECK(list_phys != PMM_NO_MEMORY);
    u32* list = (u32*)hhdm_virt(list_phys);
    u64 got = 0;
    for (;;) {
        paddr_t p = pmm_alloc(1);
        if (p == PMM_NO_MEMORY) break;
        list[got++] = (u32)(p / PAGE_SIZE);
    }
    KTEST_CHECK(got + list_frames == before.free_frames);
    KTEST_CHECK(pmm_stats().free_frames == 0);
    KTEST_CHECK(pmm_alloc(1) == PMM_NO_MEMORY);
    kprintf("  allocated all %lu free frames (%lu MiB); allocator correctly reports exhaustion\n",
            (unsigned long)(got + list_frames), (unsigned long)((got + list_frames) * PAGE_SIZE / MIB));
    for (u64 i = 0; i < got; i++) pmm_free((paddr_t)list[i] * PAGE_SIZE, 1);
    pmm_free(list_phys, list_frames);

    PmmStats end = pmm_stats();
    KTEST_CHECK(end.free_frames == before.free_frames);
    KTEST_CHECK(end.largest_free_run == before.largest_free_run);
    KTEST_CHECK(pmm_bitmap_checksum() == sum_before);
    kprintf("  freed everything: bitmap checksum and stats match\n");
    pmm_zeroing_pause(false);
    return 0;
}
