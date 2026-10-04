// PCI configuration space (mechanism 1, ports 0xCF8/0xCFC) and a list of the
// devices found at boot. The minimum the disk drivers need; phase 10 grows it
// into the full driver model (MSI, a match table, hot paths per driver).
#pragma once

#include <lib/types.h>

struct PciDevice {
    u8 bus, dev, func;
    u16 vendor, device;
    u8 class_code, subclass, prog_if, revision;
    u8 header_type;
    u32 bar[6];                 // raw BAR values
};

constexpr u32 PCI_MAX_DEVICES = 64;

// Scans every bus once. Prints nothing; `pci_print` lists the result.
void pci_init();
u32 pci_count();
const PciDevice* pci_get(u32 index);
// The first device of a class/subclass/prog-if (0xFF = any prog-if), or null.
const PciDevice* pci_find_class(u8 class_code, u8 subclass, u8 prog_if, u32 nth = 0);

u32 pci_read32(const PciDevice& d, u8 offset);
void pci_write32(const PciDevice& d, u8 offset, u32 value);
u16 pci_read16(const PciDevice& d, u8 offset);
void pci_write16(const PciDevice& d, u8 offset, u16 value);
// Turns on memory-space decoding and bus mastering (DMA).
void pci_enable_dma(const PciDevice& d);
// Physical address of a memory BAR (handles 64-bit BARs); 0 if it is an I/O BAR.
u64 pci_bar_address(const PciDevice& d, u32 index);

// One line per device (the `pci` shell command).
void pci_print();
