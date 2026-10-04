// xAPIC (MMIO) mode only. The register page is mapped uncached through
// early_map. x2APIC mode is detected and refused for now so a misconfigured
// firmware fails loudly rather than reading garbage.
//
// Timer calibration: sampling the current-count register gave readings that
// varied 2-3x between samples under VirtualBox even against the HPET, while
// the periodic interrupt itself ran at a steady rate. So the sampled value is
// only a first estimate; lapic_timer_tune() then measures the delivered
// interrupt rate and corrects the reload count in a short loop.
#include <arch/x86_64/acpi.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/percpu.h>
#include <drivers/lapic.h>
#include <drivers/refclock.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <mm/early_map.h>

namespace {

constexpr u32 REG_ID = 0x020;
constexpr u32 REG_VERSION = 0x030;
constexpr u32 REG_TPR = 0x080;
constexpr u32 REG_EOI = 0x0B0;
constexpr u32 REG_SVR = 0x0F0;
constexpr u32 REG_ESR = 0x280;
constexpr u32 REG_ICR_LOW = 0x300;
constexpr u32 REG_ICR_HIGH = 0x310;
constexpr u32 REG_LVT_TIMER = 0x320;
constexpr u32 REG_LVT_THERMAL = 0x330;
constexpr u32 REG_LVT_PERF = 0x340;
constexpr u32 REG_LVT_LINT0 = 0x350;
constexpr u32 REG_LVT_LINT1 = 0x360;
constexpr u32 REG_LVT_ERROR = 0x370;
constexpr u32 REG_TIMER_INIT = 0x380;
constexpr u32 REG_TIMER_CURRENT = 0x390;
constexpr u32 REG_TIMER_DIVIDE = 0x3E0;

constexpr u32 LVT_MASKED = 1 << 16;
constexpr u32 LVT_TIMER_PERIODIC = 1 << 17;
constexpr u32 SVR_ENABLE = 1 << 8;
constexpr u32 DIVIDE_BY_16 = 0x3;
constexpr u64 APIC_BASE_ENABLE = 1 << 11;
constexpr u64 APIC_BASE_X2APIC = 1 << 10;
constexpr u32 ICR_DELIVERY_PENDING = 1 << 12;
constexpr u32 ICR_MODE_NMI = 4 << 8;
constexpr u32 ICR_ALL_BUT_SELF = 3 << 18;

volatile u32* g_regs = nullptr;
u32 g_ticks_per_ms = 0;
u32 g_periodic_count = 0;
volatile u64 g_ticks = 0;
TickHook g_hook = nullptr;
bool g_rearm_mode = false;      // one-shot re-armed from the handler instead of hardware periodic

u32 read(u32 reg) { return g_regs[reg / 4]; }
void write(u32 reg, u32 v) { g_regs[reg / 4] = v; }
void enable_this_cpu();

// Runs on every CPU, each for its own timer. Only the bootstrap CPU's ticks
// are counted as time.
void timer_handler(InterruptFrame*, void*) {
    if (percpu_cpu_id() == 0) g_ticks = g_ticks + 1;
    if (g_rearm_mode) write(REG_TIMER_INIT, g_periodic_count);
    lapic_eoi();
    if (g_hook) g_hook();
}

void spurious_handler(InterruptFrame*, void*) {
    // No EOI for spurious interrupts.
}

void error_handler(InterruptFrame*, void*) {
    write(REG_ESR, 0);
    kprintf("lapic: error interrupt, ESR=%#x\n", read(REG_ESR));
    lapic_eoi();
}

// Measures the delivered tick rate over `ticks` ticks, polling the reference
// clock (busy, so the PIT variant stays monotonic). Returns Hz x 10, or 0 if
// the ticks did not arrive within a generous reference-clock deadline (some
// hypervisor backends starve timer delivery while the guest spins).
u64 measure_rate_x10(u32 ticks) {
    constexpr u64 DEADLINE_US = 3000000;
    u64 begin = refclock_now_us();
    u64 t0 = g_ticks;
    while (g_ticks == t0) {                      // align to a tick edge
        if (refclock_now_us() - begin > DEADLINE_US) return 0;
        cpu_relax();
    }
    u64 start_us = refclock_now_us();
    u64 t1 = g_ticks;
    while (g_ticks < t1 + ticks) {
        if (refclock_now_us() - start_us > DEADLINE_US) return 0;
        cpu_relax();
    }
    u64 us = refclock_now_us() - start_us;
    return us ? (u64)ticks * 10000000 / us : 0;
}

void enable_this_cpu() {
    write(REG_TPR, 0);                      // accept all priorities
    write(REG_LVT_TIMER, LVT_MASKED);
    write(REG_LVT_THERMAL, LVT_MASKED);
    write(REG_LVT_PERF, LVT_MASKED);
    write(REG_LVT_LINT0, LVT_MASKED);
    write(REG_LVT_LINT1, LVT_MASKED);
    write(REG_LVT_ERROR, vec::APIC_ERROR);
    write(REG_ESR, 0);
    write(REG_SVR, SVR_ENABLE | vec::APIC_SPURIOUS);
}

} // namespace

void lapic_init_cpu() {
    u64 base_msr = rdmsr(msr::APIC_BASE);
    if (base_msr & APIC_BASE_X2APIC) PANIC("lapic: a processor is in x2APIC mode (unsupported)");
    if (!(base_msr & APIC_BASE_ENABLE)) wrmsr(msr::APIC_BASE, base_msr | APIC_BASE_ENABLE);
    enable_this_cpu();
}

void lapic_timer_start_cpu() {
    ASSERT_ALWAYS(g_periodic_count);
    write(REG_TIMER_DIVIDE, DIVIDE_BY_16);
    write(REG_LVT_TIMER, vec::APIC_TIMER | LVT_TIMER_PERIODIC);
    write(REG_TIMER_INIT, g_periodic_count);
}

void lapic_send_ipi(u32 apic_id, u8 vector) {
    // The two register writes must not be split by an interrupt handler that
    // sends an interrupt of its own.
    u64 irq = interrupts_save();
    write(REG_ICR_HIGH, apic_id << 24);
    write(REG_ICR_LOW, vector);
    while (read(REG_ICR_LOW) & ICR_DELIVERY_PENDING) cpu_relax();
    interrupts_restore(irq);
}

void lapic_send_nmi_to_others() {
    if (!g_regs) return;
    write(REG_ICR_HIGH, 0);
    write(REG_ICR_LOW, ICR_MODE_NMI | ICR_ALL_BUT_SELF);
}

void lapic_init() {
    u64 base_msr = rdmsr(msr::APIC_BASE);
    if (base_msr & APIC_BASE_X2APIC) PANIC("lapic: firmware left the APIC in x2APIC mode (unsupported)");
    if (!(base_msr & APIC_BASE_ENABLE)) wrmsr(msr::APIC_BASE, base_msr | APIC_BASE_ENABLE);

    paddr_t phys = acpi_madt().lapic_address;
    paddr_t msr_phys = base_msr & 0xFFFFFF000ull;
    if (phys != msr_phys)
        kprintf("lapic: MADT address %#lx differs from IA32_APIC_BASE %#lx; using the MSR\n",
                (unsigned long)phys, (unsigned long)msr_phys);
    g_regs = (volatile u32*)early_map(msr_phys, PAGE_SIZE, MapCache::Uncached);

    enable_this_cpu();

    interrupt_register(vec::APIC_SPURIOUS, spurious_handler);
    interrupt_register(vec::APIC_ERROR, error_handler);
    interrupt_register(vec::APIC_TIMER, timer_handler);

    u32 ver = read(REG_VERSION);
    kprintf("lapic: id %u, version %#x, %u LVT entries, at %#lx\n", lapic_id(), ver & 0xFF,
            ((ver >> 16) & 0xFF) + 1, (unsigned long)msr_phys);
}

u32 lapic_id() { return read(REG_ID) >> 24; }

void lapic_eoi() { write(REG_EOI, 0); }

void lapic_timer_calibrate() {
    write(REG_TIMER_DIVIDE, DIVIDE_BY_16);
    write(REG_LVT_TIMER, vec::APIC_TIMER | LVT_MASKED);
    u32 samples[3];
    for (int i = 0; i < 3; i++) {
        write(REG_TIMER_INIT, 0xFFFFFFFF);
        refclock_sleep_us(10000);
        samples[i] = 0xFFFFFFFF - read(REG_TIMER_CURRENT);
        write(REG_TIMER_INIT, 0);
    }
    // Median of three: robust against a single interrupted or stretched sample.
    u32 lo = min(samples[0], min(samples[1], samples[2]));
    u32 hi = max(samples[0], max(samples[1], samples[2]));
    u32 median = samples[0] + samples[1] + samples[2] - lo - hi;
    g_ticks_per_ms = median / 10;
    if (g_ticks_per_ms == 0) PANIC("lapic: timer calibration produced zero ticks");
    kprintf("lapic: timer estimate %u ticks/ms (divide 16, %s reference; samples %u %u %u)\n",
            g_ticks_per_ms, refclock_name(), samples[0], samples[1], samples[2]);
}

u32 lapic_timer_ticks_per_ms() { return g_ticks_per_ms; }

void lapic_timer_set_periodic(u32 hz) {
    ASSERT_ALWAYS(g_ticks_per_ms && hz);
    g_periodic_count = (u32)((u64)g_ticks_per_ms * 1000 / hz);
    write(REG_TIMER_DIVIDE, DIVIDE_BY_16);
    write(REG_LVT_TIMER, vec::APIC_TIMER | LVT_TIMER_PERIODIC);
    write(REG_TIMER_INIT, g_periodic_count);
}

u64 lapic_timer_tune(u32 hz) {
    ASSERT_ALWAYS(interrupts_enabled() && g_periodic_count);
    u64 target_x10 = (u64)hz * 10;
    u64 measured_x10 = 0;
    int corrections = 0;
    for (int iter = 0; iter < 6; iter++) {
        measured_x10 = measure_rate_x10(hz / 4);        // a quarter second at the target rate
        if (!measured_x10) {
            kprintf("timer: WARNING: ticks stopped arriving during measurement; keeping reload %u\n",
                    g_periodic_count);
            break;
        }
        u64 err = measured_x10 > target_x10 ? measured_x10 - target_x10 : target_x10 - measured_x10;
        if (err * 50 <= target_x10) break;              // within 2%
        // Rate is inversely proportional to the reload count.
        g_periodic_count = (u32)((u64)g_periodic_count * measured_x10 / target_x10);
        g_ticks_per_ms = (u32)((u64)g_periodic_count * hz / 1000);
        corrections++;
        kprintf("timer: measured %lu.%lu Hz, correcting reload count to %u (%u ticks/ms)\n",
                (unsigned long)(measured_x10 / 10), (unsigned long)(measured_x10 % 10), g_periodic_count,
                g_ticks_per_ms);
        write(REG_TIMER_INIT, g_periodic_count);
    }
    kprintf("timer: %u Hz confirmed: measured %lu.%lu Hz by %s after %d correction(s), reload %u\n", hz,
            (unsigned long)(measured_x10 / 10), (unsigned long)(measured_x10 % 10), refclock_name(),
            corrections, g_periodic_count);
    return measured_x10;
}

void lapic_timer_set_rearm_mode(bool rearm) {
    ASSERT_ALWAYS(g_periodic_count);
    u64 flags = interrupts_save();
    g_rearm_mode = rearm;
    write(REG_TIMER_DIVIDE, DIVIDE_BY_16);
    write(REG_LVT_TIMER, rearm ? (u32)vec::APIC_TIMER : (vec::APIC_TIMER | LVT_TIMER_PERIODIC));
    write(REG_TIMER_INIT, g_periodic_count);
    interrupts_restore(flags);
}

bool lapic_timer_rearm_mode() { return g_rearm_mode; }

void lapic_timer_set_oneshot(u32 microseconds) {
    ASSERT_ALWAYS(g_ticks_per_ms);
    write(REG_TIMER_DIVIDE, DIVIDE_BY_16);
    write(REG_LVT_TIMER, vec::APIC_TIMER);
    u64 count = (u64)g_ticks_per_ms * microseconds / 1000;
    write(REG_TIMER_INIT, count ? (u32)count : 1);
}

void lapic_timer_stop() {
    write(REG_TIMER_INIT, 0);
    write(REG_LVT_TIMER, LVT_MASKED);
}

u64 lapic_timer_ticks() { return g_ticks; }

void lapic_timer_set_hook(TickHook hook) { g_hook = hook; }
