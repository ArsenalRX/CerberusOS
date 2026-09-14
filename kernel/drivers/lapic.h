// Local APIC: enable, end-of-interrupt, and the per-CPU timer. The timer is
// calibrated against the PIT once at boot and then runs periodically at
// TIMER_HZ, incrementing the tick counter; one-shot mode is available for
// later use by the scheduler. All functions touch only this CPU's APIC.
#pragma once

#include <lib/types.h>

constexpr u32 TIMER_HZ = 100;

// Maps the register page (from the MADT), enables the APIC via the spurious
// vector register, masks the LVT entries. Requires acpi_init and
// interrupts_init. Not interrupt-safe.
void lapic_init();
u32 lapic_id();
void lapic_eoi();

// Measures APIC timer ticks per millisecond using the PIT (busy-waits ~30 ms).
void lapic_timer_calibrate();
u32 lapic_timer_ticks_per_ms();
// Starts the periodic tick at hz; the handler is installed on vec::APIC_TIMER.
void lapic_timer_set_periodic(u32 hz);
// Fires vec::APIC_TIMER once after the given delay, then stops.
void lapic_timer_set_oneshot(u32 microseconds);
void lapic_timer_stop();

// Ticks since the periodic timer started. Safe to read anywhere.
u64 lapic_timer_ticks();
// Optional hook invoked from the timer interrupt (scheduler preemption later).
using TickHook = void (*)();
void lapic_timer_set_hook(TickHook hook);
