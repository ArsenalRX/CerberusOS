// Micro-benchmarks for the budgets in SPEC §20.1 that exist so far. Used by
// the `bench` shell command (`make bench`) and by `test sched`. Each function
// runs in thread context and returns a measurement; none prints.
#pragma once

#include <lib/types.h>

// Nanoseconds per thread switch: two threads yield to each other `rounds`
// times each. 0 on failure.
u64 kbench_context_switch_ns(u32 rounds);

struct WakeLatency {
    u64 avg_us;
    u64 max_us;
    bool ok;
};
// Time from wake_one() to the woken INTERACTIVE thread running, over
// `rounds` wake-ups.
WakeLatency kbench_wake_latency(u32 rounds);

// Nanoseconds per kmalloc(64)+kfree pair.
u64 kbench_kmalloc_pair_ns(u32 rounds);

// Nanoseconds per demand-paged first touch of a page (fault, zeroed frame,
// map). 0 on failure.
u64 kbench_page_fault_ns(u32 pages);

// Share of timer ticks (0-100) spent outside the idle thread while the
// caller sleeps for `ms`.
u32 kbench_busy_percent(u64 ms);

// Runs everything and prints one "bench: name=value unit" line per metric.
int kbench_run();
