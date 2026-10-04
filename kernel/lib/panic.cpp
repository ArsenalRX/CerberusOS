// Panic: report and stop. Interrupts are disabled so nothing can preempt the
// message, and every other CPU is stopped with a non-maskable interrupt
// before anything is printed. The register snapshot is taken here, so
// RIP/RSP point into panic_impl itself and the first backtrace frames are
// the panic machinery.
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <drivers/fbconsole.h>
#include <lib/console.h>
#include <lib/kprintf.h>
#include <lib/panic.h>

namespace {
volatile u32 g_panicking = 0;
}

bool panic_in_progress() { return __atomic_load_n(&g_panicking, __ATOMIC_RELAXED) != 0; }

void panic_begin() {
    asm volatile("cli");
    // Only the first CPU to get here reports; a second failure on another
    // CPU at the same moment would garble both messages.
    if (__atomic_exchange_n(&g_panicking, 1u, __ATOMIC_SEQ_CST)) {
        for (;;) asm volatile("cli; hlt");
    }
    smp_stop_others();
}

[[noreturn]] void halt_forever() {
    fbconsole_flush();
    for (;;) asm volatile("cli; hlt");
}

[[noreturn]] void panic_impl(const char* file, int line, const char* fmt, ...) {
    asm volatile("cli");
    InterruptFrame regs;
    capture_registers(&regs);
    panic_begin();
    console_emergency_text_mode();
    kprintf("\n*** KERNEL PANIC at %s:%d ***\n", file, line);
    if (smp_cpu_count() > 1) kprintf("cpu %u\n", percpu_cpu_id());
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
    dump_frame(regs);
    kprintf("System halted.\n");
    halt_forever();
}
