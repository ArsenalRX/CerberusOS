// See power.h. FADT layout from the ACPI 6.4 specification, section 5.2.9.
#include <arch/x86_64/acpi.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/io.h>
#include <arch/x86_64/power.h>
#include <drivers/refclock.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <mm/early_map.h>

namespace {

constexpr u16 SLP_EN = 1 << 13, SCI_EN = 1 << 0;
constexpr u32 FADT_RESET_REG_SUP = 1 << 10;

struct {
    bool s5_found;
    u16 pm1a_cnt, pm1b_cnt;
    u8 slp_typa, slp_typb;
    u32 smi_cmd;
    u8 acpi_enable;
    bool reset_supported;
    u8 reset_space;             // 0 memory, 1 I/O port
    u64 reset_address;
    u8 reset_value;
} g_power;

template <typename T> T field(const u8* table, u32 offset) {
    T v;
    memcpy(&v, table + offset, sizeof v);
    return v;
}

// One element of the \_S5_ package: a byte constant, with or without the
// BytePrefix (0x0A), or the ZeroOp/OneOp constants. False if it is
// something else or runs past the end.
bool package_byte(const u8*& p, const u8* end, u8* out) {
    if (p >= end) return false;
    if (*p == 0x0A) {
        if (p + 1 >= end) return false;
        *out = p[1];
        p += 2;
        return true;
    }
    if (*p == 0x00 || *p == 0x01) {
        *out = *p++;
        return true;
    }
    return false;
}

// Finds "Name(\_S5_, Package(){typa, typb, ...})" in the AML and decodes
// the first two elements.
bool find_s5(const u8* aml, usize length) {
    for (usize i = 1; i + 8 < length; i++) {
        if (memcmp(aml + i, "_S5_", 4) != 0) continue;
        // A NameOp (0x08) directly before the name, or before a root prefix.
        bool named = aml[i - 1] == 0x08 || (i >= 2 && aml[i - 1] == '\\' && aml[i - 2] == 0x08);
        if (!named) continue;
        const u8* p = aml + i + 4;
        const u8* end = aml + length;
        if (p >= end || *p != 0x12) continue;           // PackageOp
        p++;
        if (p >= end) return false;
        usize extra = *p >> 6;                          // PkgLength: 0-3 following bytes
        p += 1 + extra;
        if (p >= end) return false;
        u8 elements = *p++;
        if (elements < 2) continue;
        u8 a, b;
        if (!package_byte(p, end, &a) || !package_byte(p, end, &b)) continue;
        g_power.slp_typa = a & 7;
        g_power.slp_typb = b & 7;
        return true;
    }
    return false;
}

} // namespace

void power_init() {
    const AcpiSdtHeader* fadt = acpi_find_table("FACP");
    if (!fadt || fadt->length < 116) {
        kprintf("power: no usable FADT; power off is not available\n");
        return;
    }
    const u8* f = (const u8*)fadt;
    g_power.smi_cmd = field<u32>(f, 48);
    g_power.acpi_enable = field<u8>(f, 52);
    u32 pm1a = field<u32>(f, 64), pm1b = field<u32>(f, 68);
    // PM1 control blocks are I/O ports on every PC; anything else is refused.
    g_power.pm1a_cnt = pm1a && pm1a <= 0xFFFF ? (u16)pm1a : 0;
    g_power.pm1b_cnt = pm1b && pm1b <= 0xFFFF ? (u16)pm1b : 0;
    if (fadt->length >= 129 && (field<u32>(f, 112) & FADT_RESET_REG_SUP)) {
        g_power.reset_space = field<u8>(f, 116);
        g_power.reset_address = field<u64>(f, 120);
        g_power.reset_value = field<u8>(f, 128);
        g_power.reset_supported = g_power.reset_space <= 1 && g_power.reset_address != 0;
    }
    paddr_t dsdt_phys = fadt->length >= 148 ? field<u64>(f, 140) : 0;
    if (!dsdt_phys) dsdt_phys = field<u32>(f, 40);
    if (dsdt_phys) {
        const AcpiSdtHeader* h = (const AcpiSdtHeader*)early_map(dsdt_phys, sizeof(AcpiSdtHeader), MapCache::WriteBack);
        u32 length = h->length;
        if (memcmp(h->signature, "DSDT", 4) == 0 && length > sizeof(AcpiSdtHeader) && length <= 16 * MIB) {
            const u8* dsdt = (const u8*)early_map(dsdt_phys, length, MapCache::WriteBack);
            g_power.s5_found = find_s5(dsdt + sizeof(AcpiSdtHeader), length - sizeof(AcpiSdtHeader));
        }
    }
    kprintf("power: ACPI power off %s (PM1a %#x, S5 type %u), reset register %s\n",
            power_can_power_off() ? "available" : "NOT available", g_power.pm1a_cnt, g_power.slp_typa,
            g_power.reset_supported ? "available" : "absent (keyboard controller will be used)");
}

bool power_can_power_off() { return g_power.s5_found && g_power.pm1a_cnt; }

void power_off() {
    if (!power_can_power_off()) return;
    interrupts_disable();
    // The firmware may still own the power registers: ask for ACPI mode.
    if (!(inw(g_power.pm1a_cnt) & SCI_EN) && g_power.smi_cmd && g_power.smi_cmd <= 0xFFFF && g_power.acpi_enable) {
        outb((u16)g_power.smi_cmd, g_power.acpi_enable);
        u64 start = refclock_now_us();
        while (!(inw(g_power.pm1a_cnt) & SCI_EN) && refclock_now_us() - start < 3000000) asm volatile("pause");
    }
    outw(g_power.pm1a_cnt, (u16)(g_power.slp_typa << 10 | SLP_EN));
    if (g_power.pm1b_cnt) outw(g_power.pm1b_cnt, (u16)(g_power.slp_typb << 10 | SLP_EN));
    // The machine is gone within microseconds if this worked.
    refclock_sleep_ms(1000);
}

[[noreturn]] void power_reboot() {
    interrupts_disable();
    if (g_power.reset_supported) {
        if (g_power.reset_space == 1 && g_power.reset_address <= 0xFFFF) {
            outb((u16)g_power.reset_address, g_power.reset_value);
        } else if (g_power.reset_space == 0) {
            volatile u8* reg = (volatile u8*)early_map(g_power.reset_address, 1, MapCache::Uncached);
            *reg = g_power.reset_value;
        }
        refclock_sleep_ms(100);
    }
    // Pulse the keyboard controller's reset line.
    outb(0x64, 0xFE);
    refclock_sleep_ms(100);
    // Last resort: an exception with no way to handle it resets the CPU.
    struct __attribute__((packed)) {
        u16 limit;
        u64 base;
    } null_idt{0, 0};
    asm volatile("lidt %0; int3" ::"m"(null_idt));
    halt_forever();
}
