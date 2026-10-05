// Kernel entry point: console, CPU tables, memory management, interrupt
// controllers and the timer, then the scheduler. The desktop and the kernel
// shell run as threads.
#include <arch/x86_64/acpi.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpufeatures.h>
#include <arch/x86_64/cpuid.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <lib/csprng.h>
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
#include <arch/x86_64/power.h>
#include <drivers/ahci.h>
#include <drivers/bga.h>
#include <drivers/fbdev.h>
#include <drivers/input.h>
#include <drivers/driver.h>
#include <drivers/nvme.h>
#include <drivers/virtio_blk.h>
#include <drivers/ata.h>
#include <drivers/pci.h>
#include <drivers/vmmdev.h>
#include <fs/fs.h>
#include <lib/string.h>
#include <mm/vmm.h>
#include <proc/process.h>
#include <sched/sched.h>

namespace {

void print_banner() {
    const BootInfo& bi = g_boot_info;
    char vendor[13], brand[49];
    cpuid_vendor(vendor);
    cpuid_brand(brand);

    kprintf("\n");
    kprintf("   ____          _                          \n");
    kprintf("  / ___|___ _ __| |__   ___ _ __ _   _ ___ \n");
    kprintf(" | |   / _ \\ '__| '_ \\ / _ \\ '__| | | / __|\n");
    kprintf(" | |__|  __/ |  | |_) |  __/ |  | |_| \\__ \\\n");
    kprintf("  \\____\\___|_|  |_.__/ \\___|_|   \\__,_|___/\n");
    kprintf("\n");
    kprintf("Cerberus %s (x86-64), built %s\n", cerberus_version(), cerberus_build_date());
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

// The first thread: brings up the desktop, then becomes the kernel shell.
void init_thread(void*) {
    strlcpy(thread_current()->name, "shell", sizeof thread_current()->name);
    power_init();
    input_init();
    fbdev_init();
    pci_init();
    vmmdev_init();
    ahci_register();
    nvme_register();
    virtio_blk_register();
    ata_register();
    drivers_probe_all();
    bga_init();
    fs_init();
    if (gui_init() && !gui_start_compositor()) kprintf("gui: could not start the compositor thread\n");
    // The first user process. It is not waited for: it runs for as long as
    // the system does.
    {
        const char* const argv[] = {"init", nullptr};
        Result<Process*> init = process_spawn("/bin/init", argv, true);
        // Announced here rather than by the program: it starts on another
        // CPU, and its own first line would land somewhere in the middle of
        // the shell's.
        if (init.ok()) kprintf("init: started as pid %u\n", init.value()->pid);
        else kprintf("init: could not start /bin/init: %s\n", error_name(init.error()));
    }
    shell_run();
}

} // namespace

extern "C" u64 __stack_chk_guard;

extern "C" [[noreturn]] void kernel_main() {
    // Randomness first: the stack-protector guard must be set before any
    // function that will return has been entered under the old value. Only
    // this function, which never returns, straddles the change. The low byte
    // stays zero so a string overrun cannot copy the guard.
    // Before even that, per-CPU data: locks and kprintf look at it.
    percpu_init(0, 0);
    csprng_init();
    __stack_chk_guard = csprng_u64() & ~0xFFull;

    serial_init();
    kprintf("cerberus: serial console up\n");

    if (!limine_base_revision_supported()) PANIC("bootloader does not support Limine base revision 3");
    boot_info_collect();
    kprintf("cerberus: boot info collected (%lu memory regions)\n",
            (unsigned long)g_boot_info.region_count);

    if (!fbconsole_init(g_boot_info.framebuffer)) PANIC("framebuffer console unusable");
    kprintf("cerberus: framebuffer console %ux%u cells\n", fbconsole_columns(), fbconsole_rows());

    print_banner();

    symbols_init();
    kprintf("symbols: %lu kernel symbols loaded\n", (unsigned long)symbols_count());
    pmm_init();

    gdt_init_bsp();
    cpu_features_init();
    kprintf("gdt: loaded (kernel cs=%#x ds=%#x, user cs=%#x ds=%#x, tss=%#x)\n", seg::KCODE, seg::KDATA,
            seg::UCODE, seg::UDATA, seg::TSS);
    interrupts_init();
    kprintf("idt: 256 gates loaded, IST for #DF/#NMI/#MC\n");
    vmm_init();
    kheap_init();
    // The exception stacks used so far are plain static arrays. Now that the
    // VMM exists, move them onto stacks with a guard page below.
    for (u8 slot = 1; slot <= ist::COUNT; slot++) {
        Result<vaddr_t> stack = vmm_alloc_kernel_stack(IST_STACK_SIZE);
        if (!stack.ok()) PANIC("out of memory for exception stack %u", slot);
        tss_set_ist(0, slot, stack.value());
    }
    process_init();

    pic_init();
    kprintf("pic: remapped to 0x20-0x2f and masked\n");
    acpi_init();
    hpet_init();
    refclock_init();
    lapic_init();
    ioapic_init();
    smp_init();
    lapic_timer_calibrate();
    lapic_timer_set_periodic(TIMER_HZ);
    ps2kbd_init();
    interrupts_enable();
    kprintf("timer: periodic at %u Hz, interrupts enabled\n", TIMER_HZ);

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

    // From here on everything runs as threads on guarded stacks; the
    // bootloader's stack this function runs on is left behind.
    sched_start(init_thread, nullptr);
}
