// Local APIC: enable, end-of-interrupt, and the per-CPU timer. The timer is
// calibrated at boot in two steps: an estimate from sampling the count
// register against the reference clock, then a closed-loop correction that
// measures the real periodic interrupt rate and adjusts the reload count
// (some hypervisors report inconsistent count-register values). One-shot mode
// is available for later use by the scheduler. All functions touch only this
// CPU's APIC.
#pragma once

#include <lib/types.h>

constexpr u32 TIMER_HZ = 100;

// Maps the register page (from the MADT), enables the APIC via the spurious
// vector register, masks the LVT entries. Requires acpi_init and
// interrupts_init. Not interrupt-safe.
void lapic_init();
u32 lapic_id();
void lapic_eoi();
// Enables the APIC of another CPU (the register page is already mapped).
void lapic_init_cpu();
// Starts another CPU's periodic tick at the rate the bootstrap CPU settled on.
void lapic_timer_start_cpu();
// Sends interrupt `vector` to the CPU with this APIC id and waits until it
// has been accepted for delivery. Interrupt-safe.
void lapic_send_ipi(u32 apic_id, u8 vector);
// A non-maskable interrupt to every other CPU (panic).
void lapic_send_nmi_to_others();

// Step 1: estimate ticks per millisecond from the count register (~30 ms).
void lapic_timer_calibrate();
u32 lapic_timer_ticks_per_ms();
// Starts the periodic tick at hz; the handler is installed on vec::APIC_TIMER.
void lapic_timer_set_periodic(u32 hz);
// Step 2: with interrupts enabled and the periodic timer running, measure the
// real rate against the reference clock and correct the reload count until it
// is within 2% of hz (a few hundred ms). Returns the measured rate x10.
u64 lapic_timer_tune(u32 hz);
// Switches the tick between hardware periodic mode and one-shot mode that the
// handler re-arms every tick (the same rate). Exists to compare hypervisor
// behaviour; the scheduler may later prefer one.
void lapic_timer_set_rearm_mode(bool rearm);
bool lapic_timer_rearm_mode();
// Fires vec::APIC_TIMER once after the given delay, then stops.
void lapic_timer_set_oneshot(u32 microseconds);
void lapic_timer_stop();

// Ticks since the periodic timer started. Safe to read anywhere.
u64 lapic_timer_ticks();
// Optional hook invoked from the timer interrupt (scheduler preemption later).
using TickHook = void (*)();
void lapic_timer_set_hook(TickHook hook);
