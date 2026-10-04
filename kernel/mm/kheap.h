// Kernel heap (SPEC phase 5 and §5A). Small requests (up to 2048 bytes) come
// from slab caches of fixed-size objects; each slab is one page reached
// through the direct map. Larger requests get whole pages: physically
// contiguous through the direct map when the frame allocator can supply
// them (no extra page tables, 2 MiB TLB entries), otherwise a virtually
// contiguous mapping from the VMM.
//
// Protections:
//   - Free-list pointers inside free objects are stored XORed with a
//     per-boot secret and their own address, and every pointer taken off a
//     free list is checked to lie inside its slab; a corrupted list panics
//     instead of handing out an attacker-chosen address.
//   - Debug builds add a red zone on both sides of every object, fill new
//     memory with 0xDE and freed memory with 0xEF, record the call site of
//     every allocation, and detect double frees, overruns (on free) and
//     writes after free (on reuse).
//
// Returned memory is 16-byte aligned (page-aligned for large requests) and
// is NOT zeroed by kmalloc: use kzalloc for anything that may be copied to
// user space (SPEC §19.3).
//
// Concurrency: safe on any number of CPUs. Each CPU allocates from slabs of
// its own without taking a lock; the shared lists behind them have one lock
// (kheap.cpp describes the scheme). Slab-sized requests are safe from
// interrupt context; large requests are not (they may call into the VMM).
// Nothing here sleeps.
#pragma once

#include <lib/types.h>

constexpr usize KMALLOC_MAX_SLAB = 2048;

// Bytes between the start of a slab object and the pointer kmalloc returns
// (the debug header and front red zone). Tests use it to reach the free-list
// link of a freed object.
#ifdef CERBERUS_DEBUG
constexpr usize KHEAP_PAYLOAD_OFFSET = 32;
#else
constexpr usize KHEAP_PAYLOAD_OFFSET = 0;
#endif

// Sets up the caches and the per-boot secret. Requires pmm_init and vmm_init.
void kheap_init();

// Allocates `size` bytes. Returns nullptr if size is 0 or memory is exhausted.
[[nodiscard]] void* kmalloc(usize size);
// As kmalloc, zero-filled.
[[nodiscard]] void* kzalloc(usize size);
// Frees memory from kmalloc/kzalloc/krealloc. nullptr is ignored. Panics on
// a pointer the heap did not hand out, a double free, or (debug) a damaged
// red zone.
void kfree(void* ptr);
// Resizes an allocation, preserving its contents up to the smaller size.
// krealloc(nullptr, n) allocates; krealloc(p, 0) frees and returns nullptr.
// On failure returns nullptr and leaves the original allocation intact.
[[nodiscard]] void* krealloc(void* ptr, usize size);
// Overwrites `size` bytes with zeroes, then frees. Mandatory for keys,
// passwords and buffers that held user data.
void kfree_sensitive(void* ptr, usize size);

// Bytes actually usable behind a pointer (the size class, or whole pages).
usize ksize(const void* ptr);
// True if the allocation's bookkeeping (and, in debug builds, its red zones)
// is intact. Never panics; for tests and diagnostics.
bool kheap_check(const void* ptr);

struct KheapStats {
    u64 slab_pages;         // pages currently held by slab caches
    u64 large_pages;        // pages currently held by large allocations
    u64 live_objects;       // outstanding allocations, small and large
    u64 total_allocs;       // since boot
    u64 total_frees;
};
KheapStats kheap_stats();

// Prints per-size-class usage and, in debug builds, outstanding allocations
// grouped by the call site that made them (the `heapstat` shell command).
void kheap_report();
