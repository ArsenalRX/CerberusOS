// Scheduler core: per-CPU run queues, the sleep list, wait queues, thread and
// process lifetime. See sched.h for the policy and the locking rules.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpufeatures.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <drivers/lapic.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/vmm.h>
#include <fs/vfs.h>
#include <sched/sched.h>
#include <sched/sync.h>

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

// Per-CPU scheduler state. Everything except irq_depth is protected by the
// scheduler lock; irq_depth belongs to its CPU alone.
struct Cpu {
    u32 id;
    bool online;            // scheduling: threads may be queued here
    bool need_resched;      // switch threads at the next opportunity
    u32 irq_depth;
    Thread* current;
    Thread* idle;
    RunQueue rq[prio::COUNT];
    u32 queued;             // threads on rq
    u64 ticks;
    u64 idle_ticks;
    u64 switches;
    u64 steals;
    Thread boot_context;    // what the CPU switches away from when it starts scheduling; never resumed
};

Cpu g_cpus[MAX_CPUS];
u32 g_cpu_limit = 1;                // CPU ids below this may be scheduling
Spinlock g_lock = SPINLOCK_RANKED(lock_rank::SCHED);

Thread* g_sleepers = nullptr;       // sorted by wake_tick
Thread* g_all = nullptr;
Thread* g_dead = nullptr;           // detached zombies for the reaper (linked through next)
WaitQueue g_reaper_wq;
Process g_kernel_process;
Process* g_processes = nullptr;
bool g_running = false;
bool g_idle_spin = false;
u64 g_ticks = 0;                    // the bootstrap CPU's ticks: the scheduler's clock
u32 g_next_tid = 1;
u32 g_next_pid = 1;
u32 g_thread_count = 0;

// The calling CPU's state. Interrupts must be off.
inline Cpu* this_cpu() { return &g_cpus[percpu_cpu_id()]; }

// ------------------------------------------------------------- run queues --
void enqueue(Cpu* c, Thread* t) {
    RunQueue& q = c->rq[t->priority];
    t->next = nullptr;
    if (q.tail) q.tail->next = t;
    else q.head = t;
    q.tail = t;
    t->cpu = c->id;
    c->queued++;
}

Thread* dequeue_highest(Cpu* c) {
    for (RunQueue& q : c->rq) {
        Thread* t = q.head;
        if (!t) continue;
        q.head = t->next;
        if (!q.head) q.tail = nullptr;
        t->next = nullptr;
        c->queued--;
        return t;
    }
    return nullptr;
}

// Takes the most urgent thread waiting behind a busy CPU that may run on
// `thief`. nullptr if there is none. Threads queued for a CPU that is idle
// are left alone: that CPU has been told and is about to run them, and
// taking them would only bounce them between CPUs.
Thread* steal(Cpu* thief) {
    for (u8 level = 0; level < prio::COUNT; level++) {
        for (u32 i = 0; i < g_cpu_limit; i++) {
            Cpu* victim = &g_cpus[i];
            if (victim == thief || !victim->online || victim->current->is_idle) continue;
            RunQueue& q = victim->rq[level];
            Thread* prev = nullptr;
            for (Thread* t = q.head; t; prev = t, t = t->next) {
                if (!(t->cpu_affinity & (1u << thief->id))) continue;
                if (prev) prev->next = t->next;
                else q.head = t->next;
                if (q.tail == t) q.tail = prev;
                t->next = nullptr;
                victim->queued--;
                thief->steals++;
                return t;
            }
        }
    }
    return nullptr;
}

bool anything_to_steal(const Cpu* thief) {
    for (u32 i = 0; i < g_cpu_limit; i++) {
        const Cpu* victim = &g_cpus[i];
        if (victim == thief || !victim->online || !victim->queued || victim->current->is_idle) continue;
        for (const RunQueue& q : victim->rq)
            for (const Thread* t = q.head; t; t = t->next)
                if (t->cpu_affinity & (1u << thief->id)) return true;
    }
    return false;
}

inline bool cpu_is_free(const Cpu* c) { return c->current->is_idle && c->queued == 0; }

// Where a thread that has just become runnable should wait: the CPU it last
// ran on if that one has nothing to do (its caches may still hold the
// thread's data), else any CPU with nothing to do, else its last CPU again.
Cpu* pick_cpu(const Thread* t) {
    u32 online = 0;
    for (u32 i = 0; i < g_cpu_limit; i++)
        if (g_cpus[i].online) online |= 1u << i;
    u32 allowed = t->cpu_affinity & online;
    if (!allowed) allowed = online;
    Cpu* last = (t->cpu < g_cpu_limit && (allowed & (1u << t->cpu))) ? &g_cpus[t->cpu] : nullptr;
    if (last && cpu_is_free(last)) return last;
    Cpu* first = nullptr;
    for (u32 i = 0; i < g_cpu_limit; i++) {
        if (!(allowed & (1u << i))) continue;
        if (cpu_is_free(&g_cpus[i])) return &g_cpus[i];
        if (!first) first = &g_cpus[i];
    }
    return last ? last : first;
}

// A thread of priority `priority` was queued on `c`: have `c` switch to it
// if it is more urgent than what `c` is running.
void kick_if_more_urgent(Cpu* c, u8 priority) {
    Thread* cur = c->current;
    if (!cur->is_idle && priority >= cur->priority) return;
    if (c->need_resched) return;        // already on its way to schedule()
    c->need_resched = true;
    if (c != this_cpu()) smp_send_resched(c->id);
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

// Lock held. Puts a blocked or sleeping thread on a run queue and, if it
// should run before what its CPU is running, tells that CPU.
void make_ready(Thread* t) {
    t->state = ThreadState::Ready;
    t->waiting_on = nullptr;
    Cpu* c = pick_cpu(t);
    enqueue(c, t);
    kick_if_more_urgent(c, t->priority);
}

// Lock held. Picks the next thread for this CPU and switches to it. Returns
// when the calling thread is next scheduled, which may be on another CPU;
// the lock is held then too (taken by whoever switched to it).
void schedule() {
    Cpu* c = this_cpu();
    Thread* prev = c->current;
    if (prev->state == ThreadState::Running) {
        prev->state = ThreadState::Ready;
        if (!prev->is_idle) {
            // Normally back onto this CPU's queue; elsewhere only if the
            // thread has just been forbidden to run here.
            Cpu* home = (prev->cpu_affinity & (1u << c->id)) ? c : pick_cpu(prev);
            enqueue(home, prev);
            if (home != c) kick_if_more_urgent(home, prev->priority);
        }
    }
    Thread* next = dequeue_highest(c);
    if (!next) next = steal(c);
    if (!next) next = c->idle;
    c->need_resched = false;
    next->state = ThreadState::Running;
    next->slice = SLICE_TICKS;
    next->cpu = c->id;
    if (next == prev) return;

    PerCpu* pc = percpu();
    c->current = next;
    pc->current = next;
    next->switches++;
    c->switches++;
    tss_set_kernel_stack(c->id, next->stack_top);
    pc->kernel_rsp = next->stack_top;
    // FPU/SSE registers are only used by user threads. A thread may resume
    // on a different CPU, so its registers are saved every time it leaves
    // one, not lazily.
    if (prev->fpu && prev->state != ThreadState::Zombie) fpu_save(prev->fpu);
    if (next->fpu) fpu_restore(next->fpu);
    // The FS base register always holds the running thread's TLS pointer.
    if (next->fs_base != prev->fs_base) wrmsr(msr::FS_BASE, next->fs_base);
    // Each thread carries the address space it was running in; reload CR3
    // only when it differs from what this CPU has loaded.
    prev->space = pc->space;
    if (next->space != pc->space) next->space->activate();
    context_switch(&prev->rsp, next->rsp);
}

void block_current(WaitQueue* wq, u64 wake_tick) {
    Cpu* c = this_cpu();
    Thread* t = c->current;
    ASSERT_ALWAYS(!t->is_idle && c->irq_depth == 0);
    t->state = wq ? ThreadState::Blocked : ThreadState::Sleeping;
    t->timed_out = false;
    t->waiting_on = wq;
    if (wq) wq_append(wq, t);
    if (wake_tick) sleep_insert(t, wake_tick);
    t->priority = t->base_priority;     // blocking is what interactive threads do: back to base level
    schedule();
}

// ----------------------------------------------------------------- threads --
// Lock held.
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

// The thread must be a zombie. Whoever saw that state under the scheduler
// lock knows the thread has left its stack: the lock is not released until
// the switch away from it is complete.
void free_thread(Thread* t) {
    u64 irq = sched_lock();
    unlink_thread(t);
    sched_unlock(irq);
    vmm_free_kernel_stack(t->stack_top, THREAD_STACK_SIZE);
    kfree(t->fpu);
    kfree(t);
}

// `idle_for` is the CPU an idle thread belongs to, or -1 for an ordinary thread.
Result<Thread*> create_thread(void (*fn)(void*), void* arg, const char* name, u8 priority, Process* process,
                              bool detached, int idle_for) {
    if (priority >= prio::COUNT) return Error::Invalid;
    Thread* t = (Thread*)kzalloc(sizeof(Thread));
    if (!t) return Error::NoMemory;
    Result<vaddr_t> stack = vmm_alloc_kernel_stack(THREAD_STACK_SIZE);
    if (!stack.ok()) {
        kfree(t);
        return stack.error();
    }
    bool is_idle = idle_for >= 0;
    if (!process) process = &g_kernel_process;
    t->stack_top = stack.value();
    strlcpy(t->name, name, sizeof t->name);
    t->base_priority = t->priority = priority;
    t->slice = SLICE_TICKS;
    t->is_idle = is_idle;
    t->detached = detached;
    t->cpu_affinity = is_idle ? 1u << idle_for : ~0u;
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

    u64 irq = sched_lock();
    // A new thread starts where its creator is: that CPU is certainly awake.
    t->cpu = is_idle ? (u32)idle_for : this_cpu()->id;
    t->id = g_next_tid++;
    t->all_next = g_all;
    g_all = t;
    t->proc_next = process->threads;
    process->threads = t;
    process->thread_count++;
    g_thread_count++;
    if (is_idle) t->state = ThreadState::Ready;     // never queued; picked when nothing else can run
    else make_ready(t);
    sched_unlock(irq);
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
        u64 irq = sched_lock();
        while (!g_dead) sched_block_locked(g_reaper_wq);
        Thread* t = g_dead;
        g_dead = t->next;
        t->next = nullptr;
        sched_unlock(irq);
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

void init_boot_context(Cpu* c) {
    Thread& b = c->boot_context;
    strlcpy(b.name, "boot", sizeof b.name);
    b.state = ThreadState::Zombie;
    b.priority = b.base_priority = prio::LOW;
    b.process = &g_kernel_process;
    b.space = &vmm_kernel();
    b.cpu = c->id;
    b.cpu_affinity = 1u << c->id;
}

} // namespace

// First C++ code of every thread; entered from thread_start still holding
// the scheduler lock taken by the schedule() that switched here, with
// interrupts off.
extern "C" [[noreturn]] void thread_bootstrap(void (*fn)(void*), void* arg) {
    g_lock.release();
    interrupts_enable();
    fn(arg);
    thread_exit(0);
}

// ==================================================================== lock ==

u64 sched_lock() {
    u64 irq = interrupts_save();
    g_lock.acquire();
    return irq;
}

void sched_unlock(u64 irq) {
    // A wake-up made under the lock may have asked this CPU to switch. Do it
    // now if the caller was in ordinary thread context: interrupts were on,
    // so it holds no other spinlock and is not inside an interrupt handler
    // (which switches on its way out instead).
    if (g_running && (irq & RFLAGS_IF)) {
        Cpu* c = this_cpu();
        if (c->need_resched && !c->irq_depth) schedule();
    }
    g_lock.release();
    interrupts_restore(irq);
}

// =============================================================== WaitQueue ==

void WaitQueue::wait() {
    u64 irq = sched_lock();
    block_current(this, 0);
    sched_unlock(irq);
}

bool WaitQueue::wait_ticks(u64 ticks) {
    u64 irq = sched_lock();
    Thread* self = this_cpu()->current;
    block_current(this, g_ticks + (ticks ? ticks : 1));
    bool woken = !self->timed_out;
    sched_unlock(irq);
    return woken;
}

bool sched_wake_one_locked(WaitQueue& wq) {
    Thread* t = wq_pop(&wq);
    if (!t) return false;
    sleep_remove(t);
    make_ready(t);
    return true;
}

void sched_wake_all_locked(WaitQueue& wq) {
    while (sched_wake_one_locked(wq)) {
    }
}

bool WaitQueue::wake_one() {
    u64 irq = sched_lock();
    bool woke = sched_wake_one_locked(*this);
    sched_unlock(irq);
    return woke;
}

void WaitQueue::wake_all() {
    u64 irq = sched_lock();
    sched_wake_all_locked(*this);
    sched_unlock(irq);
}

void sched_block_locked(WaitQueue& wq) { block_current(&wq, 0); }

WaitResult sched_block_interruptible_locked(WaitQueue* wq, u64 ticks) {
    Thread* t = this_cpu()->current;
    if (t->interrupt_pending) return WaitResult::Interrupted;
    t->interruptible = true;
    t->interrupted = false;
    block_current(wq, ticks ? g_ticks + ticks : 0);
    t->interruptible = false;
    if (t->interrupted) return WaitResult::Interrupted;
    return t->timed_out ? WaitResult::Timeout : WaitResult::Woken;
}

void thread_interrupt_locked(Thread* t) {
    t->interrupt_pending = true;
    if (t->interruptible && (t->state == ThreadState::Blocked || t->state == ThreadState::Sleeping)) {
        if (t->waiting_on) wq_remove(t->waiting_on, t);
        sleep_remove(t);
        t->interrupted = true;
        t->interruptible = false;
        make_ready(t);
    } else if (t->state == ThreadState::Running && t->cpu != this_cpu()->id) {
        smp_send_resched(t->cpu);
    }
}

WaitResult thread_sleep_interruptible(u64 ticks) {
    u64 irq = sched_lock();
    WaitResult r = sched_block_interruptible_locked(nullptr, ticks ? ticks : 1);
    sched_unlock(irq);
    return r;
}

bool sched_block_locked_ticks(WaitQueue& wq, u64 ticks) {
    Thread* self = this_cpu()->current;
    block_current(&wq, g_ticks + (ticks ? ticks : 1));
    return !self->timed_out;
}

// ================================================================= threads ==

bool sched_running() { return g_running; }
Thread* thread_current() { return percpu_current_thread(); }
Process* process_kernel() { return &g_kernel_process; }

Result<Thread*> kthread_create(void (*fn)(void*), void* arg, const char* name, u8 priority, Process* process,
                               bool detached) {
    return create_thread(fn, arg, name, priority, process, detached, -1);
}

[[noreturn]] void thread_exit(int code) {
    // A user thread leaves its process's address space first: once it is
    // counted out below, another thread may tear that space down.
    if (thread_current()->user) vmm_kernel().activate();
    interrupts_disable();
    g_lock.acquire();
    Thread* t = this_cpu()->current;
    ASSERT_ALWAYS(!t->is_idle);
    t->exit_code = code;
    t->state = ThreadState::Zombie;
    if (t->user) {
        t->user = false;
        t->process->live_threads--;
        sched_wake_all_locked(t->process->exit_wait);
    }
    sched_wake_all_locked(t->joiners);
    if (t->detached) {
        t->next = g_dead;
        g_dead = t;
        sched_wake_one_locked(g_reaper_wq);
    }
    schedule();
    PANIC("thread_exit: zombie thread '%s' was scheduled again", t->name);
}

int thread_join(Thread* t) {
    ASSERT_ALWAYS(t != thread_current() && !t->detached);
    u64 irq = sched_lock();
    while (t->state != ThreadState::Zombie) sched_block_locked(t->joiners);
    sched_unlock(irq);
    int code = t->exit_code;
    free_thread(t);
    return code;
}

Result<int> thread_join_user(u32 tid) {
    Thread* self = thread_current();
    u64 irq = sched_lock();
    Thread* t = nullptr;
    for (Thread* o = self->process->threads; o; o = o->proc_next)
        if (o->id == tid) t = o;
    if (!t || t == self || t->detached || t->joining) {
        sched_unlock(irq);
        return t ? Error::Invalid : Error::NoProcess;
    }
    t->joining = true;
    while (t->state != ThreadState::Zombie) {
        if (sched_block_interruptible_locked(&t->joiners, 0) == WaitResult::Interrupted) {
            t->joining = false;
            sched_unlock(irq);
            return Error::Interrupted;
        }
    }
    sched_unlock(irq);
    int code = t->exit_code;
    free_thread(t);
    return code;
}

void thread_restore_fpu(const u8* image) {
    u64 irq = interrupts_save();
    Thread* t = thread_current();
    ASSERT_ALWAYS(t->fpu);
    memcpy(t->fpu, image, FPU_STATE_SIZE);
    fpu_restore(t->fpu);
    interrupts_restore(irq);
}

void thread_set_fs_base(u64 base) {
    u64 irq = interrupts_save();
    thread_current()->fs_base = base;
    wrmsr(msr::FS_BASE, base);
    interrupts_restore(irq);
}

void thread_detach(Thread* t) {
    u64 irq = sched_lock();
    ASSERT_ALWAYS(!t->detached);
    t->detached = true;
    if (t->state == ThreadState::Zombie) {      // already finished: hand it to the reaper now
        t->next = g_dead;
        g_dead = t;
        sched_wake_one_locked(g_reaper_wq);
    }
    sched_unlock(irq);
}

void thread_yield() {
    u64 irq = sched_lock();
    schedule();
    sched_unlock(irq);
}

void thread_sleep_ticks(u64 ticks) {
    u64 irq = sched_lock();
    block_current(nullptr, g_ticks + (ticks ? ticks : 1));
    sched_unlock(irq);
}

void thread_sleep_ms(u64 ms) {
    // Round up to whole ticks, plus one because the current tick is already
    // partly over: the sleep is never shorter than asked.
    thread_sleep_ticks((ms + SCHED_TICK_MS - 1) / SCHED_TICK_MS + 1);
}

void thread_set_affinity(u32 mask) {
    u64 irq = sched_lock();
    Cpu* c = this_cpu();
    c->current->cpu_affinity = mask;
    u32 online = 0;
    for (u32 i = 0; i < g_cpu_limit; i++)
        if (g_cpus[i].online) online |= 1u << i;
    // Not allowed here any more: schedule() queues the thread on a CPU it
    // may use, and it resumes there.
    if ((mask & online) && !(mask & (1u << c->id))) schedule();
    sched_unlock(irq);
}

u32 thread_cpu() { return percpu_cpu_id(); }

Result<void> thread_enable_fpu(const u8* initial) {
    Thread* t = thread_current();
    if (!t->fpu) {
        u8* area = (u8*)kmalloc(FPU_STATE_SIZE);        // 16-byte aligned, as FXSAVE requires
        if (!area) return Error::NoMemory;
        if (initial) memcpy(area, initial, FPU_STATE_SIZE);
        else fpu_init_state(area);
        // Publish the area and load it in one step: a switch in between
        // would save whatever is in the registers over the image.
        u64 irq = interrupts_save();
        t->fpu = area;
        fpu_restore(area);
        interrupts_restore(irq);
    }
    return {};
}

void thread_snapshot_fpu(u8* out) {
    u64 irq = interrupts_save();
    Thread* t = thread_current();
    ASSERT_ALWAYS(t->fpu);
    fpu_save(t->fpu);
    memcpy(out, t->fpu, FPU_STATE_SIZE);
    interrupts_restore(irq);
}

void thread_reset_fpu() {
    u64 irq = interrupts_save();
    Thread* t = thread_current();
    ASSERT_ALWAYS(t->fpu);
    fpu_init_state(t->fpu);
    fpu_restore(t->fpu);
    interrupts_restore(irq);
}

void thread_set_process(Thread* t, Process* p) {
    u64 irq = sched_lock();
    for (Thread** link = &t->process->threads; *link; link = &(*link)->proc_next) {
        if (*link == t) {
            *link = t->proc_next;
            break;
        }
    }
    t->process->thread_count--;
    t->process = p;
    t->proc_next = p->threads;
    p->threads = t;
    p->thread_count++;
    sched_unlock(irq);
}

// =============================================================== processes ==

Result<Process*> process_create(const char* name, AddressSpace* existing) {
    Process* p = (Process*)kzalloc(sizeof(Process));
    if (!p) return Error::NoMemory;
    if (!existing) {
        Result<AddressSpace*> space = AddressSpace::create();
        if (!space.ok()) {
            kfree(p);
            return space.error();
        }
        existing = space.value();
    }
    strlcpy(p->name, name, sizeof p->name);
    p->space = existing;
    p->parent = &g_kernel_process;
    p->umask = 022;
    u64 irq = sched_lock();
    p->pid = g_next_pid++;
    p->next = g_processes;
    g_processes = p;
    sched_unlock(irq);
    return p;
}

void process_release_threads(Process* p, Thread* keep) {
    Process* k = &g_kernel_process;
    u64 irq = sched_lock();
    Thread* t = p->threads;
    p->threads = nullptr;
    while (t) {
        Thread* next = t->proc_next;
        if (t == keep) {
            t->proc_next = p->threads;
            p->threads = t;
        } else {
            p->thread_count--;
            t->process = k;
            t->proc_next = k->threads;
            k->threads = t;
            k->thread_count++;
            if (!t->detached) {
                // Nobody is left to join it.
                t->detached = true;
                if (t->state == ThreadState::Zombie) {
                    t->next = g_dead;
                    g_dead = t;
                    sched_wake_one_locked(g_reaper_wq);
                }
            }
        }
        t = next;
    }
    sched_unlock(irq);
}

Process* process_find_locked(u32 pid) {
    for (Process* p = g_processes; p; p = p->next)
        if (p->pid == pid) return p;
    return nullptr;
}

void process_destroy(Process* p) {
    ASSERT_ALWAYS(p != &g_kernel_process && p->thread_count == 0);
    u64 irq = sched_lock();
    for (Process** link = &g_processes; *link; link = &(*link)->next) {
        if (*link == p) {
            *link = p->next;
            break;
        }
    }
    sched_unlock(irq);
    if (p->cwd) vnode_unref(p->cwd);
    // The caller may still have this space loaded if it just ran there.
    if (p->space) {             // an exited process has already given its space up
        if (&vmm_current() == p->space) vmm_kernel().activate();
        p->space->destroy();
    }
    kfree(p);
}

// ================================================================== ticking ==

// Every CPU's timer interrupt. The bootstrap CPU's tick is also the clock:
// it wakes sleepers and, once a second, restores priorities.
void sched_tick() {
    if (!g_running) return;
    g_lock.acquire();
    Cpu* c = this_cpu();
    Thread* cur = c->current;
    c->ticks++;
    cur->run_ticks++;
    if (cur->is_idle) c->idle_ticks++;

    if (c->id == 0) {
        g_ticks++;
        // Wake everything whose time has come. A thread that was also
        // waiting on a queue leaves it and is told it timed out.
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
            for (u32 i = 0; i < g_cpu_limit; i++) {
                Cpu* o = &g_cpus[i];
                if (!o->online) continue;
                for (u8 level = 1; level < prio::COUNT; level++) {
                    RunQueue old = o->rq[level];
                    o->rq[level] = {nullptr, nullptr};
                    for (Thread* t = old.head; t;) {
                        Thread* next = t->next;
                        t->priority = t->base_priority;
                        o->queued--;
                        enqueue(o, t);
                        t = next;
                    }
                }
                if (!o->current->is_idle) o->current->priority = o->current->base_priority;
            }
        }
    }

    if (!cur->is_idle && cur->slice && --cur->slice == 0) {
        if (cur->priority < prio::LOW) cur->priority++;     // used the whole slice: CPU-bound
        c->need_resched = true;
    }
    // An idle CPU looks for work that is waiting behind a busy one.
    if (cur->is_idle && (c->queued || anything_to_steal(c))) c->need_resched = true;
    g_lock.release();
}

void sched_irq_enter() { this_cpu()->irq_depth++; }

void sched_irq_exit() {
    Cpu* c = this_cpu();
    c->irq_depth--;
    if (!g_running || c->irq_depth || !c->need_resched || !c->online) return;
    g_lock.acquire();
    Thread* cur = c->current;
    if (cur->state == ThreadState::Running && !cur->is_idle) cur->preemptions++;
    schedule();
    g_lock.release();
}

void sched_set_idle_spin(bool spin) { g_idle_spin = spin; }
bool sched_idle_spin() { return g_idle_spin; }
u64 sched_ticks() { return g_ticks; }

SchedStats sched_stats() {
    u64 irq = sched_lock();
    SchedStats s{};
    for (u32 i = 0; i < g_cpu_limit; i++) {
        const Cpu& c = g_cpus[i];
        if (!c.online) continue;
        s.ticks += c.ticks;
        s.idle_ticks += c.idle_ticks;
        s.context_switches += c.switches;
        s.steals += c.steals;
        s.cpus++;
    }
    s.threads = g_thread_count;
    for (Process* p = g_processes; p; p = p->next) s.processes++;
    sched_unlock(irq);
    return s;
}

void sched_cpu_ticks(u32 cpu, u64* ticks, u64* idle_ticks) {
    u64 irq = sched_lock();
    bool ok = cpu < g_cpu_limit && g_cpus[cpu].online;
    *ticks = ok ? g_cpus[cpu].ticks : 0;
    *idle_ticks = ok ? g_cpus[cpu].idle_ticks : 0;
    sched_unlock(irq);
}

void sched_print_threads() {
    u64 irq = sched_lock();
    kprintf("   id  name                    state     cpu  level  cpu-ms  switches  preempted  process\n");
    // The list is newest-first; print oldest first.
    u32 printed = 0;
    for (u32 want = 1; printed < g_thread_count && want < g_next_tid; want++) {
        for (Thread* t = g_all; t; t = t->all_next) {
            if (t->id != want) continue;
            kprintf("  %3u  %-22s  %-8s  %3u  %u/%u    %6lu  %8lu  %9lu  %s\n", t->id, t->name, state_name(t->state),
                    t->cpu, t->priority, t->base_priority, (unsigned long)(t->run_ticks * SCHED_TICK_MS),
                    (unsigned long)t->switches, (unsigned long)t->preemptions, t->process->name);
            printed++;
        }
    }
    u64 ticks = 0, idle = 0, switches = 0, steals = 0;
    for (u32 i = 0; i < g_cpu_limit; i++) {
        const Cpu& c = g_cpus[i];
        if (!c.online) continue;
        ticks += c.ticks;
        idle += c.idle_ticks;
        switches += c.switches;
        steals += c.steals;
        kprintf("  cpu %u: %lu ticks, %lu idle (%lu%% busy), %lu switches, %lu stolen\n", c.id,
                (unsigned long)c.ticks, (unsigned long)c.idle_ticks,
                (unsigned long)(c.ticks ? (c.ticks - c.idle_ticks) * 100 / c.ticks : 0), (unsigned long)c.switches,
                (unsigned long)c.steals);
    }
    kprintf("  %lu ticks, %lu idle (%lu%% busy), %lu context switches\n", (unsigned long)ticks, (unsigned long)idle,
            (unsigned long)(ticks ? (ticks - idle) * 100 / ticks : 0), (unsigned long)switches);
    sched_unlock(irq);
}

// ==================================================================== start ==

[[noreturn]] void sched_start(void (*init)(void*), void* arg) {
    interrupts_disable();
    strlcpy(g_kernel_process.name, "kernel", sizeof g_kernel_process.name);
    g_kernel_process.pid = 0;
    g_kernel_process.space = &vmm_kernel();
    g_processes = &g_kernel_process;

    // Until its first switch, each CPU's "current" is a placeholder that is
    // never queued again: the stack it starts scheduling on is abandoned.
    g_cpu_limit = smp_cpu_count();
    for (u32 i = 0; i < g_cpu_limit; i++) {
        g_cpus[i].id = i;
        init_boot_context(&g_cpus[i]);
    }
    Cpu* c = &g_cpus[0];
    c->boot_context.space = &vmm_current();
    c->current = &c->boot_context;
    percpu()->current = &c->boot_context;
    c->online = true;

    for (u32 i = 0; i < g_cpu_limit; i++) {
        if (!smp_cpu_present(i)) continue;
        Result<Thread*> idle = create_thread(idle_main, nullptr, "idle", prio::LOW, nullptr, false, (int)i);
        if (!idle.ok()) PANIC("sched: out of memory creating the idle threads");
        g_cpus[i].idle = idle.value();
    }
    Result<Thread*> reaper = create_thread(reaper_main, nullptr, "reaper", prio::NORMAL, nullptr, false, -1);
    Result<Thread*> first = create_thread(init, arg, "init", prio::NORMAL, nullptr, false, -1);
    if (!reaper.ok() || !first.ok()) PANIC("sched: out of memory creating the first threads");

    lapic_timer_set_hook(sched_tick);
    g_running = true;
    kprintf("sched: started on %u CPU(s); %u-tick slices of %u ms, %u priority levels, %lu KiB guarded stacks\n",
            g_cpu_limit, SLICE_TICKS, SCHED_TICK_MS, prio::COUNT, (unsigned long)(THREAD_STACK_SIZE / KIB));
    smp_start_scheduling();
    g_lock.acquire();
    schedule();
    PANIC("sched: the boot context was scheduled again");
}

[[noreturn]] void sched_enter_ap() {
    interrupts_disable();
    Cpu* c = this_cpu();
    // A processor that reported in too late to be given an idle thread
    // cannot schedule; it stays out.
    if (!c->idle) {
        for (;;) asm volatile("cli; hlt");
    }
    g_lock.acquire();
    c->current = &c->boot_context;
    percpu()->current = &c->boot_context;
    c->online = true;
    schedule();
    PANIC("sched: cpu %u's boot context was scheduled again", c->id);
}
