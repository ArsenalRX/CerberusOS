// Verifies the APIC timer is running at the configured rate by comparing the
// tick counter against the boot reference clock (HPET, or the PIT counter).
#include <arch/x86_64/cpu.h>
#include <drivers/lapic.h>
#include <drivers/refclock.h>
#include <kernel/ktest.h>
#include <lib/kprintf.h>

int ktest_timer(int, char**) {
    KTEST_CHECK(interrupts_enabled());
    u64 t0 = lapic_timer_ticks();
    refclock_sleep_ms(500);
    u64 t1 = lapic_timer_ticks();
    u64 delta = t1 - t0;
    kprintf("  %lu ticks in 500 ms by %s (expect ~%u)\n", (unsigned long)delta, refclock_name(),
            TIMER_HZ / 2);
    // Allow 10% slack: VM timing under load is not exact.
    KTEST_CHECK(delta >= TIMER_HZ / 2 - TIMER_HZ / 20);
    KTEST_CHECK(delta <= TIMER_HZ / 2 + TIMER_HZ / 20);
    return 0;
}
