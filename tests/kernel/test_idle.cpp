// Diagnoses timer delivery while the CPU is halted. Measures the tick rate
// spinning, then in hlt, for N seconds each (default 2) against the reference
// clock. Hypervisors have been seen to drop periodic APIC interrupts to a
// halted vCPU; this test makes that visible in the log.
#include <arch/x86_64/cpu.h>
#include <drivers/lapic.h>
#include <drivers/refclock.h>
#include <kernel/ktest.h>
#include <lib/kprintf.h>

namespace {

u64 rate_x10(bool halt, u64 seconds) {
    u64 t0 = lapic_timer_ticks();
    u64 s = refclock_now_us();
    u64 end = s + seconds * 1000000;
    u64 now = s;
    while (now < end) {
        if (halt) cpu_halt();
        else cpu_relax();
        now = refclock_now_us();
    }
    u64 us = now - s;
    return us ? (lapic_timer_ticks() - t0) * 10000000 / us : 0;
}

u64 parse_seconds(const char* s) {
    u64 v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (u64)(*s++ - '0');
    return v ? v : 2;
}

} // namespace

int ktest_idle(int argc, char** argv) {
    KTEST_CHECK(interrupts_enabled());
    u64 seconds = argc > 1 ? parse_seconds(argv[1]) : 2;
    u64 spin = rate_x10(false, seconds);
    u64 halt = rate_x10(true, seconds);
    kprintf("  over %lu s each: spinning %lu.%lu Hz, halted %lu.%lu Hz (target %u, by %s)\n",
            (unsigned long)seconds, (unsigned long)(spin / 10), (unsigned long)(spin % 10),
            (unsigned long)(halt / 10), (unsigned long)(halt % 10), TIMER_HZ, refclock_name());
    u64 lo = TIMER_HZ * 10 - TIMER_HZ, hi = TIMER_HZ * 10 + TIMER_HZ;   // +-10%
    KTEST_CHECK(spin >= lo && spin <= hi);
    KTEST_CHECK(halt >= lo && halt <= hi);
    return 0;
}
