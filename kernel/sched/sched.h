// Threads, processes and the scheduler (SPEC phases 6 and 8, and §5A).
//
// Scheduling: every CPU has four priority levels with one FIFO run queue
// each; the highest non-empty level runs, round-robin within a level, one
// timer tick (10 ms) per slice. A thread that uses its whole slice drops one
// level (down to LOW); a thread that blocks returns to its base level; once
// a second every thread returns to its base level so nothing starves. A
// thread made ready at a higher level than the one running on its CPU
// preempts it at once.
//
// Placement: a thread that becomes runnable goes to the CPU it last ran on
// if that CPU is idle, otherwise to any idle CPU it may run on, otherwise
// back to its last CPU. A CPU whose queues are empty takes a waiting thread
// from another CPU's queue (work stealing) before it idles.
//
// Every thread runs on its own kernel stack with a guard page below it.
//
// Concurrency: one spinlock, the scheduler lock, protects the run queues,
// wait queues, the sleep list and the thread and process lists (including
// the parent/child tree). It is held across the switch from one thread to
// the next and released by the thread that resumes, which is what makes
// "put myself on a wait queue and stop running" one step: nobody can wake a
// thread and run it elsewhere while it is still on its old stack. The
// functions that block (wait, sleep, join, yield) must be called from thread
// context with interrupts enabled; the wake functions and sched_tick are
// safe from interrupt context.
#pragma once

#include <lib/result.h>
#include <lib/types.h>

class AddressSpace;
struct Thread;
struct Process;

namespace prio {
constexpr u8 INTERACTIVE = 0;   // the compositor and threads serving input
constexpr u8 HIGH = 1;
constexpr u8 NORMAL = 2;
constexpr u8 LOW = 3;
constexpr u8 COUNT = 4;
} // namespace prio

enum class ThreadState : u8 { Ready, Running, Blocked, Sleeping, Zombie };

constexpr usize THREAD_STACK_SIZE = 32 * KIB;
constexpr u32 SCHED_TICK_MS = 10;

// A list of threads waiting for something to happen. Zero-initialised is
// empty, so it can be a plain global or a member.
struct WaitQueue {
    Thread* head;
    Thread* tail;

    // Blocks the calling thread until wake_one/wake_all picks it.
    void wait();
    // As wait, but gives up after `ticks` timer ticks. Returns false on
    // timeout.
    bool wait_ticks(u64 ticks);
    // Makes the longest-waiting thread runnable. Returns false if nobody was
    // waiting. Interrupt-safe.
    bool wake_one();
    // Makes every waiting thread runnable. Interrupt-safe.
    void wake_all();
    bool empty() const { return head == nullptr; }
};

struct Credentials {
    u32 uid;
    u32 gid;
};

struct File;
struct Vnode;
struct ShmMapping;
constexpr usize PROCESS_MAX_FDS = 32;

// A process: an address space, the threads that run in it, its open files
// and its place in the parent/child tree. The working directory is added
// with the VFS (phase 9). Lifetime and the user-mode side are in
// proc/process.h.
struct Process {
    u32 pid;
    char name[32];
    AddressSpace* space;
    Thread* threads;            // linked through Thread::proc_next
    u32 thread_count;
    Process* parent;
    Process* next;              // all processes
    Process* children;          // linked through sibling
    Process* sibling;
    bool zombie;                // exited; waiting for the parent to collect exit_status
    bool auto_reap;             // nobody will wait: free it as soon as it exits
    int exit_status;            // wait status: (code << 8) for exit, signal number if killed
    Credentials cred;
    File* files[PROCESS_MAX_FDS];
    u32 fd_cloexec;             // bit per descriptor: close it on execve
    Vnode* cwd;                 // working directory (a reference); null = the root
    u32 umask;                  // permission bits removed from new files
    WaitQueue child_wait;       // woken when a child of this process exits
    vaddr_t mmap_hint;          // randomised base for the process's mmap region
    // Threads and exit (phase 11).
    u32 live_threads;           // user threads that have not exited yet
    bool exiting;               // one thread is ending the process; the others must leave
    WaitQueue exit_wait;        // the ending thread waits here for the others
    // Signals (proc/signal.h).
    u64 sig_pending;            // bit per signal number
    u64 sig_handlers[32];       // 0 = default, 1 = ignore, else the handler's address
    vaddr_t sig_trampoline;     // code in the process that calls sigreturn
    ShmMapping* shm_maps;       // shared memory mapped by this process (ipc/shm.h)
};

struct Thread {
    u64 rsp;                    // saved stack pointer; must stay first (switch.asm)
    u32 id;
    char name[24];
    ThreadState state;
    u8 base_priority;
    u8 priority;                // current level: base, or lower after using full slices
    u8 slice;                   // ticks left in the current slice
    bool detached;
    bool timed_out;
    bool is_idle;
    bool user;                  // a user thread counted in process->live_threads
    bool joining;               // a thread_join is waiting for this thread
    bool interrupt_pending;     // a signal or the end of the process is waiting: see thread_interrupt_locked
    u64 sig_mask;               // signals this thread does not take (bit per number); SIGKILL never counts
    bool interruptible;         // blocked in a wait that an interruption may end
    bool interrupted;           // the last interruptible wait was ended that way
    bool iret_return;           // leave the current system call through iretq (all registers restored)
    u64 fs_base;                // the thread's TLS pointer (FS base) in user mode
    u32 cpu_affinity;           // bit per CPU the thread may run on
    u32 cpu;                    // the CPU it is running on, queued on, or last ran on
    Process* process;
    AddressSpace* space;        // address space active while this thread runs
    vaddr_t stack_top;
    Thread* next;               // run queue or wait queue link
    Thread* sleep_next;         // sleep list link
    Thread* all_next;
    Thread* proc_next;
    WaitQueue* waiting_on;
    u64 wake_tick;              // 0 = not on the sleep list
    int exit_code;
    WaitQueue joiners;
    u64 run_ticks;              // timer ticks spent running
    u64 switches;               // times switched in
    u64 preemptions;            // times switched out involuntarily
    void (*entry)(void*);
    void* arg;
    u8* fpu;                    // FXSAVE area; null for threads that never run user code
};

struct SchedStats {
    u64 ticks;                  // timer ticks since the scheduler started, added over all CPUs
    u64 idle_ticks;             // of those, spent in an idle thread
    u64 context_switches;
    u64 steals;                 // threads taken from another CPU's queue
    u32 threads;
    u32 processes;
    u32 cpus;                   // CPUs scheduling
};

// Starts scheduling: creates the kernel process, the idle and reaper threads
// and a first thread running init(arg), then switches to it. Never returns;
// the stack it was called on is abandoned. Requires the heap, the VMM and a
// running periodic timer.
[[noreturn]] void sched_start(void (*init)(void*), void* arg);
bool sched_running();
// Another CPU joins in (called by smp_ap_main once sched_start has run).
// Never returns; the stack it was called on is abandoned.
[[noreturn]] void sched_enter_ap();

Thread* thread_current();
Process* process_kernel();

// Creates a thread in `process` (the kernel process if null) and makes it
// runnable. With `detached` the thread is freed by the reaper when it ends
// and the returned pointer must not be used afterwards: the thread may
// already have run and exited. Errors: NoMemory, Invalid (bad priority).
Result<Thread*> kthread_create(void (*fn)(void*), void* arg, const char* name, u8 priority = prio::NORMAL,
                               Process* process = nullptr, bool detached = false);
// Ends the calling thread. Its stack is freed by thread_join, or by the
// reaper if it was detached.
[[noreturn]] void thread_exit(int code);
// Waits for a thread to end, frees it and returns its exit code. Exactly one
// join per non-detached thread.
int thread_join(Thread* t);
// Lets the reaper free the thread when it ends; it must not be joined.
void thread_detach(Thread* t);
// Gives up the rest of the slice to other runnable threads of the same or
// higher level.
void thread_yield();
// Sleeps at least `ms` milliseconds (rounded up to whole ticks).
void thread_sleep_ms(u64 ms);
// Sleeps until the `ticks`-th next timer tick (ticks >= 1).
void thread_sleep_ticks(u64 ticks);
// Restricts the calling thread to the CPUs in `mask` (bit n = CPU n) and
// moves it at once if it is on one it may no longer use. A mask naming no
// running CPU means "any".
void thread_set_affinity(u32 mask);
// The CPU the caller is running on now. Unless the thread is pinned to one
// CPU, it may be on another by the time the value is used.
u32 thread_cpu();

// Gives the calling thread an FPU/SSE save area, which it needs before it
// first runs user code. `initial` is a 512-byte FXSAVE image to start from
// (fork copies the parent's), or null for the clean power-on state.
// Error: NoMemory.
Result<void> thread_enable_fpu(const u8* initial);
// Writes the calling thread's live FPU/SSE registers into `out` (512 bytes).
// The thread must have an FPU area.
void thread_snapshot_fpu(u8* out);
// Moves a thread to another process's thread list (used when a process
// exits before its last thread has been freed).
void thread_set_process(Thread* t, Process* p);

// Puts the calling thread's FPU/SSE registers back to the clean power-on
// state (a program that replaces itself with execve starts clean).
void thread_reset_fpu();

// A new process. With `space` null it gets its own empty user address
// space; otherwise it takes ownership of the one given (fork passes a
// copy-on-write clone). Error: NoMemory.
Result<Process*> process_create(const char* name, AddressSpace* space = nullptr);
// Hands every thread of `p` except `keep` to the kernel process, to be freed
// by the reaper once it has ended. For a process that is ending: its other
// threads have already left user mode for good.
void process_release_threads(Process* p, Thread* keep);
// Scheduler lock held: the process with this pid, or null.
Process* process_find_locked(u32 pid);
// A copy of the process table for display (System Monitor): at most `max`
// rows, returns the count. Thread context.
struct ProcessInfo {
    u32 pid;
    char name[32];
    u32 threads;
    u64 run_ticks;              // of all its threads, ever
    bool zombie;
};
u32 sched_process_snapshot(ProcessInfo* out, u32 max);
// Frees a process and its address space. It must have no threads left.
void process_destroy(Process* p);

// For synchronisation primitives and the process tree, whose state the
// scheduler lock protects. sched_lock disables interrupts, takes the lock and
// returns the previous interrupt state for sched_unlock, which also switches
// threads first if a wake-up made under the lock calls for it.
u64 sched_lock();
void sched_unlock(u64 saved);
// With the lock held: blocks the calling thread on `wq`. Returns with the
// lock held again, possibly on another CPU.
void sched_block_locked(WaitQueue& wq);

// How an interruptible wait ended.
enum class WaitResult : u8 { Woken, Timeout, Interrupted };
// With the lock held: as sched_block_locked, but the wait also ends when the
// thread is interrupted (a signal arrives or its process is ending), and
// does not start at all if an interruption is already pending. `wq` may be
// null (a plain sleep); ticks 0 = no timeout. Long waits made on behalf of
// a user program use this, so the program can always be stopped.
WaitResult sched_block_interruptible_locked(WaitQueue* wq, u64 ticks);
// With the lock held: marks `t` interrupted. If it is in an interruptible
// wait it is woken; if it is running on another CPU that CPU is told, so
// the thread reaches its return-to-user check soon.
void thread_interrupt_locked(Thread* t);
// Ends an interruptible sleep of the calling thread early or after `ticks`.
WaitResult thread_sleep_interruptible(u64 ticks);
// User thread support: joins thread `tid` of the calling process (frees it,
// returns its exit code). Errors: NoProcess (no such thread), Invalid
// (itself, or already being joined), Interrupted.
Result<int> thread_join_user(u32 tid);
// Loads a 512-byte FXSAVE image as the calling thread's FPU state.
void thread_restore_fpu(const u8* image);
// Sets the calling thread's user TLS pointer (FS base).
void thread_set_fs_base(u64 base);
// As sched_block_locked, but wakes after `ticks` timer ticks at the latest;
// false if it was the timeout.
bool sched_block_locked_ticks(WaitQueue& wq, u64 ticks);
// With the lock held: the wake functions.
bool sched_wake_one_locked(WaitQueue& wq);
void sched_wake_all_locked(WaitQueue& wq);

// Timer interrupt entry (installed as the LAPIC tick hook by sched_start).
void sched_tick();
// Bracket every device interrupt handler; the exit performs the preemption
// decided inside the handler.
void sched_irq_enter();
void sched_irq_exit();

// Idle policy: halt (default) or spin. Spinning is a workaround for
// hypervisors that stop delivering the timer to a halted CPU.
void sched_set_idle_spin(bool spin);
bool sched_idle_spin();

u64 sched_ticks();
SchedStats sched_stats();
// Timer ticks and idle ticks of one CPU (0 for a CPU that is not running).
void sched_cpu_ticks(u32 cpu, u64* ticks, u64* idle_ticks);
// Prints one line per thread (the `ps` shell command).
void sched_print_threads();
