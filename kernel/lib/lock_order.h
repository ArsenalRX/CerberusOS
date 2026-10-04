// The order in which spinlocks may be taken (SPEC §5A phase 8). This is the
// one place the order is written down.
//
// Every Spinlock carries a rank. A CPU may only take a lock whose rank is
// higher than every lock it already holds; debug builds check this on every
// acquisition and panic on a violation, so a possible deadlock is reported
// the first time the offending path runs, not the day two CPUs happen to
// meet in it. Rank 0 means "not ranked": such a lock is never checked and
// must be a leaf (nothing else is taken while it is held).
//
// Why this order:
//   - A user address space is edited first; that may allocate bookkeeping
//     from the heap and frames from the frame allocator, and must flush
//     other CPUs' TLBs before it lets go.
//   - The heap takes frames, so it comes before the frame allocator. It
//     never calls the VMM while holding its own lock.
//   - The scheduler lock is taken by wake-ups from almost anywhere, so it is
//     near the end; code holding it calls nothing but the console.
//   - The console is last: anything may print.
#pragma once

#include <lib/types.h>

namespace lock_rank {
constexpr u8 UNRANKED = 0;
constexpr u8 VMM_USER = 10;     // a user AddressSpace (page tables and VMA list)
constexpr u8 VMM_KERNEL = 20;   // the kernel AddressSpace
constexpr u8 VMM_FRAMES = 30;   // reference counts of shared (copy-on-write) frames
constexpr u8 HEAP = 40;         // shared slab lists and the large-allocation list
constexpr u8 PMM = 50;          // the frame bitmap
constexpr u8 TLB = 60;          // one TLB shootdown at a time
constexpr u8 SCHED = 70;        // run queues, wait queues, sleep list, thread and process lists
constexpr u8 CSPRNG = 80;
constexpr u8 CONSOLE = 90;      // serial port, framebuffer console, terminal cells
} // namespace lock_rank
