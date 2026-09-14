// Register access is the classic index/data pair at the MMIO base. Redirection
// entries are 64 bits split across two registers; the high half (destination)
// is written first so a half-programmed entry cannot fire at the wrong CPU.
#include <arch/x86_64/acpi.h>
#include <arch/x86_64/interrupts.h>
#include <drivers/ioapic.h>
#include <drivers/lapic.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <mm/early_map.h>

namespace {

constexpr u32 REG_ID = 0x00;
constexpr u32 REG_VERSION = 0x01;
constexpr u32 REG_REDIRECT_BASE = 0x10;

constexpr u64 ENTRY_ACTIVE_LOW = 1 << 13;
constexpr u64 ENTRY_LEVEL = 1 << 15;
constexpr u64 ENTRY_MASKED = 1 << 16;

struct IoApic {
    volatile u32* regs;
    u32 gsi_base;
    u32 gsi_count;
    u8 id;
};

IoApic g_ioapics[8];
usize g_count = 0;

u32 read(IoApic& io, u32 reg) {
    io.regs[0] = reg;
    return io.regs[4];
}

void write(IoApic& io, u32 reg, u32 v) {
    io.regs[0] = reg;
    io.regs[4] = v;
}

IoApic* find(u32 gsi) {
    for (usize i = 0; i < g_count; i++)
        if (gsi >= g_ioapics[i].gsi_base && gsi < g_ioapics[i].gsi_base + g_ioapics[i].gsi_count)
            return &g_ioapics[i];
    return nullptr;
}

void set_mask(u8 irq, bool masked) {
    u16 flags;
    u32 gsi = ioapic_irq_to_gsi(irq, &flags);
    IoApic* io = find(gsi);
    if (!io) PANIC("ioapic: no controller for gsi %u", gsi);
    u32 reg = REG_REDIRECT_BASE + 2 * (gsi - io->gsi_base);
    u32 lo = read(*io, reg);
    if (masked) lo |= ENTRY_MASKED;
    else lo &= ~ENTRY_MASKED;
    write(*io, reg, lo);
}

} // namespace

void ioapic_init() {
    const MadtInfo& madt = acpi_madt();
    if (madt.ioapic_count == 0) PANIC("ioapic: MADT lists no I/O APIC");
    g_count = 0;
    for (usize i = 0; i < madt.ioapic_count && g_count < 8; i++) {
        IoApic& io = g_ioapics[g_count++];
        io.regs = (volatile u32*)early_map(madt.ioapics[i].address, PAGE_SIZE, MapCache::Uncached);
        io.gsi_base = madt.ioapics[i].gsi_base;
        io.id = madt.ioapics[i].id;
        u32 ver = read(io, REG_VERSION);
        io.gsi_count = ((ver >> 16) & 0xFF) + 1;
        kprintf("ioapic: id %u at %#lx, gsi %u-%u, version %#x\n", io.id,
                (unsigned long)madt.ioapics[i].address, io.gsi_base, io.gsi_base + io.gsi_count - 1,
                ver & 0xFF);
        for (u32 n = 0; n < io.gsi_count; n++) {
            write(io, REG_REDIRECT_BASE + 2 * n + 1, 0);
            write(io, REG_REDIRECT_BASE + 2 * n, ENTRY_MASKED);
        }
    }

    u32 bsp = lapic_id();
    for (u8 irq = 0; irq < 16; irq++) {
        u16 flags;
        u32 gsi = ioapic_irq_to_gsi(irq, &flags);
        bool low = (flags & 3) == 3;
        bool level = ((flags >> 2) & 3) == 3;
        if (find(gsi)) ioapic_route_gsi(gsi, vec::IRQ_BASE + irq, bsp, low, level, true);
    }
}

u32 ioapic_irq_to_gsi(u8 irq, u16* flags) {
    const MadtInfo& madt = acpi_madt();
    for (usize i = 0; i < madt.override_count; i++) {
        if (madt.overrides[i].source == irq) {
            if (flags) *flags = madt.overrides[i].flags;
            return madt.overrides[i].gsi;
        }
    }
    if (flags) *flags = 0;      // conforming: ISA is edge-triggered, active high
    return irq;
}

void ioapic_route_gsi(u32 gsi, u8 vector, u32 dest, bool active_low, bool level, bool masked) {
    IoApic* io = find(gsi);
    if (!io) PANIC("ioapic: no controller for gsi %u", gsi);
    u64 entry = vector;
    if (active_low) entry |= ENTRY_ACTIVE_LOW;
    if (level) entry |= ENTRY_LEVEL;
    if (masked) entry |= ENTRY_MASKED;
    entry |= (u64)(dest & 0xFF) << 56;
    u32 reg = REG_REDIRECT_BASE + 2 * (gsi - io->gsi_base);
    write(*io, reg + 1, (u32)(entry >> 32));
    write(*io, reg, (u32)entry);
}

void ioapic_unmask_irq(u8 irq) { set_mask(irq, false); }
void ioapic_mask_irq(u8 irq) { set_mask(irq, true); }
