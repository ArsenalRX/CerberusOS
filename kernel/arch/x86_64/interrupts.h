// IDT setup, interrupt dispatch, exception reporting, and register/backtrace
// dumps. Handlers run with interrupts disabled (interrupt gates).
#pragma once

#include <lib/types.h>

// Layout must match the push order in interrupts.asm.
struct InterruptFrame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector;
    u64 error;
    u64 rip, cs, rflags, rsp, ss;
};

using InterruptHandler = void (*)(InterruptFrame* frame, void* ctx);

namespace vec {
constexpr u8 IRQ_BASE = 0x20;       // legacy ISA IRQ n -> vector 0x20 + n
constexpr u8 APIC_TIMER = 0x30;
constexpr u8 IPI_RESCHED = 0xF0;    // from another CPU: look at your run queue (smp.h)
constexpr u8 IPI_TLB = 0xF1;        // from another CPU: flush your TLB
constexpr u8 APIC_ERROR = 0xFE;
constexpr u8 APIC_SPURIOUS = 0xFF;
} // namespace vec

// Builds the IDT (all 256 gates pointing at the asm stubs, IST slots for
// #DF/#NMI/#MC) and loads it. Bootstrap CPU, after gdt_init_bsp.
void interrupts_init();
// Loads the same IDT on another CPU, after its gdt_init_cpu.
void interrupts_load();

// Installs a handler for a vector. Returns false if one is already installed.
// Handlers for vectors >= 32 must acknowledge the interrupt controller.
bool interrupt_register(u8 vector, InterruptHandler fn, void* ctx = nullptr);
void interrupt_unregister(u8 vector);
// A free vector for a device interrupt (0x40-0xEF), or 0 if none is left.
// The vector is reserved; register a handler for it next.
u8 interrupt_alloc_vector();

const char* exception_name(u8 vector);
// Number of times a vector has been dispatched since boot (diagnostics).
u64 interrupt_count(u8 vector);

// Reports an exception nobody could handle (name, decoded error code,
// registers, backtrace) and halts. For handlers that tried and failed.
[[noreturn]] void exception_fatal(InterruptFrame* frame);

// Prints the full register set, decodes the error code, and walks the stack.
void dump_frame(const InterruptFrame& f);
// Walks the RBP chain from `rbp`, printing each return address with a symbol.
void backtrace(u64 rbp, u64 first_rip = 0);

extern "C" void interrupt_dispatch(InterruptFrame* frame);
extern "C" void capture_registers(InterruptFrame* out);
