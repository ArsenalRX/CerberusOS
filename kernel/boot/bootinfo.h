// BootInfo: everything the kernel needs from the bootloader, copied into kernel
// memory so Limine's bootloader-reclaimable structures can later be freed.
#pragma once

#include <lib/types.h>

enum class MemoryType : u8 {
    Usable,
    Reserved,
    AcpiReclaimable,
    AcpiNvs,
    Bad,
    BootloaderReclaimable,
    KernelAndModules,
    Framebuffer,
    ReservedMapped,
    Unknown,
};

const char* memory_type_name(MemoryType t);

struct MemoryRegion {
    paddr_t base;
    u64 length;
    MemoryType type;
};

struct FramebufferInfo {
    bool present;
    void* address;          // virtual address as mapped by the bootloader (HHDM)
    u32 width, height, pitch;
    u16 bpp;
    u8 red_size, red_shift, green_size, green_shift, blue_size, blue_shift;
};

struct BootModule {
    vaddr_t address;        // virtual (HHDM) address of the file contents
    u64 size;
    char path[64];
};

struct CpuInfo {
    u32 processor_id;
    u32 lapic_id;
};

struct BootInfo {
    static constexpr usize MAX_REGIONS = 128;
    static constexpr usize MAX_MODULES = 8;
    static constexpr usize MAX_CPUS = 64;

    char bootloader[48];
    u64 hhdm_offset;
    paddr_t kernel_phys_base;
    vaddr_t kernel_virt_base;
    paddr_t rsdp;               // physical address, 0 if none

    usize region_count;
    MemoryRegion regions[MAX_REGIONS];

    FramebufferInfo framebuffer;

    usize module_count;
    BootModule modules[MAX_MODULES];

    u32 bsp_lapic_id;
    usize cpu_count;
    CpuInfo cpus[MAX_CPUS];

    u64 usable_bytes() const;
    u64 total_bytes() const;    // highest end address of any RAM-backed region
};

// Filled in by boot_info_collect(); read-only afterwards.
extern BootInfo g_boot_info;

// True if the bootloader understood the base revision we asked for. If false,
// no response pointer may be trusted.
bool limine_base_revision_supported();

// Reads every Limine response into g_boot_info. Must be called before
// bootloader-reclaimable memory is touched. Panics if a required response
// (memory map, HHDM, kernel address) is missing.
void boot_info_collect();
