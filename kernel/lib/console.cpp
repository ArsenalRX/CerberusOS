// Fan-out to the serial driver and the active screen console. Lock-free at
// this stage; a lock arrives with the scheduler in phase 6.
#include <arch/x86_64/cpu.h>
#include <drivers/fbconsole.h>
#include <drivers/ps2kbd.h>
#include <drivers/serial.h>
#include <gui/desktop.h>
#include <lib/console.h>

void console_putc(char c) {
    serial_putc(c);
    if (gui_active()) gui_terminal_putc(c);
    else fbconsole_putc(c);
}

void console_write(const char* s, usize n) {
    for (usize i = 0; i < n; i++) console_putc(s[i]);
}

void console_puts(const char* s) {
    while (*s) console_putc(*s++);
}

int console_getc() {
    int c = serial_getc();
    if (c >= 0) return c;
    if (gui_active()) {
        gui_pump();
        return gui_terminal_getc();
    }
    return ps2kbd_getc();
}

void console_idle(bool spin) {
    if (gui_active()) {
        // While the desktop is up we poll rather than halt: some hypervisor
        // backends (VirtualBox on Hyper-V) stop delivering the periodic timer
        // interrupt to a halted vCPU, which would freeze the compositor and
        // input. Spinning keeps it responsive. The scheduler (phase 6) gives
        // the compositor its own thread and restores hlt in the idle thread.
        gui_pump();
        cpu_relax();
        return;
    }
    fbconsole_flush();
    if (spin) cpu_relax();
    else cpu_halt();
}

void console_emergency_text_mode() { gui_emergency_text_mode(); }
