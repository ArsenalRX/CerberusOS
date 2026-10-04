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
#include <arch/x86_64/acpi.h>
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
    // TSC), and under KVM, which presents one constant-rate counter to every
    // virtual CPU even when it hides the flag. Other hypervisors are not
    // trusted: VirtualBox on a Hyper-V host gives each virtual CPU a counter
    // of its own, and a clock built on them stalls whenever a thread changes
    // CPU (seen 2026-10-04: 58 timer ticks in "500 ms"). Measure the rate
    // over 50 ms.
    bool kvm = g_cpu.hypervisor && __builtin_memcmp(g_cpu.hv_vendor, "KVMKVMKVM", 9) == 0;
    // VirtualBox does not always set the CPUID hypervisor bit, passes the
    // host's invariant flag through, and its counter still cannot be trusted
    // (rates of 7, 1,345 and 1,530 MHz measured on one 4,200 MHz CPU): it is
    // recognised by its firmware tables instead. Inside any other virtual
    // machine than KVM the invariant flag is not believed either.
    bool vbox = __builtin_memcmp(acpi_oem_id(), "VBOX", 4) == 0;
    bool trusted = !vbox && (g_cpu.hypervisor ? kvm : g_cpu.invariant_tsc);
    if (!trusted) {
        kprintf("refclock: %s (no trustworthy TSC%s)\n", refclock_name(),
                vbox ? " under VirtualBox" : g_cpu.hypervisor ? " under this hypervisor" : "");
        return;
    }
    // Three 20 ms measurements must agree within 1%: a counter that does
    // not tick steadily against the device clock is not used.
    u64 rates[3], t1 = 0, s1 = 0;
    for (u64& rate : rates) {
        u64 s0 = calibration_now_us();
        u64 t0 = rdtsc();
        do s1 = calibration_now_us();
        while (s1 - s0 < 20000);
        t1 = rdtsc();
        rate = (t1 - t0) * 1000000 / (s1 - s0);
    }
    u64 lo = rates[0], hi = rates[0];
    for (u64 rate : rates) {
        if (rate < lo) lo = rate;
        if (rate > hi) hi = rate;
    }
    u64 hz = (rates[0] + rates[1] + rates[2]) / 3;
    if (lo < 100000000 || hi - lo > lo / 100) {
        kprintf("refclock: %s (TSC measured at %lu to %lu kHz, not steady enough to use)\n", refclock_name(),
                (unsigned long)(lo / 1000), (unsigned long)(hi / 1000));
        return;
    }
    g_tsc_hz = hz;
    g_tsc_mult = (1000000ull << 32) / hz;                  // fits: 1e6 * 2^32 < 2^64
    g_tsc_base = t1 - (s1 << 32) / g_tsc_mult;             // continue from the device clock (s1 is seconds into boot)
    Source calibrated_by = g_source;
    g_source = Source::Tsc;
    kprintf("refclock: tsc at %lu MHz (%s), calibrated against the %s\n", (unsigned long)(hz / 1000000),
            kvm ? "KVM" : "invariant", calibrated_by == Source::Hpet ? "hpet" : "pit");
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

void refclock_poll() {
    if (g_source == Source::Pit) (void)pit_now_us();
}

void refclock_sleep_us(u64 microseconds) {
    u64 start = refclock_now_us();
    while (refclock_now_us() - start < microseconds) asm volatile("pause");
}
