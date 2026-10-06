// Signals (SPEC phase 11): a minimal set with user handlers.
//
//   SIGKILL  always ends the process       SIGTERM  ends it unless handled
//   SIGSEGV  a fault; ends it unless handled (SIGILL and SIGFPE likewise)
//   SIGCHLD  a child exited; ignored unless handled
//   SIGUSR1, SIGUSR2  for programs; end the process unless handled
//
// A signal is posted to a process and taken by whichever of its threads
// next returns to user mode (the return-to-user check, user_return). A
// handler runs on the thread's own stack: the kernel saves the interrupted
// registers and FPU state there, the handler returns into a small
// trampoline the kernel mapped into the process, and the trampoline's
// sigreturn call restores them. A system call that was waiting when the
// signal arrived returns EINTR.
//
// The same check is how a process ends with several threads: the thread
// that ends it marks the process as exiting and interrupts the others,
// which leave at their next return to user mode.
#pragma once

#include <arch/x86_64/interrupts.h>
#include <lib/result.h>
#include <lib/types.h>
#include <sched/sched.h>

namespace sig {
constexpr int ILL = 4, FPE = 8, KILL = 9, USR1 = 10, SEGV = 11, USR2 = 12, TERM = 15, CHLD = 17;
constexpr int MAX = 32;
constexpr u64 DFL = 0, IGN = 1;
} // namespace sig

// kill(): posts `signo` to process `pid` (0 only checks that the caller
// may). Errors: NoProcess, Perm (another user's process), Invalid.
Result<void> signal_send(u32 pid, int signo, const Credentials& sender);
// Scheduler lock held: posts a signal and interrupts the process's threads.
// Signals that would be ignored are dropped here.
void signal_post_locked(Process* p, int signo);
// signal(): installs a handler (sig::DFL, sig::IGN or an address) and
// returns the previous one. SIGKILL cannot be changed. Error: Invalid.
Result<u64> signal_set_handler(int signo, u64 handler);
// sigreturn(): restores the state a handler interrupted. Returns the value
// for rax. A damaged frame ends the process.
i64 signal_return(InterruptFrame* frame);
// A fault in user code: if the process handles `signo`, arranges for the
// handler to run and returns true; otherwise false (the caller ends it).
bool signal_deliver_fault(InterruptFrame* frame, int signo);
// The calling thread's signal mask: returns the old mask; with `apply`,
// changes it (how 0 block, 1 unblock, 2 set). SIGKILL cannot be masked.
Result<u64> signal_set_mask(int how, u64 set, bool apply);
// Maps the sigreturn trampoline into the active (new) address space near
// `hint` and returns its address. Error: NoMemory.
Result<vaddr_t> signal_map_trampoline(AddressSpace* space, vaddr_t hint);

// Called on every return to user mode that came through the kernel (system
// calls and interrupts): if the thread has been interrupted, its process is
// ending or a signal is waiting, acts on it. May not return (the thread or
// the process ends).
void user_return(InterruptFrame* frame);
