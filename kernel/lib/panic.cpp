// Panic: report and stop. Interrupts are disabled so nothing can preempt the
// message; other CPUs are stopped via IPI once SMP exists (phase 8). The
// register snapshot is taken here, so RIP/RSP point into panic_impl itself and
// the first backtrace frames are the panic machinery.
#include <arch/x86_64/interrupts.h>
#include <drivers/fbconsole.h>
#include <lib/console.h>
#include <lib/kprintf.h>
#include <lib/panic.h>

[[noreturn]] void halt_forever() {
    fbconsole_flush();
    for (;;) asm volatile("cli; hlt");
}

[[noreturn]] void panic_impl(const char* file, int line, const char* fmt, ...) {
    asm volatile("cli");
    InterruptFrame regs;
    capture_registers(&regs);
    console_emergency_text_mode();
    kprintf("\n*** KERNEL PANIC at %s:%d ***\n", file, line);
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
    dump_frame(regs);
    kprintf("System halted.\n");
    halt_forever();
}
