// Panic: report and stop. Interrupts are disabled so nothing can preempt the
// message; other CPUs are stopped via IPI once SMP exists (phase 8).
#include <lib/kprintf.h>
#include <lib/panic.h>

[[noreturn]] void halt_forever() {
    for (;;) asm volatile("cli; hlt");
}

[[noreturn]] void panic_impl(const char* file, int line, const char* fmt, ...) {
    asm volatile("cli");
    kprintf("\n*** KERNEL PANIC at %s:%d ***\n", file, line);
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\nSystem halted.\n");
    halt_forever();
}
