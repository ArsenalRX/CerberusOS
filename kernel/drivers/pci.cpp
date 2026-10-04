// See pci.h.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/io.h>
#include <boot/bootinfo.h>
#include <drivers/pci.h>
#include <lib/kprintf.h>

namespace {

constexpr u16 CONFIG_ADDRESS = 0xCF8, CONFIG_DATA = 0xCFC;

PciDevice g_devices[PCI_MAX_DEVICES];
u32 g_count = 0;

u32 address(u8 bus, u8 dev, u8 func, u8 offset) {
    return 0x80000000u | (u32)bus << 16 | (u32)dev << 11 | (u32)func << 8 | (offset & 0xFC);
}

// Config space accesses are two port operations that must not interleave.
u32 raw_read(u8 bus, u8 dev, u8 func, u8 offset) {
    u64 irq = interrupts_save();
    outl(CONFIG_ADDRESS, address(bus, dev, func, offset));
    u32 v = inl(CONFIG_DATA);
    interrupts_restore(irq);
    return v;
}

void raw_write(u8 bus, u8 dev, u8 func, u8 offset, u32 value) {
    u64 irq = interrupts_save();
    outl(CONFIG_ADDRESS, address(bus, dev, func, offset));
    outl(CONFIG_DATA, value);
    interrupts_restore(irq);
}

void add(u8 bus, u8 dev, u8 func) {
    if (g_count == PCI_MAX_DEVICES) return;
    u32 id = raw_read(bus, dev, func, 0x00);
    PciDevice& d = g_devices[g_count++];
    d.bus = bus;
    d.dev = dev;
    d.func = func;
    d.vendor = (u16)id;
    d.device = (u16)(id >> 16);
    u32 cls = raw_read(bus, dev, func, 0x08);
    d.revision = (u8)cls;
    d.prog_if = (u8)(cls >> 8);
    d.subclass = (u8)(cls >> 16);
    d.class_code = (u8)(cls >> 24);
    d.header_type = (u8)(raw_read(bus, dev, func, 0x0C) >> 16);
    if ((d.header_type & 0x7F) == 0)
        for (int i = 0; i < 6; i++) d.bar[i] = raw_read(bus, dev, func, (u8)(0x10 + 4 * i));
}

const char* class_name(u8 c, u8 s) {
    switch (c) {
    case 0x01: return s == 0x01 ? "IDE controller" : s == 0x06 ? "SATA controller (AHCI)" : s == 0x08 ? "NVMe controller" : "storage";
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia";
    case 0x06: return s == 0x00 ? "host bridge" : s == 0x01 ? "ISA bridge" : s == 0x04 ? "PCI bridge" : "bridge";
    case 0x0C: return s == 0x03 ? "USB controller" : s == 0x05 ? "SMBus" : "serial bus";
    }
    return "device";
}

} // namespace

void pci_init() {
    g_count = 0;
    for (u32 bus = 0; bus < 256; bus++)
        for (u8 dev = 0; dev < 32; dev++) {
            u32 id = raw_read((u8)bus, dev, 0, 0x00);
            if ((u16)id == 0xFFFF) continue;
            u8 header = (u8)(raw_read((u8)bus, dev, 0, 0x0C) >> 16);
            add((u8)bus, dev, 0);
            if (header & 0x80)
                for (u8 func = 1; func < 8; func++)
                    if ((u16)raw_read((u8)bus, dev, func, 0x00) != 0xFFFF) add((u8)bus, dev, func);
        }
}

u32 pci_count() { return g_count; }
const PciDevice* pci_get(u32 index) { return index < g_count ? &g_devices[index] : nullptr; }

const PciDevice* pci_find_class(u8 class_code, u8 subclass, u8 prog_if, u32 nth) {
    for (u32 i = 0; i < g_count; i++) {
        const PciDevice& d = g_devices[i];
        if (d.class_code == class_code && d.subclass == subclass && (prog_if == 0xFF || d.prog_if == prog_if)) {
            if (nth == 0) return &d;
            nth--;
        }
    }
    return nullptr;
}

u32 pci_read32(const PciDevice& d, u8 offset) { return raw_read(d.bus, d.dev, d.func, offset); }
void pci_write32(const PciDevice& d, u8 offset, u32 value) { raw_write(d.bus, d.dev, d.func, offset, value); }

u16 pci_read16(const PciDevice& d, u8 offset) {
    return (u16)(raw_read(d.bus, d.dev, d.func, offset & 0xFC) >> ((offset & 2) * 8));
}

void pci_write16(const PciDevice& d, u8 offset, u16 value) {
    u32 v = raw_read(d.bus, d.dev, d.func, offset & 0xFC);
    u32 shift = (offset & 2) * 8;
    v = (v & ~(0xFFFFu << shift)) | (u32)value << shift;
    raw_write(d.bus, d.dev, d.func, offset & 0xFC, v);
}

void pci_enable_dma(const PciDevice& d) {
    u16 cmd = pci_read16(d, 0x04);
    pci_write16(d, 0x04, cmd | (1 << 1) | (1 << 2));       // memory space, bus master
}

u64 pci_bar_address(const PciDevice& d, u32 index) {
    if (index >= 6) return 0;
    u32 b = d.bar[index];
    if (b & 1) return 0;                                    // I/O space
    u64 addr = b & ~0xFull;
    if (((b >> 1) & 3) == 2 && index < 5) addr |= (u64)d.bar[index + 1] << 32;    // 64-bit BAR
    return addr;
}

u8 pci_read8(const PciDevice& d, u8 offset) {
    return (u8)(raw_read(d.bus, d.dev, d.func, offset & 0xFC) >> ((offset & 3) * 8));
}

u8 pci_find_capability(const PciDevice& d, u8 id, u8 after) {
    if (!(pci_read16(d, 0x06) & (1 << 4))) return 0;        // status: no capability list
    u8 ptr = after ? pci_read8(d, (u8)(after + 1)) : pci_read8(d, 0x34);
    for (int guard = 0; guard < 48 && ptr >= 0x40; guard++) {
        ptr &= 0xFC;
        if (pci_read8(d, ptr) == id) return ptr;
        ptr = pci_read8(d, (u8)(ptr + 1));
    }
    return 0;
}

bool pci_enable_msi(const PciDevice& d, u8 vector) {
    u8 cap = pci_find_capability(d, 0x05);
    if (!cap) return false;
    u16 control = pci_read16(d, (u8)(cap + 2));
    bool is64 = control & (1 << 7);
    // Message address: the local APIC of the bootstrap CPU; data: the vector
    // (fixed delivery, edge triggered).
    pci_write32(d, (u8)(cap + 4), 0xFEE00000u | (g_boot_info.bsp_lapic_id << 12));
    if (is64) {
        pci_write32(d, (u8)(cap + 8), 0);
        pci_write16(d, (u8)(cap + 12), vector);
    } else {
        pci_write16(d, (u8)(cap + 8), vector);
    }
    control = (u16)((control & ~(7u << 4)) | 1);            // one message, enabled
    pci_write16(d, (u8)(cap + 2), control);
    pci_write16(d, 0x04, (u16)(pci_read16(d, 0x04) | (1 << 10)));   // INTx off
    return true;
}

void pci_print() {
    for (u32 i = 0; i < g_count; i++) {
        const PciDevice& d = g_devices[i];
        kprintf("  %02x:%02x.%x  %04x:%04x  class %02x.%02x.%02x  %s\n", d.bus, d.dev, d.func, d.vendor, d.device,
                d.class_code, d.subclass, d.prog_if, class_name(d.class_code, d.subclass));
    }
}
