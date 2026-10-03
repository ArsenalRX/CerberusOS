// Kernel entry point. Phase 2: after the console is up, install the GDT/TSS
// and IDT, bring up the interrupt controllers and the APIC timer, then drop
// into the kernel shell.
#include <arch/x86_64/acpi.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpuid.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/interrupts.h>
#include <boot/bootinfo.h>
#include <drivers/fbconsole.h>
#include <drivers/hpet.h>
#include <drivers/ioapic.h>
#include <drivers/lapic.h>
#include <drivers/pic.h>
#include <drivers/ps2kbd.h>
#include <drivers/ps2mouse.h>
#include <drivers/refclock.h>
#include <drivers/rtc.h>
#include <gui/desktop.h>
#include <drivers/serial.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/shell.h>
#include <lib/symbols.h>
#include <lib/version.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

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
    kprintf("Lumen %s (x86-64), built %s\n", lumen_version(), lumen_build_date());
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

    symbols_init();
    kprintf("symbols: %lu kernel symbols loaded\n", (unsigned long)symbols_count());
    pmm_init();

    gdt_init_bsp();
    kprintf("gdt: loaded (kernel cs=%#x ds=%#x, user cs=%#x ds=%#x, tss=%#x)\n", seg::KCODE, seg::KDATA,
            seg::UCODE, seg::UDATA, seg::TSS);
    interrupts_init();
    kprintf("idt: 256 gates loaded, IST for #DF/#NMI/#MC\n");
    vmm_init();
    kheap_init();

    pic_init();
    kprintf("pic: remapped to 0x20-0x2f and masked\n");
    acpi_init();
    hpet_init();
    refclock_init();
    lapic_init();
    ioapic_init();
    lapic_timer_calibrate();
    lapic_timer_set_periodic(TIMER_HZ);
    ps2kbd_init();
    interrupts_enable();
    kprintf("timer: periodic at %u Hz, interrupts enabled\n", TIMER_HZ);
    kprintf("ps2kbd: irq 1 unmasked (early driver, US layout)\n");

    // Measure the delivered tick rate and correct the reload count if the
    // estimate was off. A wrong rate here would make every timeout wrong.
    u64 measured_hz_x10 = lapic_timer_tune(TIMER_HZ);

    rtc_init();
    bool mouse = ps2mouse_init();

    // One-line summary for the serial log so a VM run can be judged at a glance.
    kprintf("boot: OK  serial fbconsole(%ux%u) symbols(%lu) pmm gdt idt acpi %s lapic ioapic "
            "timer(%uHz, measured %lu.%luHz) keyboard %s rtc\n",
            fbconsole_columns(), fbconsole_rows(), (unsigned long)symbols_count(),
            hpet_available() ? "hpet" : "no-hpet", TIMER_HZ, (unsigned long)(measured_hz_x10 / 10),
            (unsigned long)(measured_hz_x10 % 10), mouse ? "mouse" : "no-mouse");

    gui_init();
    shell_run();
}
