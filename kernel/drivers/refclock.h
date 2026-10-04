// Reference clock in microseconds since boot, for timer calibration,
// busy-waits, measurements and timestamps. The time-stamp counter calibrated
// against the HPET (or PIT) when the CPU's TSC runs at a constant rate,
// otherwise the HPET, otherwise PIT channel 0 read as a free-running counter.
// Safe to call from any CPU and from interrupt handlers; monotonic. The PIT
// fallback extends a 16-bit counter in software and loses time if it is not
// read for more than ~55 ms.
#pragma once

#include <lib/types.h>

// Chooses the source and calibrates the TSC (takes 50 ms). Requires acpi_init,
// hpet_init if an HPET exists, and cpu_features_init.
void refclock_init();
const char* refclock_name();
u64 refclock_now_us();
void refclock_sleep_us(u64 microseconds);
// Called from the timer tick: keeps the PIT fallback from losing time when
// nothing else reads the clock for a while. Does nothing for other sources.
void refclock_poll();
static inline void refclock_sleep_ms(u64 ms) { refclock_sleep_us(ms * 1000); }
