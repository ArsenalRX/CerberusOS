// High Precision Event Timer: a free-running 64-bit counter with a known
// period, discovered through the ACPI "HPET" table. Used as the time
// reference for calibrating the APIC timer and for short busy-waits; the
// PIT is the fallback when no HPET exists. Interrupts from the HPET are not
// used.
#pragma once

#include <lib/types.h>

// Maps and starts the main counter. Returns false (and logs why) if the ACPI
// table is missing or the hardware looks broken. Requires acpi_init.
bool hpet_init();
bool hpet_available();
u64 hpet_frequency_hz();
u64 hpet_counter();
// Busy-waits for the given duration. Safe with interrupts on or off.
void hpet_sleep_us(u64 microseconds);
static inline void hpet_sleep_ms(u64 ms) { hpet_sleep_us(ms * 1000); }
// Elapsed microseconds between two counter readings.
u64 hpet_delta_us(u64 start, u64 end);
