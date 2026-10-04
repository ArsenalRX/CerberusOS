// The only way kernel code may touch user memory (SPEC §19.3).
//
// Every function checks that the whole user range lies in the user half and
// does not wrap, then copies with a fault fixup: if the user address is
// unmapped or lacks permission, the copy stops and the function returns
// Error::Fault instead of crashing the kernel. Demand-paged and
// copy-on-write pages are resolved transparently on the way.
//
// With SMAP the CPU itself forbids ring 0 from touching user pages; these
// routines open that window (stac) for exactly the copy instruction and
// close it again (clac), including on the fault path. A kernel access to
// user memory from anywhere else is a kernel bug and is reported as one.
//
// Copy once, then validate the copy: never read the same user bytes twice
// and assume they are unchanged.
//
// Callable from thread context only (a fault may allocate). Not from
// interrupt handlers.
#pragma once

#include <arch/x86_64/interrupts.h>
#include <lib/result.h>
#include <lib/types.h>

// Copies n bytes from user address `src` into the kernel buffer `dst`.
[[nodiscard]] Result<void> copy_from_user(void* dst, vaddr_t src, usize n);
// Copies n bytes from the kernel buffer `src` to user address `dst`.
[[nodiscard]] Result<void> copy_to_user(vaddr_t dst, const void* src, usize n);
// Copies a NUL-terminated string of at most max-1 characters plus the NUL.
// Returns its length (without the NUL); Error::Fault on a bad address,
// Error::TooBig if no NUL was found within max bytes.
[[nodiscard]] Result<usize> strncpy_from_user(char* dst, vaddr_t src, usize max);
// Writes n zero bytes to user memory.
[[nodiscard]] Result<void> clear_user(vaddr_t dst, usize n);

// True if rip is one of the instructions that are allowed to fault on user
// memory. The page-fault handler resolves or fixes up faults only for these.
bool usercopy_is_access(u64 rip);
// Redirects a faulting user-copy instruction to its error return. Returns
// false if the frame is not at one.
bool usercopy_fixup(InterruptFrame* frame, vaddr_t addr, u64 error);

struct UsercopyFault {
    vaddr_t addr;       // CR2 of the last failed copy
    u64 error;          // page-fault error code
};
// Details of the most recent failed copy (for tests and diagnostics).
UsercopyFault usercopy_last_fault();
