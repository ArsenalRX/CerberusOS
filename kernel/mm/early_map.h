// Early-boot page mapping for physical ranges the bootloader did not put in
// the direct map (ACPI tables, APIC and other MMIO). Extends the bootloader's
// page tables in place using a small static pool of page-table pages, and
// hands out virtual addresses from the kernel vmalloc region (SPEC §6.1).
// Nothing is ever unmapped; phase 4's VMM adopts these tables.
#pragma once

#include <lib/types.h>

enum class MapCache { WriteBack, Uncached };

// Maps [phys, phys + size) and returns the virtual address corresponding to
// phys. Panics if the page-table pool is exhausted. Not interrupt-safe.
void* early_map(paddr_t phys, usize size, MapCache cache);

// Physical address of a kernel-image virtual address (code or static data).
paddr_t kernel_virt_to_phys(const void* p);

// Virtual address in the HHDM for a physical address that the bootloader
// mapped (usable / reclaimable / kernel / framebuffer regions only).
void* hhdm_virt(paddr_t p);
