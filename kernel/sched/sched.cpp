// Scheduler core: run queues, the sleep list, wait queues, thread and process
// lifetime. See sched.h for the policy and the locking rules.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <drivers/lapic.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/vmm.h>
#include <sched/sched.h>

extern "C" {
void context_switch(u64* save_rsp, u64 load_rsp);
void thread_start();
[[noreturn]] void thread_bootstrap(void (*fn)(void*), void* arg);
}

namespace {

constexpr u8 SLICE_TICKS = 1;
constexpr u64 BOOST_INTERVAL_TICKS = 100;   // everyone back to base level once a second
constexpr u64 RFLAGS_IF = 1 << 9;

struct RunQueue {
    Thread* head;
    Thread* tail;
};

// Per-CPU scheduler state. One entry until phase 8 gives each CPU its own.
struct Cpu {
    Thread* current;
    Thread* idle;
    bool need_resched;
    u32 irq_depth;
};

Cpu g_cpu;
RunQueue g_rq[prio::COUNT];
Thread* g_sleepers = nullptr;       // sorted by wake_tick
Thread* g_all = nullptr;
Thread* g_dead = nullptr;           // detached zombies for the reaper (linked through next)
WaitQueue g_reaper_wq;
Process g_kernel_process;
Process* g_processes = nullptr;
Thread g_boot_context;              // what sched_start switches away from; never resumed
bool g_running = false;
bool g_idle_spin = false;
u64 g_ticks = 0;
u64 g_idle_ticks = 0;
u64 g_switches = 0;
u32 g_next_tid = 1;
u32 g_next_pid = 1;
u32 g_thread_count = 0;

// ------------------------------------------------------------- run queues --
void enqueue(Thread* t) {
    RunQueue& q = g_rq[t->priority];
    t->next = nullptr;
    if (q.tail) q.tail->next = t;
    else q.head = t;
    q.tail = t;
}

Thread* dequeue_highest() {
    for (RunQueue& q : g_rq) {
        Thread* t = q.head;
        if (!t) continue;
        q.head = t->next;
        if (!q.head) q.tail = nullptr;
        t->next = nullptr;
        return t;
    }
    return nullptr;
}

// ------------------------------------------------------------- sleep list --
void sleep_insert(Thread* t, u64 wake_tick) {
    t->wake_tick = wake_tick;
    Thread** link = &g_sleepers;
    while (*link && (*link)->wake_tick <= wake_tick) link = &(*link)->sleep_next;
    t->sleep_next = *link;
    *link = t;
}

void sleep_remove(Thread* t) {
    if (!t->wake_tick) return;
    for (Thread** link = &g_sleepers; *link; link = &(*link)->sleep_next) {
        if (*link == t) {
            *link = t->sleep_next;
            break;
        }
    }
    t->sleep_next = nullptr;
    t->wake_tick = 0;
}

// ------------------------------------------------------------ wait queues --
void wq_append(WaitQueue* wq, Thread* t) {
    t->next = nullptr;
    if (wq->tail) wq->tail->next = t;
    else wq->head = t;
    wq->tail = t;
}

Thread* wq_pop(WaitQueue* wq) {
    Thread* t = wq->head;
    if (!t) return nullptr;
    wq->head = t->next;
    if (!wq->head) wq->tail = nullptr;
    t->next = nullptr;
    return t;
}

void wq_remove(WaitQueue* wq, Thread* t) {
    Thread* prev = nullptr;
    for (Thread* i = wq->head; i; prev = i, i = i->next) {
        if (i != t) continue;
        if (prev) prev->next = t->next;
        else wq->head = t->next;
        if (wq->tail == t) wq->tail = prev;
        t->next = nullptr;
        return;
    }
}

// Interrupts off. Puts a blocked or sleeping thread back on a run queue and
// asks for a reschedule if it should run before the current thread.
void make_ready(Thread* t) {
    t->state = ThreadState::Ready;
    t->waiting_on = nullptr;
    enqueue(t);
    Thread* cur = g_cpu.current;
    if (cur->is_idle || t->priority < cur->priority) g_cpu.need_resched = true;
}

// Interrupts off. Picks the next thread and switches to it. Returns when the
// calling thread is next scheduled.
void schedule() {
    Thread* prev = g_cpu.current;
    if (prev->state == ThreadState::Running) {
        prev->state = ThreadState::Ready;
        if (!prev->is_idle) enqueue(prev);
    }
    Thread* next = dequeue_highest();
    if (!next) next = g_cpu.idle;
    g_cpu.need_resched = false;
    next->state = ThreadState::Running;
    next->slice = SLICE_TICKS;
    if (next == prev) return;

    g_cpu.current = next;
    next->switches++;
    g_switches++;
    tss_set_kernel_stack(0, next->stack_top);
    // Each thread carries the address space it was running in; reload CR3
    // only when the two differ.
    prev->space = &vmm_current();
    if (next->space != prev->space) next->space->activate();
    context_switch(&prev->rsp, next->rsp);
}

// After a wake-up made from thread context: switch now if a more urgent
// thread became runnable. `flags` are the caller's saved RFLAGS.
void resched_if_needed(u64 flags) {
    if (!g_running || !g_cpu.need_resched || g_cpu.irq_depth || !(flags & RFLAGS_IF)) return;
    u64 irq = interrupts_save();
    schedule();
    interrupts_restore(irq);
}

void block_current(WaitQueue* wq, u64 wake_tick) {
    Thread* t = g_cpu.current;
    ASSERT_ALWAYS(!t->is_idle && g_cpu.irq_depth == 0);
    t->state = wq ? ThreadState::Blocked : ThreadState::Sleeping;
    t->timed_out = false;
    t->waiting_on = wq;
    if (wq) wq_append(wq, t);
    if (wake_tick) sleep_insert(t, wake_tick);
    t->priority = t->base_priority;     // blocking is what interactive threads do: back to base level
    schedule();
}

// ----------------------------------------------------------------- threads --
void unlink_thread(Thread* t) {
    for (Thread** link = &g_all; *link; link = &(*link)->all_next) {
        if (*link == t) {
            *link = t->all_next;
            break;
        }
    }
    for (Thread** link = &t->process->threads; *link; link = &(*link)->proc_next) {
        if (*link == t) {
            *link = t->proc_next;
            break;
        }
    }
    t->process->thread_count--;
    g_thread_count--;
}

// The thread must be a zombie that is no longer running on any CPU.
void free_thread(Thread* t) {
    u64 irq = interrupts_save();
    unlink_thread(t);
    interrupts_restore(irq);
    vmm_free_kernel_stack(t->stack_top, THREAD_STACK_SIZE);
    kfree(t);
}

Result<Thread*> create_thread(void (*fn)(void*), void* arg, const char* name, u8 priority, Process* process,
                              bool is_idle) {
    if (priority >= prio::COUNT) return Error::Invalid;
    Thread* t = (Thread*)kzalloc(sizeof(Thread));
    if (!t) return Error::NoMemory;
    Result<vaddr_t> stack = vmm_alloc_kernel_stack(THREAD_STACK_SIZE);
    if (!stack.ok()) {
        kfree(t);
        return stack.error();
    }
    if (!process) process = &g_kernel_process;
    t->stack_top = stack.value();
    strlcpy(t->name, name, sizeof t->name);
    t->base_priority = t->priority = priority;
    t->slice = SLICE_TICKS;
    t->is_idle = is_idle;
    t->cpu_affinity = ~0u;
    t->process = process;
    t->space = process->space;
    t->entry = fn;
    t->arg = arg;

    // Initial frame for context_switch: six callee-saved registers, then the
    // return address. The stack top is 16-byte aligned, so thread_start runs
    // with the alignment a call instruction expects.
    u64* sp = (u64*)t->stack_top;
    *--sp = (u64)thread_start;
    *--sp = 0;              // rbp: end of the backtrace chain
    *--sp = 0;              // rbx
    *--sp = (u64)fn;        // r12
    *--sp = (u64)arg;       // r13
    *--sp = 0;              // r14
    *--sp = 0;              // r15
    t->rsp = (u64)sp;

    u64 irq = interrupts_save();
    t->id = g_next_tid++;
    t->all_next = g_all;
    g_all = t;
    t->proc_next = process->threads;
    process->threads = t;
    process->thread_count++;
    g_thread_count++;
    if (is_idle) t->state = ThreadState::Ready;     // never queued; picked when nothing else can run
    else make_ready(t);
    interrupts_restore(irq);
    if (!is_idle) resched_if_needed(irq);
    return t;
}

void idle_main(void*) {
    for (;;) {
        if (g_idle_spin) cpu_relax();
        else cpu_halt();
    }
}

// Frees detached threads after they have exited and switched away.
void reaper_main(void*) {
    for (;;) {
        u64 irq = interrupts_save();
        while (!g_dead) sched_block_locked(g_reaper_wq);
        Thread* t = g_dead;
        g_dead = t->next;
        t->next = nullptr;
        interrupts_restore(irq);
        free_thread(t);
    }
}

const char* state_name(ThreadState s) {
    switch (s) {
    case ThreadState::Ready: return "ready";
    case ThreadState::Running: return "running";
    case ThreadState::Blocked: return "blocked";
    case ThreadState::Sleeping: return "sleeping";
    case ThreadState::Zombie: return "zombie";
    }
    return "?";
}

} // namespace

// First C++ code of every thread; entered from thread_start with interrupts
// still disabled by the schedule() that switched here.
extern "C" [[noreturn]] void thread_bootstrap(void (*fn)(void*), void* arg) {
    interrupts_enable();
    fn(arg);
    thread_exit(0);
}

// =============================================================== WaitQueue ==

void WaitQueue::wait() {
    u64 irq = interrupts_save();
    block_current(this, 0);
    interrupts_restore(irq);
}

bool WaitQueue::wait_ticks(u64 ticks) {
    u64 irq = interrupts_save();
    block_current(this, g_ticks + (ticks ? ticks : 1));
    bool woken = !g_cpu.current->timed_out;
    interrupts_restore(irq);
    return woken;
}

bool WaitQueue::wake_one() {
    u64 irq = interrupts_save();
    Thread* t = wq_pop(this);
    if (t) {
        sleep_remove(t);
        make_ready(t);
    }
    interrupts_restore(irq);
    resched_if_needed(irq);
    return t != nullptr;
}

void WaitQueue::wake_all() {
    u64 irq = interrupts_save();
    while (Thread* t = wq_pop(this)) {
        sleep_remove(t);
        make_ready(t);
    }
    interrupts_restore(irq);
    resched_if_needed(irq);
}

void sched_block_locked(WaitQueue& wq) { block_current(&wq, 0); }

// ================================================================= threads ==

bool sched_running() { return g_running; }
Thread* thread_current() { return g_running ? g_cpu.current : nullptr; }
Process* process_kernel() { return &g_kernel_process; }

Result<Thread*> kthread_create(void (*fn)(void*), void* arg, const char* name, u8 priority, Process* process) {
    return create_thread(fn, arg, name, priority, process, false);
}

[[noreturn]] void thread_exit(int code) {
    interrupts_disable();
    Thread* t = g_cpu.current;
    ASSERT_ALWAYS(!t->is_idle);
    t->exit_code = code;
    t->state = ThreadState::Zombie;
    while (Thread* j = wq_pop(&t->joiners)) make_ready(j);
    if (t->detached) {
        t->next = g_dead;
        g_dead = t;
        if (Thread* r = wq_pop(&g_reaper_wq)) make_ready(r);
    }
    schedule();
    PANIC("thread_exit: zombie thread '%s' was scheduled again", t->name);
}

int thread_join(Thread* t) {
    ASSERT_ALWAYS(t != g_cpu.current && !t->detached);
    u64 irq = interrupts_save();
    while (t->state != ThreadState::Zombie) sched_block_locked(t->joiners);
    interrupts_restore(irq);
    int code = t->exit_code;
    free_thread(t);
    return code;
}

void thread_detach(Thread* t) {
    u64 irq = interrupts_save();
    ASSERT_ALWAYS(!t->detached);
    t->detached = true;
    if (t->state == ThreadState::Zombie) {      // already finished: hand it to the reaper now
        t->next = g_dead;
        g_dead = t;
        if (Thread* r = wq_pop(&g_reaper_wq)) make_ready(r);
    }
    interrupts_restore(irq);
}

void thread_yield() {
    u64 irq = interrupts_save();
    schedule();
    interrupts_restore(irq);
}

void thread_sleep_ticks(u64 ticks) {
    u64 irq = interrupts_save();
    block_current(nullptr, g_ticks + (ticks ? ticks : 1));
    interrupts_restore(irq);
}

void thread_sleep_ms(u64 ms) {
    // Round up to whole ticks, plus one because the current tick is already
    // partly over: the sleep is never shorter than asked.
    thread_sleep_ticks((ms + SCHED_TICK_MS - 1) / SCHED_TICK_MS + 1);
}

// =============================================================== processes ==

Result<Process*> process_create(const char* name) {
    Process* p = (Process*)kzalloc(sizeof(Process));
    if (!p) return Error::NoMemory;
    Result<AddressSpace*> space = AddressSpace::create();
    if (!space.ok()) {
        kfree(p);
        return space.error();
    }
    strlcpy(p->name, name, sizeof p->name);
    p->space = space.value();
    p->parent = &g_kernel_process;
    u64 irq = interrupts_save();
    p->pid = g_next_pid++;
    p->next = g_processes;
    g_processes = p;
    interrupts_restore(irq);
    return p;
}

void process_destroy(Process* p) {
    ASSERT_ALWAYS(p != &g_kernel_process && p->thread_count == 0);
    u64 irq = interrupts_save();
    for (Process** link = &g_processes; *link; link = &(*link)->next) {
        if (*link == p) {
            *link = p->next;
            break;
        }
    }
    interrupts_restore(irq);
    // The caller may still have this space loaded if it just ran there.
    if (&vmm_current() == p->space) vmm_kernel().activate();
    p->space->destroy();
    kfree(p);
}

// ================================================================== ticking ==

void sched_tick() {
    if (!g_running) return;
    g_ticks++;
    Thread* cur = g_cpu.current;
    cur->run_ticks++;
    if (cur->is_idle) g_idle_ticks++;

    // Wake everything whose time has come. A thread that was also waiting
    // on a queue leaves it and is told it timed out.
    while (g_sleepers && g_sleepers->wake_tick <= g_ticks) {
        Thread* t = g_sleepers;
        g_sleepers = t->sleep_next;
        t->sleep_next = nullptr;
        t->wake_tick = 0;
        if (t->waiting_on) {
            wq_remove(t->waiting_on, t);
            t->timed_out = true;
        }
        make_ready(t);
    }

    // Once a second, lift every demoted thread back to its base level.
    if (g_ticks % BOOST_INTERVAL_TICKS == 0) {
        for (u8 level = 1; level < prio::COUNT; level++) {
            RunQueue old = g_rq[level];
            g_rq[level] = {nullptr, nullptr};
            for (Thread* t = old.head; t;) {
                Thread* next = t->next;
                t->priority = t->base_priority;
                enqueue(t);
                t = next;
            }
        }
        if (!cur->is_idle) cur->priority = cur->base_priority;
    }

    if (!cur->is_idle && cur->slice && --cur->slice == 0) {
        if (cur->priority < prio::LOW) cur->priority++;     // used the whole slice: CPU-bound
        g_cpu.need_resched = true;
    }
}

void sched_irq_enter() { g_cpu.irq_depth++; }

void sched_irq_exit() {
    g_cpu.irq_depth--;
    if (!g_running || g_cpu.irq_depth || !g_cpu.need_resched) return;
    Thread* cur = g_cpu.current;
    if (cur->state == ThreadState::Running && !cur->is_idle) cur->preemptions++;
    schedule();
}

void sched_set_idle_spin(bool spin) { g_idle_spin = spin; }
bool sched_idle_spin() { return g_idle_spin; }
u64 sched_ticks() { return g_ticks; }

SchedStats sched_stats() {
    u64 irq = interrupts_save();
    SchedStats s{g_ticks, g_idle_ticks, g_switches, g_thread_count};
    interrupts_restore(irq);
    return s;
}

void sched_print_threads() {
    u64 irq = interrupts_save();
    kprintf("   id  name                    state     level  cpu-ms  switches  preempted  process\n");
    // The list is newest-first; print oldest first.
    u32 printed = 0;
    for (u32 want = 1; printed < g_thread_count && want < g_next_tid; want++) {
        for (Thread* t = g_all; t; t = t->all_next) {
            if (t->id != want) continue;
            kprintf("  %3u  %-22s  %-8s  %u/%u    %6lu  %8lu  %9lu  %s\n", t->id, t->name, state_name(t->state),
                    t->priority, t->base_priority, (unsigned long)(t->run_ticks * SCHED_TICK_MS),
                    (unsigned long)t->switches, (unsigned long)t->preemptions, t->process->name);
            printed++;
        }
    }
    u64 busy = g_ticks - g_idle_ticks;
    kprintf("  %lu ticks, %lu idle (%lu%% busy), %lu context switches\n", (unsigned long)g_ticks,
            (unsigned long)g_idle_ticks, (unsigned long)(g_ticks ? busy * 100 / g_ticks : 0),
            (unsigned long)g_switches);
    interrupts_restore(irq);
}

// ==================================================================== start ==

[[noreturn]] void sched_start(void (*init)(void*), void* arg) {
    interrupts_disable();
    strlcpy(g_kernel_process.name, "kernel", sizeof g_kernel_process.name);
    g_kernel_process.pid = 0;
    g_kernel_process.space = &vmm_kernel();
    g_processes = &g_kernel_process;

    // Until the first switch, "current" is a placeholder that is never
    // queued again: the stack this function runs on is abandoned.
    strlcpy(g_boot_context.name, "boot", sizeof g_boot_context.name);
    g_boot_context.state = ThreadState::Zombie;
    g_boot_context.priority = g_boot_context.base_priority = prio::LOW;
    g_boot_context.process = &g_kernel_process;
    g_boot_context.space = &vmm_current();
    g_cpu.current = &g_boot_context;

    Result<Thread*> idle = create_thread(idle_main, nullptr, "idle", prio::LOW, nullptr, true);
    Result<Thread*> reaper = create_thread(reaper_main, nullptr, "reaper", prio::NORMAL, nullptr, false);
    Result<Thread*> first = create_thread(init, arg, "init", prio::NORMAL, nullptr, false);
    if (!idle.ok() || !reaper.ok() || !first.ok()) PANIC("sched: out of memory creating the first threads");
    g_cpu.idle = idle.value();

    lapic_timer_set_hook(sched_tick);
    g_running = true;
    kprintf("sched: started; %u-tick slices of %u ms, %u priority levels, %lu KiB guarded stacks\n", SLICE_TICKS,
            SCHED_TICK_MS, prio::COUNT, (unsigned long)(THREAD_STACK_SIZE / KIB));
    schedule();
    PANIC("sched: the boot context was scheduled again");
}
