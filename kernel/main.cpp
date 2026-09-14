// Kernel entry point. Phase 1: bring up serial and the framebuffer console,
// capture the boot information, and print the boot banner.
#include <arch/x86_64/cpuid.h>
#include <boot/bootinfo.h>
#include <drivers/fbconsole.h>
#include <drivers/serial.h>
#include <lib/kprintf.h>
#include <lib/panic.h>

namespace {

void print_banner() {
    const BootInfo& bi = g_boot_info;
    char vendor[13], brand[49];
    cpuid_vendor(vendor);
    cpuid_brand(brand);

    kprintf("\n");
    kprintf("  _                              \n");
    kprintf(" | |   _   _ _ __ ___   ___ _ __ \n");
    kprintf(" | |  | | | | '_ ` _ \\ / _ \\ '_ \\\n");
    kprintf(" | |__| |_| | | | | | |  __/ | | |\n");
    kprintf(" |_____\\__,_|_| |_| |_|\\___|_| |_|\n");
    kprintf("\n");
    kprintf("Lumen %s (x86-64), built %s\n", LUMEN_VERSION, LUMEN_BUILD_DATE);
    kprintf("bootloader: %s\n", bi.bootloader);
    kprintf("cpu: %s, %s, %lu cpu(s), bsp lapic %u\n", vendor, brand,
            (unsigned long)bi.cpu_count, bi.bsp_lapic_id);
    kprintf("kernel: phys %#lx virt %#lx, hhdm %#lx, rsdp %#lx\n",
            (unsigned long)bi.kernel_phys_base, (unsigned long)bi.kernel_virt_base,
            (unsigned long)bi.hhdm_offset, (unsigned long)bi.rsdp);
    if (bi.framebuffer.present) {
        kprintf("framebuffer: %ux%u, %u bpp, pitch %u, at %p\n", bi.framebuffer.width,
                bi.framebuffer.height, bi.framebuffer.bpp, bi.framebuffer.pitch,
                bi.framebuffer.address);
    } else {
        kprintf("framebuffer: none\n");
    }
    kprintf("modules: %lu\n", (unsigned long)bi.module_count);
    for (usize i = 0; i < bi.module_count; i++)
        kprintf("  %s  %lu bytes\n", bi.modules[i].path, (unsigned long)bi.modules[i].size);

    kprintf("memory map: %lu entries\n", (unsigned long)bi.region_count);
    for (usize i = 0; i < bi.region_count; i++) {
        const MemoryRegion& r = bi.regions[i];
        kprintf("  %016lx-%016lx %10lu KiB  %s\n", (unsigned long)r.base,
                (unsigned long)(r.base + r.length - 1), (unsigned long)(r.length / KIB),
                memory_type_name(r.type));
    }
    kprintf("memory: %lu MiB usable, top of RAM at %lu MiB\n",
            (unsigned long)(bi.usable_bytes() / MIB), (unsigned long)(bi.total_bytes() / MIB));
}

} // namespace

extern "C" [[noreturn]] void kernel_main() {
    serial_init();
    kprintf("lumen: serial console up\n");

    if (!limine_base_revision_supported()) PANIC("bootloader does not support Limine base revision 3");
    boot_info_collect();
    kprintf("lumen: boot info collected (%lu memory regions)\n",
            (unsigned long)g_boot_info.region_count);

    if (!fbconsole_init(g_boot_info.framebuffer)) PANIC("framebuffer console unusable");
    kprintf("lumen: framebuffer console %ux%u cells\n", fbconsole_columns(), fbconsole_rows());

    print_banner();

    kprintf("lumen: phase 1 complete, halting\n");
    halt_forever();
}
