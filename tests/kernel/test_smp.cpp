// Phase 8 acceptance (SPEC §5 and §5A): every CPU runs threads; a counter
// incremented under a spinlock by all CPUs at once comes out exact; one CPU
// unmaps a page while another is reading it and the reader faults at once
// (TLB shootdown); idle CPUs take waiting threads from a busy one (work
// stealing); the heap survives every CPU allocating at once and freeing each
// other's memory; and the scheduler benchmarks run. In debug builds all of
// it runs under the lock-rank checker (lib/lock_order.h), which panics on a
// violation, so reaching the end means there was none.
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <drivers/refclock.h>
#include <kernel/kbench.h>
#include <kernel/ktest.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/usercopy.h>
#include <mm/vmm.h>
#include <sched/sched.h>
#include <sched/sync.h>

namespace {

constexpr u32 MAX_TEST_CPUS = 8;        // the parts that use one thread per CPU stop at this many

// ---------------------------------------------------------- one per CPU ---
struct PinCtx {
    u32 cpu;
    u32 ran_on;
};

void pinned_reporter(void* arg) {
    PinCtx* c = (PinCtx*)arg;
    thread_set_affinity(1u << c->cpu);
    c->ran_on = thread_cpu();
}

// -------------------------------------------------------- shared counter ---
constexpr u64 INCREMENTS = 1000000;
Spinlock g_counter_lock;
u64 g_locked_counter;
volatile u64 g_unlocked_counter;
u32 g_start_gate;                       // the threads start counting together

void counter_thread(void* arg) {
    thread_set_affinity(1u << (u32)(u64)arg);
    while (!__atomic_load_n(&g_start_gate, __ATOMIC_ACQUIRE)) thread_yield();
    for (u64 i = 0; i < INCREMENTS; i++) {
        g_counter_lock.lock();
        g_locked_counter++;
        g_counter_lock.unlock();
        g_unlocked_counter = g_unlocked_counter + 1;    // deliberately unprotected, for contrast
    }
}

// -------------------------------------------------------- TLB shootdown ---
constexpr vaddr_t SHOT_ADDR = 0x60000000ull;
struct ShootCtx {
    u32 cpu;
    volatile u64 reads;         // successful reads so far
    volatile bool unmapped;     // set by the main thread once munmap has returned
    volatile u64 stale;         // reads that succeeded although they began after that
    volatile bool faulted;
    volatile bool setup_failed;
};

// Runs in the test process on another CPU: reads one page over and over, so
// its translation is as cached as it can be, until a read fails.
void shoot_reader(void* arg) {
    ShootCtx* c = (ShootCtx*)arg;
    thread_set_affinity(1u << c->cpu);
    u64 value = 0x5348305444574E21ull;
    if (!copy_to_user(SHOT_ADDR, &value, sizeof value).ok()) {
        c->setup_failed = true;
        return;
    }
    for (;;) {
        bool was_unmapped = c->unmapped;
        u64 seen = 0;
        if (!copy_from_user(&seen, SHOT_ADDR, sizeof seen).ok()) {
            c->faulted = true;
            return;
        }
        if (was_unmapped || seen != value) c->stale = c->stale + 1;
        c->reads = c->reads + 1;
    }
}

// --------------------------------------------------------- work stealing ---
struct BusyCtx {
    u32 ran_on; // bit per CPU the thread was seen running on
};

void busy_thread(void* arg) {
    BusyCtx* c = (BusyCtx*)arg;
    // Measured in timer ticks spent running (see counting_worker in test_sched).
    u64 until = thread_current()->run_ticks + 6;
    while (__atomic_load_n(&thread_current()->run_ticks, __ATOMIC_RELAXED) < until) c->ran_on |= 1u << thread_cpu();
}

// ------------------------------------------------------------------ heap ---
constexpr u32 HEAP_SLOTS = 256;
constexpr u32 HEAP_ROUNDS = 40000;
struct HeapSlot {
    u8* ptr;
    u32 size;
    u8 fill;
};
HeapSlot g_heap_slots[HEAP_SLOTS];
Spinlock g_heap_slots_lock;
volatile bool g_heap_corrupt;

bool filled_with(const u8* p, usize n, u8 value) {
    for (usize i = 0; i < n; i++)
        if (p[i] != value) return false;
    return true;
}

// Each round: allocate a block, swap it into a shared slot, and free
// whatever was there, which another CPU probably allocated.
void heap_thread(void* arg) {
    u32 cpu = (u32)(u64)arg;
    thread_set_affinity(1u << cpu);
    u64 rng = 0x9E3779B97F4A7C15ull * (cpu + 1);
    for (u32 i = 0; i < HEAP_ROUNDS; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        u32 size = (rng >> 8) % 16 == 0 ? 2049 + (u32)((rng >> 16) % 9000) : 1 + (u32)((rng >> 16) % 2048);
        u8 fill = (u8)(rng >> 40);
        u8* fresh = (u8*)kmalloc(size);
        if (!fresh) {
            g_heap_corrupt = true;
            return;
        }
        memset(fresh, fill, size);
        HeapSlot mine{fresh, size, fill};
        g_heap_slots_lock.lock();
        HeapSlot& slot = g_heap_slots[(rng >> 48) % HEAP_SLOTS];
        HeapSlot old = slot;
        slot = mine;
        g_heap_slots_lock.unlock();
        if (old.ptr) {
            if (!filled_with(old.ptr, old.size, old.fill) || !kheap_check(old.ptr)) g_heap_corrupt = true;
            kfree(old.ptr);
        }
    }
}

// ----------------------------------------------------------------- mutex ---
Mutex g_mutex;
u64 g_mutex_counter;
void mutex_thread(void*) {
    for (u32 i = 0; i < 50000; i++) {
        g_mutex.lock();
        g_mutex_counter++;
        g_mutex.unlock();
    }
}

} // namespace

int ktest_smp(int, char**) {
    u32 cpus = smp_cpu_count();
    u32 present = 0;
    for (u32 i = 0; i < cpus; i++) present += smp_cpu_present(i);
    kprintf("  %u CPU(s) in use\n", present);
    if (present < 2) {
        kprintf("  only one CPU: nothing to test (run with more, e.g. qemu -smp 4)\n");
        return 0;
    }
    KTEST_CHECK(present == cpus && cpus <= MAX_CPUS);
    u32 n = cpus < MAX_TEST_CPUS ? cpus : MAX_TEST_CPUS;
    SchedStats s0 = sched_stats();
    KheapStats h0 = kheap_stats();
    Thread* threads[2 * MAX_TEST_CPUS];

    // --- every CPU runs a thread that asks for it ---
    static PinCtx pins[MAX_TEST_CPUS];
    for (u32 i = 0; i < n; i++) {
        pins[i] = {i, ~0u};
        Result<Thread*> t = kthread_create(pinned_reporter, &pins[i], "smp-pin");
        KTEST_CHECK(t.ok());
        threads[i] = t.value();
    }
    for (u32 i = 0; i < n; i++) thread_join(threads[i]);
    for (u32 i = 0; i < n; i++) KTEST_CHECK(pins[i].ran_on == i);
    kprintf("  affinity: a thread pinned to each of %u CPUs ran on exactly that CPU\n", n);

    // --- spinlock: every CPU increments one counter 1,000,000 times ---
    g_locked_counter = 0;
    g_unlocked_counter = 0;
    g_start_gate = 0;
    for (u32 i = 0; i < n; i++) {
        Result<Thread*> t = kthread_create(counter_thread, (void*)(u64)i, "smp-count");
        KTEST_CHECK(t.ok());
        threads[i] = t.value();
    }
    u64 c0 = refclock_now_us();
    __atomic_store_n(&g_start_gate, 1u, __ATOMIC_RELEASE);
    for (u32 i = 0; i < n; i++) thread_join(threads[i]);
    u64 c1 = refclock_now_us();
    u64 expected = (u64)n * INCREMENTS;
    KTEST_CHECK(g_locked_counter == expected);
    kprintf("  spinlock: %u CPUs x %lu increments = %lu, exact (%lu ms); the same count without the lock lost %lu\n",
            n, (unsigned long)INCREMENTS, (unsigned long)g_locked_counter, (unsigned long)((c1 - c0) / 1000),
            (unsigned long)(expected - g_unlocked_counter));

    // --- TLB shootdown: unmap on this CPU while another CPU is reading ---
    Result<Process*> made = process_create("tlb-test");
    KTEST_CHECK(made.ok());
    Process* proc = made.value();
    KTEST_CHECK(proc->space->mmap(SHOT_ADDR, PAGE_SIZE, vm::WRITE, mmap_flag::FIXED).ok());
    u32 me = thread_cpu();
    thread_set_affinity(1u << me);              // stay put, so "another CPU" stays another
    static ShootCtx shoot;
    memset(&shoot, 0, sizeof shoot);
    shoot.cpu = (me + 1) % cpus;
    Result<Thread*> reader = kthread_create(shoot_reader, &shoot, "tlb-reader", prio::NORMAL, proc);
    KTEST_CHECK(reader.ok());
    u64 deadline = refclock_now_us() + 3000000;
    while (shoot.reads < 20000 && !shoot.setup_failed && refclock_now_us() < deadline) thread_yield();
    KTEST_CHECK(!shoot.setup_failed && shoot.reads >= 20000);
    u64 shots0 = smp_tlb_shootdowns();
    KTEST_CHECK(proc->space->munmap(SHOT_ADDR, PAGE_SIZE).ok());
    shoot.unmapped = true;
    thread_join(reader.value());
    thread_set_affinity(~0u);
    u64 shots = smp_tlb_shootdowns() - shots0;
    UsercopyFault uf = usercopy_last_fault();
    KTEST_CHECK(shoot.faulted && shoot.stale == 0 && shots >= 1);
    KTEST_CHECK(uf.addr == SHOT_ADDR);
    process_destroy(proc);
    kprintf("  TLB shootdown: cpu %u unmapped a page cpu %u had read %lu times; its next read faulted at %#lx, "
            "none got through\n",
            me, shoot.cpu, (unsigned long)shoot.reads, (unsigned long)uf.addr);

    // --- work stealing: more busy threads than CPUs, started from one CPU ---
    SchedStats w0 = sched_stats();
    u64 tk0[MAX_TEST_CPUS], id0[MAX_TEST_CPUS];
    for (u32 i = 0; i < n; i++) sched_cpu_ticks(i, &tk0[i], &id0[i]);
    u64 wt0 = refclock_now_us();
    static BusyCtx busy[2 * MAX_TEST_CPUS];
    for (u32 i = 0; i < 2 * n; i++) {
        busy[i].ran_on = 0;
        Result<Thread*> t = kthread_create(busy_thread, &busy[i], "smp-busy");
        KTEST_CHECK(t.ok());
        threads[i] = t.value();
    }
    for (u32 i = 0; i < 2 * n; i++) thread_join(threads[i]);
    u32 used_mask = 0;
    for (u32 i = 0; i < 2 * n; i++) used_mask |= busy[i].ran_on;
    u32 used = 0;
    for (u32 i = 0; i < MAX_CPUS; i++) used += (used_mask >> i) & 1;
    u64 steals = sched_stats().steals - w0.steals;
    if (used != n || steals < 1) {
        kprintf("  work stealing: %u threads ran on %u CPUs (mask %#x), %lu steal(s) in %lu ms\n", 2 * n, used,
                used_mask, (unsigned long)steals, (unsigned long)((refclock_now_us() - wt0) / 1000));
        for (u32 i = 0; i < n; i++) {
            u64 tk, id;
            sched_cpu_ticks(i, &tk, &id);
            kprintf("    cpu %u: %lu ticks, %lu idle\n", i, (unsigned long)(tk - tk0[i]), (unsigned long)(id - id0[i]));
        }
        sched_print_threads();
    }
    KTEST_CHECK(used == n && steals >= 1);
    kprintf("  work stealing: %u busy threads ran on %u different CPUs; idle CPUs took %lu thread(s) from "
            "busy ones\n",
            2 * n, used, (unsigned long)steals);

    // --- heap: every CPU allocates at once and frees the others' blocks ---
    memset(g_heap_slots, 0, sizeof g_heap_slots);
    g_heap_corrupt = false;
    u64 h_t0 = refclock_now_us();
    for (u32 i = 0; i < n; i++) {
        Result<Thread*> t = kthread_create(heap_thread, (void*)(u64)i, "smp-heap");
        KTEST_CHECK(t.ok());
        threads[i] = t.value();
    }
    for (u32 i = 0; i < n; i++) thread_join(threads[i]);
    u64 h_t1 = refclock_now_us();
    for (HeapSlot& s : g_heap_slots) {
        if (!s.ptr) continue;
        if (!filled_with(s.ptr, s.size, s.fill) || !kheap_check(s.ptr)) g_heap_corrupt = true;
        kfree(s.ptr);
        s.ptr = nullptr;
    }
    KTEST_CHECK(!g_heap_corrupt);
    kprintf("  heap: %u CPUs x %u allocations, each freeing blocks the others allocated: every block intact "
            "(%lu ms)\n",
            n, HEAP_ROUNDS, (unsigned long)((h_t1 - h_t0) / 1000));

    // --- sleeping lock across CPUs, and the scheduler benchmarks ---
    g_mutex_counter = 0;
    for (u32 i = 0; i < n; i++) {
        Result<Thread*> t = kthread_create(mutex_thread, nullptr, "smp-mutex");
        KTEST_CHECK(t.ok());
        threads[i] = t.value();
    }
    for (u32 i = 0; i < n; i++) thread_join(threads[i]);
    KTEST_CHECK(g_mutex_counter == (u64)n * 50000);
    WakeLatency wake = kbench_wake_latency(200);
    u64 switch_ns = kbench_context_switch_ns(50000);
    u64 pair_ns = kbench_kmalloc_pair_ns(100000);
    // Only sanity bounds here: the budgets (1 ms wake-up, 2 µs switch) are
    // defined for QEMU/KVM and measured by `make bench`; VirtualBox on this
    // host has a worst-case wake-up of several milliseconds.
    if (!(wake.ok && wake.avg_us < 20000 && switch_ns > 0 && switch_ns < 20000 && pair_ns > 0))
        kprintf("  benchmarks out of range: wake ok=%d avg %lu us, switch %lu ns, kmalloc+kfree %lu ns\n", (int)wake.ok,
                (unsigned long)wake.avg_us, (unsigned long)switch_ns, (unsigned long)pair_ns);
    KTEST_CHECK(wake.ok && wake.avg_us < 20000 && switch_ns > 0 && switch_ns < 20000 && pair_ns > 0);
    kprintf("  mutex: %u threads made %lu increments, none lost; wake-up %lu us, context switch %lu ns, "
            "kmalloc+kfree %lu ns\n",
            n, (unsigned long)g_mutex_counter, (unsigned long)wake.avg_us, (unsigned long)switch_ns,
            (unsigned long)pair_ns);
#ifdef LUMEN_DEBUG
    kprintf("  lock order: every spinlock taken above was checked against kernel/lib/lock_order.h; no violation\n");
#else
    kprintf("  lock order: the checker is only built into debug kernels\n");
#endif

    // --- nothing left behind ---
    thread_sleep_ticks(3);
    SchedStats s1 = sched_stats();
    KheapStats h1 = kheap_stats();
    KTEST_CHECK(s1.threads == s0.threads);
    KTEST_CHECK(h1.live_objects == h0.live_objects);
    kprintf("  cleanup: %u threads before and after, no allocation left behind\n", s1.threads);
    return 0;
}
