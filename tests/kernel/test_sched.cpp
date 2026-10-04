// Phase 6 acceptance (SPEC §5 and §5A): five threads that are visibly
// preempted; a bounded-buffer producer/consumer run (10 s by default) with
// exact totals and no deadlock; sleep accuracy; mutex, semaphore and
// reader-writer lock behaviour; wake-up latency and context-switch time;
// two processes that each see only their own memory; the desktop staying
// alive throughout; and no thread or allocation left behind.
// Usage: test sched [seconds]   (length of the producer/consumer run)
#include <drivers/refclock.h>
#include <gui/desktop.h>
#include <kernel/kbench.h>
#include <kernel/ktest.h>
#include <lib/kprintf.h>
#include <mm/kheap.h>
#include <mm/usercopy.h>
#include <mm/vmm.h>
#include <sched/sched.h>
#include <sched/sync.h>

namespace {

// ------------------------------------------------------------ preemption --
struct Worker {
    u32 index;
    volatile u64 count;
    u64 preempted;
};
Worker g_workers[5];

// Pure CPU work with no voluntary switch: only the timer can interleave these.
void counting_worker(void* arg) {
    Worker* w = (Worker*)arg;
    for (int report = 1; report <= 3; report++) {
        u64 until = refclock_now_us() + 80000;
        while (refclock_now_us() < until) w->count = w->count + 1;
        kprintf("  worker %u: count %lu\n", w->index, (unsigned long)w->count);
    }
    w->preempted = thread_current()->preemptions;
}

// ------------------------------------------------------ producer/consumer --
constexpr u32 BUF_SLOTS = 16;
constexpr u64 SENTINEL = ~0ull;
struct Channel {
    Mutex lock;
    CondVar not_full, not_empty;
    u64 slots[BUF_SLOTS];
    u32 head, count;
    u64 deadline_us;
};
Channel g_chan;

struct Tally {
    u64 items;
    u64 sum;
};
Tally g_produced[2], g_consumed[2];

void chan_put(u64 v) {
    g_chan.lock.lock();
    while (g_chan.count == BUF_SLOTS) g_chan.not_full.wait(g_chan.lock);
    g_chan.slots[(g_chan.head + g_chan.count) % BUF_SLOTS] = v;
    g_chan.count++;
    g_chan.lock.unlock();
    g_chan.not_empty.signal();
}

u64 chan_get() {
    g_chan.lock.lock();
    while (g_chan.count == 0) g_chan.not_empty.wait(g_chan.lock);
    u64 v = g_chan.slots[g_chan.head];
    g_chan.head = (g_chan.head + 1) % BUF_SLOTS;
    g_chan.count--;
    g_chan.lock.unlock();
    g_chan.not_full.signal();
    return v;
}

void producer(void* arg) {
    Tally* t = (Tally*)arg;
    u64 next = 1;
    // Check the clock only every 64 items; reading it is slow under a hypervisor.
    for (;;) {
        if ((t->items & 63) == 0 && refclock_now_us() >= g_chan.deadline_us) break;
        chan_put(next);
        t->items++;
        t->sum += next++;
    }
}

void consumer(void* arg) {
    Tally* t = (Tally*)arg;
    for (;;) {
        u64 v = chan_get();
        if (v == SENTINEL) return;
        t->items++;
        t->sum += v;
    }
}

// ------------------------------------------------------------------ mutex --
Mutex g_counter_lock;
volatile u64 g_counter;

void locked_incrementer(void*) {
    for (u32 i = 0; i < 50000; i++) {
        g_counter_lock.lock();
        u64 v = g_counter;
        if ((i & 1023) == 0) thread_yield();        // hold the lock across a switch
        g_counter = v + 1;
        g_counter_lock.unlock();
    }
}

// -------------------------------------------------- semaphore and rwlock --
Semaphore g_sem;
volatile u32 g_sem_passed;
void sem_waiter(void*) {
    g_sem.down();
    g_sem_passed = g_sem_passed + 1;
}

RwLock g_rw;
volatile i32 g_rw_readers_inside, g_rw_max_readers;
volatile bool g_rw_writer_inside, g_rw_violation;

void rw_reader(void*) {
    for (int i = 0; i < 200; i++) {
        g_rw.read_lock();
        g_rw_readers_inside = g_rw_readers_inside + 1;
        if (g_rw_readers_inside > g_rw_max_readers) g_rw_max_readers = g_rw_readers_inside;
        if (g_rw_writer_inside) g_rw_violation = true;
        thread_yield();
        g_rw_readers_inside = g_rw_readers_inside - 1;
        g_rw.read_unlock();
    }
}

void rw_writer(void*) {
    for (int i = 0; i < 100; i++) {
        g_rw.write_lock();
        if (g_rw_writer_inside || g_rw_readers_inside) g_rw_violation = true;
        g_rw_writer_inside = true;
        thread_yield();
        g_rw_writer_inside = false;
        g_rw.write_unlock();
        thread_yield();
    }
}

// ------------------------------------------------------ address spaces ---
constexpr vaddr_t PRIVATE_ADDR = 0x50000000ull;
volatile bool g_space_mismatch;

// Runs in its own process: writes its tag to an address the other process
// uses too, and checks after every switch that it still reads its own.
void space_worker(void* arg) {
    u64 tag = (u64)arg;
    if (!copy_to_user(PRIVATE_ADDR, &tag, sizeof tag).ok()) g_space_mismatch = true;
    for (int i = 0; i < 2000; i++) {
        u64 seen = 0;
        if (!copy_from_user(&seen, PRIVATE_ADDR, sizeof seen).ok() || seen != tag) g_space_mismatch = true;
        thread_yield();
    }
}

u64 parse_seconds(const char* s) {
    u64 v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (u64)(*s++ - '0');
    return v ? v : 10;
}

} // namespace

int ktest_sched(int argc, char** argv) {
    u64 seconds = argc > 1 ? parse_seconds(argv[1]) : 10;
    SchedStats s0 = sched_stats();
    KheapStats h0 = kheap_stats();
    u64 frames0 = gui_active() ? gui_stats().frames : 0;
    Thread* threads[8];

    // --- five CPU-bound threads share the CPU by preemption alone ---
    for (u32 i = 0; i < 5; i++) {
        g_workers[i] = {i + 1, 0, 0};
        Result<Thread*> t = kthread_create(counting_worker, &g_workers[i], "worker");
        KTEST_CHECK(t.ok());
        threads[i] = t.value();
    }
    for (u32 i = 0; i < 5; i++) thread_join(threads[i]);
    for (u32 i = 0; i < 5; i++) KTEST_CHECK(g_workers[i].count > 0 && g_workers[i].preempted > 0);
    kprintf("  preemption: 5 busy threads interleaved; preempted %lu, %lu, %lu, %lu and %lu times\n",
            (unsigned long)g_workers[0].preempted, (unsigned long)g_workers[1].preempted,
            (unsigned long)g_workers[2].preempted, (unsigned long)g_workers[3].preempted,
            (unsigned long)g_workers[4].preempted);

    // --- sleep is never short and not much longer than asked ---
    u64 t0 = refclock_now_us();
    thread_sleep_ms(100);
    u64 slept = refclock_now_us() - t0;
    KTEST_CHECK(slept >= 100000 && slept <= 140000);
    kprintf("  sleep: asked for 100 ms, slept %lu.%lu ms\n", (unsigned long)(slept / 1000),
            (unsigned long)(slept % 1000 / 100));

    // --- mutex: 4 threads x 50,000 increments, switching inside the lock ---
    g_counter = 0;
    for (u32 i = 0; i < 4; i++) {
        Result<Thread*> t = kthread_create(locked_incrementer, nullptr, "incrementer");
        KTEST_CHECK(t.ok());
        threads[i] = t.value();
    }
    for (u32 i = 0; i < 4; i++) thread_join(threads[i]);
    KTEST_CHECK(g_counter == 200000);
    kprintf("  mutex: 4 threads made %lu increments, none lost\n", (unsigned long)g_counter);

    // --- semaphore: three waiters pass only after three ups ---
    g_sem_passed = 0;
    for (u32 i = 0; i < 3; i++) {
        Result<Thread*> t = kthread_create(sem_waiter, nullptr, "sem-waiter");
        KTEST_CHECK(t.ok());
        threads[i] = t.value();
    }
    thread_sleep_ticks(2);
    KTEST_CHECK(g_sem_passed == 0);
    g_sem.up();
    thread_sleep_ticks(2);
    KTEST_CHECK(g_sem_passed == 1);
    g_sem.up();
    g_sem.up();
    for (u32 i = 0; i < 3; i++) thread_join(threads[i]);
    KTEST_CHECK(g_sem_passed == 3 && !g_sem.try_down());

    // --- reader-writer lock: readers overlap, a writer is always alone ---
    g_rw_max_readers = 0;
    g_rw_violation = false;
    for (u32 i = 0; i < 4; i++) {
        Result<Thread*> t = kthread_create(i < 3 ? rw_reader : rw_writer, nullptr, i < 3 ? "reader" : "writer");
        KTEST_CHECK(t.ok());
        threads[i] = t.value();
    }
    for (u32 i = 0; i < 4; i++) thread_join(threads[i]);
    KTEST_CHECK(!g_rw_violation && g_rw_max_readers >= 2);
    kprintf("  semaphore and reader-writer lock: correct (up to %d readers inside at once, writers alone)\n",
            g_rw_max_readers);

    // --- producer/consumer over a 16-slot buffer ---
    g_chan = Channel{};
    g_produced[0] = g_produced[1] = g_consumed[0] = g_consumed[1] = Tally{0, 0};
    g_chan.deadline_us = refclock_now_us() + seconds * 1000000;
    const char* names[4] = {"producer", "producer", "consumer", "consumer"};
    for (u32 i = 0; i < 4; i++) {
        Result<Thread*> t = i < 2 ? kthread_create(producer, &g_produced[i], names[i])
                                  : kthread_create(consumer, &g_consumed[i - 2], names[i]);
        KTEST_CHECK(t.ok());
        threads[i] = t.value();
    }
    thread_join(threads[0]);
    thread_join(threads[1]);
    chan_put(SENTINEL);                     // one per consumer, after every real item
    chan_put(SENTINEL);
    thread_join(threads[2]);
    thread_join(threads[3]);
    u64 made = g_produced[0].items + g_produced[1].items, made_sum = g_produced[0].sum + g_produced[1].sum;
    u64 got = g_consumed[0].items + g_consumed[1].items, got_sum = g_consumed[0].sum + g_consumed[1].sum;
    KTEST_CHECK(made > 0 && made == got && made_sum == got_sum && g_chan.count == 0);
    kprintf("  producer/consumer: %lu s, %lu items produced and %lu consumed, sums match, no deadlock\n",
            (unsigned long)seconds, (unsigned long)made, (unsigned long)got);

    // --- two processes, one virtual address, two different values ---
    g_space_mismatch = false;
    Process* procs[2];
    for (u32 i = 0; i < 2; i++) {
        Result<Process*> p = process_create(i ? "space-b" : "space-a");
        KTEST_CHECK(p.ok());
        procs[i] = p.value();
        KTEST_CHECK(procs[i]->space->mmap(PRIVATE_ADDR, PAGE_SIZE, vm::WRITE, mmap_flag::FIXED).ok());
        Result<Thread*> t = kthread_create(space_worker, (void*)(u64)(0xA0 + i), "space-worker", prio::NORMAL, procs[i]);
        KTEST_CHECK(t.ok());
        threads[i] = t.value();
    }
    thread_join(threads[0]);
    thread_join(threads[1]);
    KTEST_CHECK(!g_space_mismatch);
    process_destroy(procs[0]);
    process_destroy(procs[1]);
    kprintf("  address spaces: two processes switched 4000 times, each always read its own value\n");

    // --- latency and switch cost ---
    // The budget (SPEC §20.1: 1 ms, 2 us) is checked on the average. The
    // worst single sample is reported but not checked: it includes whatever
    // the host did to this virtual CPU and any compositor pass that was
    // already running at the same level.
    WakeLatency wake = kbench_wake_latency(200);
    u64 switch_ns = kbench_context_switch_ns(50000);
    kprintf("  wake-up latency: average %lu us, worst %lu us (budget 1000); context switch: %lu ns (budget 2000)\n",
            (unsigned long)wake.avg_us, (unsigned long)wake.max_us, (unsigned long)switch_ns);
    KTEST_CHECK(wake.ok && wake.avg_us < 1000);
    KTEST_CHECK(switch_ns > 0 && switch_ns < 2000);

    // --- the desktop kept running, and nothing was left behind ---
    if (gui_active()) {
        u64 frames = gui_stats().frames - frames0;
        KTEST_CHECK(frames >= seconds / 2);
        kprintf("  desktop: the compositor presented %lu frames while the tests ran\n", (unsigned long)frames);
    }
    thread_sleep_ticks(3);                  // let the reaper finish with detached helpers, if any
    SchedStats s1 = sched_stats();
    KheapStats h1 = kheap_stats();
    KTEST_CHECK(s1.threads == s0.threads);
    KTEST_CHECK(h1.live_objects == h0.live_objects);
    kprintf("  cleanup: %u threads before and after, no allocation left behind; %lu context switches in total\n",
            s1.threads, (unsigned long)(s1.context_switches - s0.context_switches));
    return 0;
}
