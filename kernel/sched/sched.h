// Threads, processes and the scheduler (SPEC phase 6 and §5A).
//
// Scheduling: four priority levels with one FIFO run queue each; the highest
// non-empty level runs, round-robin within a level, one timer tick (10 ms)
// per slice. A thread that uses its whole slice drops one level (down to
// LOW); a thread that blocks returns to its base level; once a second every
// thread returns to its base level so nothing starves. A thread made ready
// at a higher level than the running one preempts it at once.
//
// Every thread runs on its own kernel stack with a guard page below it.
//
// Concurrency: all scheduler state is protected by disabling interrupts
// (one CPU until phase 8, which adds per-CPU run queues and a lock). The
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

// A process: an address space and the threads that run in it. The file
// descriptor table and working directory are added with the VFS (phase 9).
struct Process {
    u32 pid;
    char name[32];
    AddressSpace* space;
    Thread* threads;            // linked through Thread::proc_next
    u32 thread_count;
    Process* parent;
    Process* next;              // all processes
    int exit_status;
    Credentials cred;
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
    u32 cpu_affinity;           // bit per CPU; all ones until phase 8 uses it
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
};

struct SchedStats {
    u64 ticks;                  // timer ticks since the scheduler started
    u64 idle_ticks;             // of those, spent in the idle thread
    u64 context_switches;
    u32 threads;
};

// Starts scheduling: creates the kernel process, the idle and reaper threads
// and a first thread running init(arg), then switches to it. Never returns;
// the stack it was called on is abandoned. Requires the heap, the VMM and a
// running periodic timer.
[[noreturn]] void sched_start(void (*init)(void*), void* arg);
bool sched_running();

Thread* thread_current();
Process* process_kernel();

// Creates a thread in `process` (the kernel process if null) and makes it
// runnable. Errors: NoMemory, Invalid (bad priority).
Result<Thread*> kthread_create(void (*fn)(void*), void* arg, const char* name, u8 priority = prio::NORMAL,
                               Process* process = nullptr);
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

// A new process with its own empty user address space. Error: NoMemory.
Result<Process*> process_create(const char* name);
// Frees a process and its address space. It must have no threads left.
void process_destroy(Process* p);

// For synchronisation primitives: blocks the calling thread on `wq`.
// Interrupts must already be disabled; returns with them still disabled.
void sched_block_locked(WaitQueue& wq);

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
// Prints one line per thread (the `ps` shell command).
void sched_print_threads();
