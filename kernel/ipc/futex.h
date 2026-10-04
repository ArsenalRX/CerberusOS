// Futex (SPEC phase 11): lets programs build locks that need the kernel only
// when they must wait.
//
//   futex_wait(addr, val, timeout): if the 32-bit word at addr still holds
//       val, sleep until futex_wake(addr) or the timeout; otherwise return
//       at once (Again).
//   futex_wake(addr, count): wake up to `count` threads waiting on addr.
//
// A word in shared memory (ipc/shm.h) is the same futex in every process
// that maps it; any other word is private to its address space.
#pragma once

#include <lib/result.h>
#include <lib/types.h>

constexpr u64 FUTEX_FOREVER = ~0ull;

// Errors: Fault (bad address), Invalid (unaligned), Again (the word no
// longer holds `val`), Timeout, Interrupted.
Result<void> futex_wait(vaddr_t addr, u32 val, u64 timeout_ms);
// Returns how many threads were woken. Errors: Fault, Invalid.
Result<u32> futex_wake(vaddr_t addr, u32 count);
