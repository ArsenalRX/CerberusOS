// 8253/8254 programmable interval timer, used only as a known-rate reference
// to calibrate the APIC timer. Channel 2 with the speaker gate is used so no
// interrupt is involved. Busy-waits; never call with the scheduler running.
#pragma once

#include <lib/types.h>

constexpr u32 PIT_FREQUENCY_HZ = 1193182;

// Spins for approximately ms milliseconds (1..50 per call is accurate).
void pit_sleep_ms(u32 ms);
