// Table discovery. Every table is checksummed; a bad checksum is reported but
// the table is still used, since virtual firmware is occasionally sloppy.
#include <arch/x86_64/acpi.h>
#include <boot/bootinfo.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <mm/early_map.h>

namespace {

struct __attribute__((packed)) Rsdp {
    char signature[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_address;
    // ACPI 2.0+
    u32 length;
    u64 xsdt_address;
    u8 extended_checksum;
    u8 reserved[3];
};

constexpr usize MAX_TABLES = 32;
const AcpiSdtHeader* g_tables[MAX_TABLES];
usize g_table_count = 0;
MadtInfo g_madt;

bool checksum_ok(const void* p, usize len) {
    u8 sum = 0;
    for (usize i = 0; i < len; i++) sum += ((const u8*)p)[i];
    return sum == 0;
}

// Maps a table whose length is unknown until its header is readable.
const AcpiSdtHeader* map_table(paddr_t phys) {
    const AcpiSdtHeader* h = (const AcpiSdtHeader*)early_map(phys, sizeof(AcpiSdtHeader), MapCache::WriteBack);
    if (h->length > sizeof(AcpiSdtHeader)) h = (const AcpiSdtHeader*)early_map(phys, h->length, MapCache::WriteBack);
    return h;
}

void add_table(paddr_t phys) {
    if (g_table_count >= MAX_TABLES) return;
    const AcpiSdtHeader* h = map_table(phys);
    if (!checksum_ok(h, h->length))
        kprintf("acpi: warning: table %.4s at %#lx has a bad checksum\n", h->signature, (unsigned long)phys);
    g_tables[g_table_count++] = h;
}

void parse_madt(const AcpiSdtHeader* madt) {
    memset(&g_madt, 0, sizeof g_madt);
    const u8* p = (const u8*)madt;
    g_madt.lapic_address = *(const u32*)(p + 36);
    u32 flags = *(const u32*)(p + 40);
    g_madt.has_legacy_pic = flags & 1;

    const u8* end = p + madt->length;
    for (const u8* e = p + 44; e + 2 <= end && e + e[1] <= end; e += e[1]) {
        u8 type = e[0];
        switch (type) {
        case 0: {   // processor local APIC
            u32 lflags = *(const u32*)(e + 4);
            if (lflags & 3) g_madt.cpu_count++;
            break;
        }
        case 1: {   // I/O APIC
            if (g_madt.ioapic_count < 8) {
                IoApicInfo& io = g_madt.ioapics[g_madt.ioapic_count++];
                io.id = e[2];
                io.address = *(const u32*)(e + 4);
                io.gsi_base = *(const u32*)(e + 8);
            }
            break;
        }
        case 2: {   // interrupt source override
            if (g_madt.override_count < 24) {
                IrqOverride& o = g_madt.overrides[g_madt.override_count++];
                o.source = e[3];
                o.gsi = *(const u32*)(e + 4);
                o.flags = *(const u16*)(e + 8);
            }
            break;
        }
        case 5:     // local APIC address override
            g_madt.lapic_address = *(const u64*)(e + 4);
            break;
        case 9: {   // processor local x2APIC
            u32 lflags = *(const u32*)(e + 8);
            if (lflags & 3) g_madt.cpu_count++;
            break;
        }
        default: break;
        }
    }
}

char g_oem_id[7];

} // namespace

const char* acpi_oem_id() { return g_oem_id; }

void acpi_init() {
    if (!g_boot_info.rsdp) PANIC("acpi: bootloader provided no RSDP");
    const Rsdp* rsdp = (const Rsdp*)early_map(g_boot_info.rsdp, sizeof(Rsdp), MapCache::WriteBack);
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) PANIC("acpi: bad RSDP signature");
    if (!checksum_ok(rsdp, 20)) kprintf("acpi: warning: RSDP v1 checksum bad\n");
    memcpy(g_oem_id, rsdp->oem_id, 6);

    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        if (!checksum_ok(rsdp, 36)) kprintf("acpi: warning: RSDP v2 checksum bad\n");
        const AcpiSdtHeader* xsdt = map_table(rsdp->xsdt_address);
        usize n = (xsdt->length - sizeof(AcpiSdtHeader)) / 8;
        const u64* entries = (const u64*)((const u8*)xsdt + sizeof(AcpiSdtHeader));
        for (usize i = 0; i < n; i++) {
            u64 addr;
            memcpy(&addr, &entries[i], 8);      // XSDT entries are not 8-aligned
            if (addr) add_table(addr);
        }
        kprintf("acpi: RSDP rev %u, XSDT with %lu tables\n", rsdp->revision, (unsigned long)n);
    } else {
        const AcpiSdtHeader* rsdt = map_table(rsdp->rsdt_address);
        usize n = (rsdt->length - sizeof(AcpiSdtHeader)) / 4;
        const u32* entries = (const u32*)((const u8*)rsdt + sizeof(AcpiSdtHeader));
        for (usize i = 0; i < n; i++)
            if (entries[i]) add_table(entries[i]);
        kprintf("acpi: RSDP rev %u, RSDT with %lu tables\n", rsdp->revision, (unsigned long)n);
    }

    kprintf("acpi: tables:");
    for (usize i = 0; i < g_table_count; i++) kprintf(" %.4s", g_tables[i]->signature);
    kprintf("\n");

    const AcpiSdtHeader* madt = acpi_find_table("APIC");
    if (!madt) PANIC("acpi: no MADT; cannot configure interrupt controllers");
    parse_madt(madt);
    kprintf("acpi: MADT: lapic at %#lx, %lu cpu(s), %lu ioapic(s), %lu override(s)%s\n",
            (unsigned long)g_madt.lapic_address, (unsigned long)g_madt.cpu_count,
            (unsigned long)g_madt.ioapic_count, (unsigned long)g_madt.override_count,
            g_madt.has_legacy_pic ? ", legacy PIC present" : "");
    for (usize i = 0; i < g_madt.override_count; i++) {
        const IrqOverride& o = g_madt.overrides[i];
        kprintf("acpi:   irq %u -> gsi %u flags %#x\n", o.source, o.gsi, o.flags);
    }
}

const AcpiSdtHeader* acpi_find_table(const char* signature) {
    for (usize i = 0; i < g_table_count; i++)
        if (memcmp(g_tables[i]->signature, signature, 4) == 0) return g_tables[i];
    return nullptr;
}

const MadtInfo& acpi_madt() { return g_madt; }
