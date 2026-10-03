// See kbench.h. The clock is the reference clock (HPET when present), read
// only at the start and end of each run so its own cost does not count.
#include <drivers/refclock.h>
#include <gui/desktop.h>
#include <kernel/kbench.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/vmm.h>
#include <sched/sched.h>
#include <sched/sync.h>

namespace {

void yielder(void* arg) {
    u32 rounds = (u32)(u64)arg;
    for (u32 i = 0; i < rounds; i++) thread_yield();
}

struct WakeCtx {
    WaitQueue wq;
    volatile u64 sent_us;
    volatile u64 sum_us;
    volatile u64 max_us;
    volatile u32 seen;
    volatile bool stop;
};

void wake_target(void* arg) {
    WakeCtx* c = (WakeCtx*)arg;
    for (;;) {
        c->wq.wait();
        if (c->stop) return;
        u64 d = refclock_now_us() - c->sent_us;
        c->sum_us = c->sum_us + d;
        if (d > c->max_us) c->max_us = d;
        c->seen = c->seen + 1;
    }
}

} // namespace

u64 kbench_context_switch_ns(u32 rounds) {
    // The caller sleeps in join while the two threads hand the CPU back and
    // forth. Divide by the switches that really happened: a yield with no
    // other thread at the same level does not switch.
    // Same level as the caller, so neither starts until the caller blocks.
    Result<Thread*> a = kthread_create(yielder, (void*)(u64)rounds, "bench-yield-a", prio::NORMAL);
    Result<Thread*> b = kthread_create(yielder, (void*)(u64)rounds, "bench-yield-b", prio::NORMAL);
    if (!a.ok() || !b.ok()) {
        if (a.ok()) thread_join(a.value());
        if (b.ok()) thread_join(b.value());
        return 0;
    }
    u64 s0 = sched_stats().context_switches;
    u64 t0 = refclock_now_us();
    thread_join(a.value());
    thread_join(b.value());
    u64 t1 = refclock_now_us();
    u64 switches = sched_stats().context_switches - s0;
    return switches ? (t1 - t0) * 1000 / switches : 0;
}

WakeLatency kbench_wake_latency(u32 rounds) {
    static WakeCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    Result<Thread*> t = kthread_create(wake_target, &ctx, "bench-wake", prio::INTERACTIVE);
    if (!t.ok()) return {0, 0, false};
    thread_sleep_ticks(1);                  // let it reach its first wait
    for (u32 i = 0; i < rounds; i++) {
        ctx.sent_us = refclock_now_us();
        ctx.wq.wake_one();                  // the target is more urgent: it runs before this returns
        while (ctx.seen <= i) thread_yield();
    }
    ctx.stop = true;
    ctx.wq.wake_one();
    thread_join(t.value());
    return {rounds ? ctx.sum_us / rounds : 0, ctx.max_us, true};
}

u64 kbench_kmalloc_pair_ns(u32 rounds) {
    u64 t0 = refclock_now_us();
    for (u32 i = 0; i < rounds; i++) {
        void* p = kmalloc(64);
        if (!p) return 0;
        kfree(p);
    }
    return (refclock_now_us() - t0) * 1000 / rounds;
}

u64 kbench_page_fault_ns(u32 pages) {
    Result<Process*> made = process_create("bench-fault");
    if (!made.ok()) return 0;
    Process* p = made.value();
    Result<vaddr_t> region = p->space->mmap(0, (usize)pages * PAGE_SIZE, vm::WRITE, 0);
    u64 ns = 0;
    if (region.ok()) {
        AddressSpace& before = vmm_current();
        p->space->activate();
        volatile u8* mem = (volatile u8*)region.value();
        u64 t0 = refclock_now_us();
        for (u32 i = 0; i < pages; i++) mem[(usize)i * PAGE_SIZE] = 1;
        ns = (refclock_now_us() - t0) * 1000 / pages;
        before.activate();
    }
    process_destroy(p);
    return ns;
}

u32 kbench_busy_percent(u64 ms) {
    SchedStats a = sched_stats();
    thread_sleep_ms(ms);
    SchedStats b = sched_stats();
    u64 ticks = b.ticks - a.ticks;
    u64 idle = b.idle_ticks - a.idle_ticks;
    return ticks ? (u32)((ticks - idle) * 100 / ticks) : 0;
}

int kbench_run() {
    kprintf("bench: context_switch=%lu ns\n", (unsigned long)kbench_context_switch_ns(100000));
    WakeLatency w = kbench_wake_latency(500);
    kprintf("bench: wake_latency_avg=%lu us\n", (unsigned long)w.avg_us);
    kprintf("bench: wake_latency_max=%lu us\n", (unsigned long)w.max_us);
    kprintf("bench: kmalloc_kfree_pair=%lu ns\n", (unsigned long)kbench_kmalloc_pair_ns(200000));
    kprintf("bench: minor_page_fault=%lu ns\n", (unsigned long)kbench_page_fault_ns(2048));
    kprintf("bench: idle_cpu_busy=%u %%\n", kbench_busy_percent(3000));
    if (gui_active()) {
        GuiStats g = gui_stats();
        kprintf("bench: last_composite=%lu us\n", (unsigned long)g.last_frame_us);
    }
    kprintf("bench: done\n");
    return 0;
}
