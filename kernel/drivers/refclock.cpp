// Reference clock. Preferred source: the CPU's time-stamp counter, calibrated
// once at boot against the HPET (or the PIT when there is no HPET). Reading
// it is one instruction with no device access, so it is cheap, safe on every
// CPU at once and needs no lock. Fallbacks: a direct HPET counter read, or
// PIT channel 0 read as a free-running counter (mode 2, 65536 reload).
//
// Why the PIT is the last choice: reading it takes three port accesses that
// must not interleave between CPUs (until 2026-10-04 they could, and on
// VirtualBox, which has no HPET, the clock then raced ahead several times
// over), every read is a slow VM exit, and the 16-bit count wraps every
// ~55 ms, so time is lost if nobody reads it for longer than that.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpufeatures.h>
#include <arch/x86_64/io.h>
#include <drivers/hpet.h>
#include <drivers/pit.h>
#include <drivers/refclock.h>
#include <lib/kprintf.h>

namespace {

enum class Source { Pit, Hpet, Tsc };
Source g_source = Source::Pit;
u64 g_hpet_base = 0;

// TSC state: microseconds = (tsc - base) * mult >> 32.
u64 g_tsc_base = 0;
u64 g_tsc_mult = 0;
u64 g_tsc_hz = 0;
u64 g_last_us = 0;          // highest value handed out; keeps the clock monotonic across CPUs

// PIT state, under g_pit_lock (interrupts off while held).
constexpr u16 PIT_CH0 = 0x40;
constexpr u16 PIT_CMD = 0x43;
u16 g_pit_prev = 0;
u64 g_pit_ticks = 0;       // total elapsed PIT ticks since refclock_init
volatile u32 g_pit_lock = 0;

u16 pit_read_count() {
    outb(PIT_CMD, 0x00);
    u8 lo = inb(PIT_CH0);
    u8 hi = inb(PIT_CH0);
    return (u16)(lo | (hi << 8));
}

u64 pit_now_us() {
    // A raw lock rather than a Spinlock: it takes no other lock, holds for a
    // few port accesses, and the clock must keep working inside a panic.
    u64 irq = interrupts_save();
    while (__atomic_exchange_n(&g_pit_lock, 1u, __ATOMIC_ACQUIRE)) cpu_relax();
    u16 cur = pit_read_count();
    g_pit_ticks += (u32)(g_pit_prev - cur) % 65536;   // counts down, wraps at 0
    g_pit_prev = cur;
    u64 us = g_pit_ticks * 1000000 / PIT_FREQUENCY_HZ;
    __atomic_store_n(&g_pit_lock, 0u, __ATOMIC_RELEASE);
    interrupts_restore(irq);
    return us;
}

u64 calibration_now_us() { return g_source == Source::Hpet ? hpet_delta_us(g_hpet_base, hpet_counter()) : pit_now_us(); }

} // namespace

void refclock_init() {
    if (hpet_available()) {
        g_source = Source::Hpet;
        g_hpet_base = hpet_counter();
    } else {
        g_source = Source::Pit;
        outb(PIT_CMD, 0x34);        // channel 0, lo/hi, mode 2 (rate generator), binary
        outb(PIT_CH0, 0);
        outb(PIT_CH0, 0);           // reload 65536
        g_pit_prev = pit_read_count();
        g_pit_ticks = 0;
    }

    // The TSC is usable when the CPU says its rate is constant (invariant
    // TSC), and under a hypervisor, which presents a constant-rate TSC even
    // when it hides the flag. Measure its rate over 50 ms.
    if (!g_cpu.invariant_tsc && !g_cpu.hypervisor) {
        kprintf("refclock: %s (no invariant TSC)\n", refclock_name());
        return;
    }
    u64 s0 = calibration_now_us();
    u64 t0 = rdtsc();
    u64 s1;
    do s1 = calibration_now_us();
    while (s1 - s0 < 50000);
    u64 t1 = rdtsc();
    u64 hz = (t1 - t0) * 1000000 / (s1 - s0);
    if (hz < 100000000) {           // under 100 MHz: not believable, keep the device clock
        kprintf("refclock: %s (TSC measured at %lu Hz, not used)\n", refclock_name(), (unsigned long)hz);
        return;
    }
    g_tsc_hz = hz;
    g_tsc_mult = (1000000ull << 32) / hz;                  // fits: 1e6 * 2^32 < 2^64
    g_tsc_base = t1 - (s1 << 32) / g_tsc_mult;             // continue from the device clock (s1 is seconds into boot)
    Source calibrated_by = g_source;
    g_source = Source::Tsc;
    kprintf("refclock: tsc at %lu MHz (%s), calibrated against the %s\n", (unsigned long)(hz / 1000000),
            g_cpu.invariant_tsc ? "invariant" : "hypervisor", calibrated_by == Source::Hpet ? "hpet" : "pit");
}

const char* refclock_name() {
    switch (g_source) {
    case Source::Tsc: return "tsc";
    case Source::Hpet: return "hpet";
    default: return "pit";
    }
}

u64 refclock_now_us() {
    if (g_source == Source::Hpet) return hpet_delta_us(g_hpet_base, hpet_counter());
    if (g_source == Source::Pit) return pit_now_us();
    u64 us = (u64)(((unsigned __int128)(rdtsc() - g_tsc_base) * g_tsc_mult) >> 32);
    // The CPUs' counters may differ by a few cycles; never go backwards when
    // a thread reads the clock on one CPU and then on another.
    u64 last = __atomic_load_n(&g_last_us, __ATOMIC_RELAXED);
    while (us > last && !__atomic_compare_exchange_n(&g_last_us, &last, us, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    return us > last ? us : last;
}

void refclock_sleep_us(u64 microseconds) {
    u64 start = refclock_now_us();
    while (refclock_now_us() - start < microseconds) asm volatile("pause");
}
