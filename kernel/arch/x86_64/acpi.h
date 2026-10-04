// Minimal ACPI: locate the RSDP, walk the XSDT/RSDT, and parse the MADT for
// the local APIC address, I/O APICs, and interrupt source overrides. Tables
// are mapped read-only-in-spirit via early_map and kept for later consumers
// (HPET, MCFG in phase 10).
#pragma once

#include <lib/types.h>

struct __attribute__((packed)) AcpiSdtHeader {
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
};

struct IoApicInfo {
    u8 id;
    paddr_t address;
    u32 gsi_base;
};

struct IrqOverride {
    u8 source;          // ISA IRQ number
    u32 gsi;            // global system interrupt it is wired to
    u16 flags;          // polarity bits 0-1, trigger bits 2-3 (MPS INTI flags)
};

struct MadtInfo {
    paddr_t lapic_address;
    usize cpu_count;                // enabled local APICs listed
    usize ioapic_count;
    IoApicInfo ioapics[8];
    usize override_count;
    IrqOverride overrides[24];
    bool has_legacy_pic;
};

// Parses the tables. Panics if there is no RSDP or no MADT: the kernel cannot
// route interrupts without them.
void acpi_init();
// Returns the table with the given 4-character signature, or nullptr.
const AcpiSdtHeader* acpi_find_table(const char* signature);
const MadtInfo& acpi_madt();
// The firmware vendor id from the RSDP ("BOCHS ", "VBOX  ", ...), NUL-terminated.
const char* acpi_oem_id();
