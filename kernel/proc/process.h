// User processes: loading a program, entering ring 3, fork, execve, exit,
// wait, and what happens when a user program faults (SPEC phase 7).
//
// Address-space layout of a user process (all randomised per exec, from the
// kernel CSPRNG):
//   program image   somewhere in [PIE_BASE_MIN, +1 TiB)
//   mmap region     searched upward from a base in [MMAP_BASE_MIN, +1 TiB)
//   stack           8 MiB, demand-paged, guard page below, top within 16 GiB
//                   below STACK_TOP_MAX
// Programs must be position-independent; nothing is ever mapped writable and
// executable.
//
// Everything here runs in thread context.
#pragma once

#include <arch/x86_64/interrupts.h>
#include <lib/result.h>
#include <lib/types.h>
#include <sched/sched.h>

constexpr vaddr_t PIE_BASE_MIN = 0x0000100000000000ull;
constexpr vaddr_t MMAP_BASE_MIN = 0x0000400000000000ull;
constexpr vaddr_t STACK_TOP_MAX = 0x00007FFFFFFFF000ull;
constexpr usize USER_STACK_SIZE = 8 * MIB;

constexpr usize EXEC_MAX_STRINGS = 64;          // argv and envp entries, each
constexpr usize EXEC_MAX_ARG_BYTES = 32 * KIB;  // all strings together
constexpr usize PATH_MAX = 256;
constexpr usize EXEC_MAX_FILE = 64 * MIB;      // largest program file execve will load

// Wait-status encoding (as returned through waitpid): a normal exit stores
// the 8-bit code in bits 8-15; a process killed by a fault stores the signal
// number in the low 7 bits.
constexpr int WAIT_SIGSEGV = 11, WAIT_SIGILL = 4, WAIT_SIGFPE = 8;
inline int wait_status_exited(int code) { return (code & 0xFF) << 8; }

// Argument and environment strings for a new program image, held in kernel
// memory while the address space is replaced.
struct ExecArgs {
    char* strings;                  // EXEC_MAX_ARG_BYTES buffer of NUL-terminated strings
    usize used;
    u32 argc, envc;
    u32 offsets[2 * EXEC_MAX_STRINGS];   // argv offsets, then envp offsets
};
// Allocates the string buffer. Error: NoMemory.
Result<void> exec_args_init(ExecArgs* a);
void exec_args_free(ExecArgs* a);
// Appends one string to argv (env = false) or envp. Errors: TooBig.
Result<void> exec_args_add(ExecArgs* a, const char* s, bool env);

// Finds the boot archive and installs the system-call entry point. Call once
// after the scheduler's prerequisites (heap, GDT, per-CPU data) are up.
void process_init();

// Starts a new process running `path` from the boot archive, as a child of
// the kernel. With auto_reap the process frees itself when it exits;
// otherwise the caller must collect it with process_wait. The program is
// loaded by the new process's own thread, so a load failure shows up as
// exit code 127. Errors: NoMemory, TooBig.
// The new process starts in the kernel's working directory (the shell's
// `cd`). Standard output goes to `out` when given (shell redirection),
// otherwise to the console like standard input and error.
// `cred` (default: root) is the identity the program runs as.
Result<Process*> process_spawn(const char* path, const char* const argv[], bool auto_reap, File* out = nullptr,
                               const Credentials* cred = nullptr);
// Blocks until `child` (a child of the kernel) exits, frees it, and returns
// its wait status.
int process_wait(Process* child);

// The calling process ends with the given wait status: its other threads
// are stopped first, then its address space and files are released; its
// parent collects the status.
[[noreturn]] void process_exit(int wait_status);

// The calling thread ends. If it is the last user thread of its process the
// process ends with exit code `code`.
[[noreturn]] void process_thread_exit(int code);
// A new thread in the calling process, starting in user mode at `entry` with
// `arg` in rdi, `stack` as its stack pointer and `tls` as its FS base.
// Returns its id (for thread_join_user). Errors: NoMemory, Again (too many
// threads, or the process is ending).
Result<u32> process_thread_spawn(vaddr_t entry, u64 arg, vaddr_t stack, u64 tls);
constexpr u32 PROCESS_MAX_THREADS = 256;
// True if a child of the calling process has exited and not been collected.
bool process_has_exited_child();

// fork: a copy of the calling process that resumes from the same system-call
// frame with 0 in rax. Only the calling thread exists in the copy. Returns
// the child's pid. Error: NoMemory.
Result<i64> process_fork(const InterruptFrame* frame);
// execve: replaces the calling process's program. On success the frame is
// rewritten to start the new image and the old address space is gone; on
// failure nothing has changed. Errors: NotFound, IsDir, Perm, NotExecutable,
// NoMemory.
Result<void> process_exec(InterruptFrame* frame, const char* path, const ExecArgs& args);
// waitpid: collects an exited child (pid > 0: that child; -1: any). Returns
// its pid and stores its wait status, or returns 0 at once with nohang when
// none has exited. Errors: NoChild, Invalid, Interrupted.
Result<i64> process_waitpid(i64 pid, int* status, bool nohang);

// A CPU exception raised by user code, or a user-mode page fault that could
// not be resolved. If the process handles the matching signal (SIGSEGV,
// SIGILL, SIGFPE) the frame is rewritten to run its handler and the call
// returns; otherwise the process is killed and the call does not return.
void user_exception(InterruptFrame* frame);

// Defined in usermode.asm.
extern "C" [[noreturn]] void enter_user(const InterruptFrame* frame);
