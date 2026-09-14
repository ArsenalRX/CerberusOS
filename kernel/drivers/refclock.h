// Boot-time reference clock for timer calibration and busy-waits: the HPET
// when present, otherwise PIT channel 0 read as a free-running counter.
// The PIT variant extends the 16-bit counter in software, so refclock_now_us
// must be called at least every ~50 ms to stay monotonic; calibration loops
// poll it continuously, which satisfies that. Not interrupt-safe.
#pragma once

#include <lib/types.h>

// Chooses the source. Requires acpi_init (and hpet_init if an HPET exists).
void refclock_init();
const char* refclock_name();
u64 refclock_now_us();
void refclock_sleep_us(u64 microseconds);
static inline void refclock_sleep_ms(u64 ms) { refclock_sleep_us(ms * 1000); }
