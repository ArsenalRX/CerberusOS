// System-call entry from ring 3 (SPEC §7 and §5A phase 7).
//
// Every argument is hostile: numbers are range-checked, flag words reject
// unknown bits, lengths are capped and checked for overflow, and user memory
// is only touched through mm/usercopy.h. Nothing the kernel returns contains
// a kernel address or uninitialised bytes.
#pragma once

#include <arch/x86_64/interrupts.h>
#include <lib/result.h>
#include <lib/types.h>

// errno values (the usual Unix numbers), returned negated in rax.
namespace err {
constexpr i64 PERM = 1, NOENT = 2, SRCH = 3, INTR = 4, IO = 5, TOOBIG = 7, NOEXEC = 8, BADF = 9, CHILD = 10,
              AGAIN = 11, NOMEM = 12, FAULT = 14, BUSY = 16, EXIST = 17, NOTDIR = 20, ISDIR = 21, INVAL = 22,
              MFILE = 24, NOSPC = 28, DEADLK = 35, NOSYS = 38, TIMEDOUT = 110, ACCES = 13, XDEV = 18,
              NODEV = 19, ROFS = 30, NAMETOOLONG = 36, NOTEMPTY = 39, LOOP = 40, SPIPE = 29, PIPE = 32,
              MSGSIZE = 90;
} // namespace err

// The -errno value for a kernel Error.
i64 errno_of(Error e);

// Programs the MSRs behind the `syscall` instruction. Requires the GDT and
// per-CPU data. Once per CPU.
void syscall_init();

// Called from usermode.asm with the saved user state. Stores the result in
// frame->rax and makes the frame safe to return to user mode. Returns
// nonzero if every register in the frame must be restored (iretq), zero if
// the fast return (sysret, which loses rcx and r11) will do.
extern "C" u64 syscall_dispatch(InterruptFrame* frame);
