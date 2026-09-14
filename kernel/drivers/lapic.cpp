// xAPIC (MMIO) mode only. The register page is mapped uncached through
// early_map. x2APIC mode is detected and refused for now so a misconfigured
// firmware fails loudly rather than reading garbage.
#include <arch/x86_64/acpi.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/interrupts.h>
#include <drivers/lapic.h>
#include <drivers/pit.h>
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

volatile u32* g_regs = nullptr;
u32 g_ticks_per_ms = 0;
volatile u64 g_ticks = 0;
TickHook g_hook = nullptr;

u32 read(u32 reg) { return g_regs[reg / 4]; }
void write(u32 reg, u32 v) { g_regs[reg / 4] = v; }

void timer_handler(InterruptFrame*, void*) {
    g_ticks = g_ticks + 1;
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

} // namespace

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

    write(REG_TPR, 0);                      // accept all priorities
    write(REG_LVT_TIMER, LVT_MASKED);
    write(REG_LVT_THERMAL, LVT_MASKED);
    write(REG_LVT_PERF, LVT_MASKED);
    write(REG_LVT_LINT0, LVT_MASKED);
    write(REG_LVT_LINT1, LVT_MASKED);
    write(REG_LVT_ERROR, vec::APIC_ERROR);
    write(REG_ESR, 0);
    write(REG_SVR, SVR_ENABLE | vec::APIC_SPURIOUS);

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
    u32 best = 0;
    // Three 10 ms samples; keep the largest (the least disturbed by emulation hiccups).
    for (int i = 0; i < 3; i++) {
        write(REG_TIMER_INIT, 0xFFFFFFFF);
        pit_sleep_ms(10);
        u32 elapsed = 0xFFFFFFFF - read(REG_TIMER_CURRENT);
        write(REG_TIMER_INIT, 0);
        if (elapsed > best) best = elapsed;
    }
    g_ticks_per_ms = best / 10;
    if (g_ticks_per_ms == 0) PANIC("lapic: timer calibration produced zero ticks");
    kprintf("lapic: timer %u ticks/ms (divide 16), ~%u.%03u MHz\n", g_ticks_per_ms,
            g_ticks_per_ms / 1000, g_ticks_per_ms % 1000);
}

u32 lapic_timer_ticks_per_ms() { return g_ticks_per_ms; }

void lapic_timer_set_periodic(u32 hz) {
    ASSERT_ALWAYS(g_ticks_per_ms && hz);
    write(REG_TIMER_DIVIDE, DIVIDE_BY_16);
    write(REG_LVT_TIMER, vec::APIC_TIMER | LVT_TIMER_PERIODIC);
    write(REG_TIMER_INIT, (u32)((u64)g_ticks_per_ms * 1000 / hz));
}

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
