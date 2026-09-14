// Register block layout per the IA-PC HPET specification 1.0a. Only the
// general capability/configuration registers and the main counter are used.
#include <arch/x86_64/acpi.h>
#include <drivers/hpet.h>
#include <lib/kprintf.h>
#include <mm/early_map.h>

namespace {

constexpr u32 REG_CAPABILITIES = 0x000;   // bits 32-63: counter period in femtoseconds
constexpr u32 REG_CONFIG = 0x010;         // bit 0: enable main counter
constexpr u32 REG_MAIN_COUNTER = 0x0F0;
constexpr u64 CONFIG_ENABLE = 1 << 0;
constexpr u64 FS_PER_SECOND = 1000000000000000ull;
constexpr u64 MAX_PERIOD_FS = 100000000;   // spec limit: 100 ns

volatile u8* g_regs = nullptr;
u64 g_period_fs = 0;
u64 g_frequency_hz = 0;

u64 read64(u32 reg) { return *(volatile u64*)(g_regs + reg); }
void write64(u32 reg, u64 v) { *(volatile u64*)(g_regs + reg) = v; }

} // namespace

bool hpet_init() {
    const AcpiSdtHeader* table = acpi_find_table("HPET");
    if (!table) {
        kprintf("hpet: no ACPI HPET table\n");
        return false;
    }
    // Generic Address Structure at offset 40; the address itself at 44.
    const u8* p = (const u8*)table;
    u8 space = p[40];
    u64 base;
    __builtin_memcpy(&base, p + 44, 8);
    if (space != 0 || base == 0) {
        kprintf("hpet: unsupported address space %u at %#lx\n", space, (unsigned long)base);
        return false;
    }
    g_regs = (volatile u8*)early_map(base, 0x400, MapCache::Uncached);
    u64 caps = read64(REG_CAPABILITIES);
    g_period_fs = caps >> 32;
    if (g_period_fs == 0 || g_period_fs > MAX_PERIOD_FS) {
        kprintf("hpet: implausible period %lu fs, ignoring\n", (unsigned long)g_period_fs);
        g_regs = nullptr;
        return false;
    }
    g_frequency_hz = FS_PER_SECOND / g_period_fs;
    write64(REG_CONFIG, read64(REG_CONFIG) | CONFIG_ENABLE);
    u64 a = read64(REG_MAIN_COUNTER);
    u64 b = read64(REG_MAIN_COUNTER);
    if (b == a) {
        // Give it a moment; some emulators advance lazily.
        for (int i = 0; i < 100000 && (b = read64(REG_MAIN_COUNTER)) == a; i++) asm volatile("pause");
    }
    kprintf("hpet: at %#lx, %lu.%06lu MHz, %u comparators, 64-bit counter %s\n", (unsigned long)base,
            (unsigned long)(g_frequency_hz / 1000000), (unsigned long)(g_frequency_hz % 1000000),
            (unsigned)((caps >> 8) & 0x1F) + 1, b != a ? "running" : "NOT advancing");
    if (b == a) {
        g_regs = nullptr;
        return false;
    }
    return true;
}

bool hpet_available() { return g_regs != nullptr; }
u64 hpet_frequency_hz() { return g_frequency_hz; }
u64 hpet_counter() { return read64(REG_MAIN_COUNTER); }

u64 hpet_delta_us(u64 start, u64 end) { return (end - start) * 1000000 / g_frequency_hz; }

void hpet_sleep_us(u64 microseconds) {
    u64 ticks = microseconds * g_frequency_hz / 1000000;
    u64 start = read64(REG_MAIN_COUNTER);
    while (read64(REG_MAIN_COUNTER) - start < ticks) asm volatile("pause");
}
