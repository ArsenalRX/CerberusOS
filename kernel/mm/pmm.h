// Physical memory manager: a bitmap over every 4 KiB frame below the top of
// RAM, built from the bootloader memory map. Usable regions are free;
// everything else (kernel image, bootloader structures, framebuffer, ACPI,
// holes, the bitmap itself) is reserved. Allocation is first-fit over the
// bitmap; contiguous runs are supported. Frames are returned as physical
// addresses; use hhdm_virt() to touch them.
//
// One spinlock protects the bitmap; all functions are safe on any CPU and
// from interrupt context. None of them sleep.
#pragma once

#include <lib/types.h>

struct PmmStats {
    u64 total_frames;       // frames covered by the bitmap
    u64 usable_frames;      // frames that were free at boot
    u64 used_frames;        // usable frames currently allocated
    u64 free_frames;
    u64 reserved_frames;    // never allocatable
    u64 largest_free_run;   // in frames (computed on demand)
};

constexpr paddr_t PMM_NO_MEMORY = 0;

// Builds the bitmap. Requires boot_info_collect. Panics if no usable region
// can hold the bitmap.
void pmm_init();

// Allocates `count` physically contiguous frames. Returns PMM_NO_MEMORY on
// failure. The memory is not zeroed.
paddr_t pmm_alloc(usize count);
// As pmm_alloc, but zero-filled through the HHDM.
paddr_t pmm_alloc_zeroed(usize count);
// Starts the thread that keeps a pool of zeroed frames for
// pmm_alloc_zeroed(1). Needs the scheduler.
void pmm_start_zeroing();
// Frames in that pool now (for `heapstat`/bench).
u32 pmm_zeroed_ready();
// Stops the refills and returns the pool to the bitmap (true), or lets it
// run again (false): for tests that audit the bitmap.
void pmm_zeroing_pause(bool pause);
static inline paddr_t pmm_alloc_page() { return pmm_alloc(1); }
// Frees frames previously returned by pmm_alloc. Double frees panic in
// debug builds.
void pmm_free(paddr_t addr, usize count);

PmmStats pmm_stats();
// Debug aid for tests: a checksum of the whole bitmap.
u64 pmm_bitmap_checksum();
