// IDT construction and the C++ side of interrupt handling. Unhandled
// exceptions dump everything useful and halt; unhandled IRQs are logged once.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/interrupts.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <lib/symbols.h>

extern "C" const u64 isr_stub_table[256];

namespace {

struct __attribute__((packed)) IdtEntry {
    u16 offset_lo;
    u16 selector;
    u8 ist;
    u8 type_attr;
    u16 offset_mid;
    u32 offset_hi;
    u32 reserved;
};

struct __attribute__((packed)) IdtPointer {
    u16 limit;
    u64 base;
};

constexpr u8 GATE_INTERRUPT = 0x8E;   // present, DPL 0, 64-bit interrupt gate

alignas(16) IdtEntry g_idt[256];

struct Registration {
    InterruptHandler fn;
    void* ctx;
};
Registration g_handlers[256];
u32 g_unhandled_irq_count[256];

void set_gate(u8 vector, u64 handler, u8 ist_slot, u8 type_attr) {
    IdtEntry& e = g_idt[vector];
    e.offset_lo = (u16)handler;
    e.selector = seg::KCODE;
    e.ist = ist_slot;
    e.type_attr = type_attr;
    e.offset_mid = (u16)(handler >> 16);
    e.offset_hi = (u32)(handler >> 32);
    e.reserved = 0;
}

const char* const EXCEPTION_NAMES[32] = {
    "#DE Divide Error",
    "#DB Debug",
    "NMI Non-Maskable Interrupt",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR Bound Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack-Segment Fault",
    "#GP General Protection Fault",
    "#PF Page Fault",
    "Reserved (15)",
    "#MF x87 Floating-Point Error",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD Floating-Point Exception",
    "#VE Virtualization Exception",
    "#CP Control Protection Exception",
    "Reserved (22)",
    "Reserved (23)",
    "Reserved (24)",
    "Reserved (25)",
    "Reserved (26)",
    "#HV Hypervisor Injection",
    "#VC VMM Communication",
    "#SX Security Exception",
    "Reserved (30)",
    "Reserved (31)",
};

void print_symbolised(u64 addr) {
    u64 off;
    const char* name = symbols_lookup(addr, &off);
    if (name) kprintf("%#018lx <%s+%#lx>", (unsigned long)addr, name, (unsigned long)off);
    else kprintf("%#018lx <?>", (unsigned long)addr);
}

void decode_error_code(const InterruptFrame& f) {
    switch (f.vector) {
    case 14: {
        u64 e = f.error;
        kprintf("page fault at %#018lx: %s %s %s%s%s%s\n", (unsigned long)read_cr2(),
                e & 1 ? "protection-violation" : "not-present",
                e & 2 ? "write" : "read",
                e & 4 ? "user" : "kernel",
                e & 8 ? " reserved-bit" : "",
                e & 16 ? " instruction-fetch" : "",
                e & 32 ? " protection-key" : "");
        break;
    }
    case 10: case 11: case 12: case 13: {
        u64 e = f.error;
        if (e == 0) {
            kprintf("error code 0 (not selector-related)\n");
            break;
        }
        static const char* const tables[] = {"GDT", "IDT", "LDT", "IDT"};
        kprintf("selector error: %s index %lu in %s%s\n", e & 1 ? "external" : "internal",
                (unsigned long)(e >> 3), tables[(e >> 1) & 3],
                (e >> 1) & 1 ? " (interrupt vector)" : "");
        break;
    }
    default:
        kprintf("error code %#lx\n", (unsigned long)f.error);
        break;
    }
}

[[noreturn]] void unhandled_exception(InterruptFrame* f) {
    interrupts_disable();
    kprintf("\n*** EXCEPTION %lu: %s ***\n", (unsigned long)f->vector, exception_name((u8)f->vector));
    decode_error_code(*f);
    dump_frame(*f);
    kprintf("System halted.\n");
    halt_forever();
}

} // namespace

void interrupts_init() {
    memset(g_idt, 0, sizeof g_idt);
    for (unsigned v = 0; v < 256; v++) set_gate((u8)v, isr_stub_table[v], ist::NONE, GATE_INTERRUPT);
    set_gate(8, isr_stub_table[8], ist::DOUBLE_FAULT, GATE_INTERRUPT);
    set_gate(2, isr_stub_table[2], ist::NMI, GATE_INTERRUPT);
    set_gate(18, isr_stub_table[18], ist::MACHINE_CHECK, GATE_INTERRUPT);

    IdtPointer ptr{(u16)(sizeof g_idt - 1), (u64)g_idt};
    asm volatile("lidt %0" ::"m"(ptr) : "memory");
}

bool interrupt_register(u8 vector, InterruptHandler fn, void* ctx) {
    if (g_handlers[vector].fn) return false;
    g_handlers[vector] = {fn, ctx};
    return true;
}

void interrupt_unregister(u8 vector) { g_handlers[vector] = {nullptr, nullptr}; }

const char* exception_name(u8 vector) { return vector < 32 ? EXCEPTION_NAMES[vector] : "IRQ"; }

extern "C" void interrupt_dispatch(InterruptFrame* f) {
    Registration& r = g_handlers[f->vector];
    if (r.fn) {
        r.fn(f, r.ctx);
        return;
    }
    if (f->vector < 32) unhandled_exception(f);
    // Spurious or unclaimed IRQ: report the first few occurrences, then stay quiet.
    if (g_unhandled_irq_count[f->vector]++ < 3)
        kprintf("interrupt: unhandled vector %lu (no handler registered)\n", (unsigned long)f->vector);
}

void dump_frame(const InterruptFrame& f) {
    kprintf("RIP="); print_symbolised(f.rip); kprintf("\n");
    kprintf("RAX=%016lx RBX=%016lx RCX=%016lx RDX=%016lx\n", (unsigned long)f.rax, (unsigned long)f.rbx,
            (unsigned long)f.rcx, (unsigned long)f.rdx);
    kprintf("RSI=%016lx RDI=%016lx RBP=%016lx RSP=%016lx\n", (unsigned long)f.rsi, (unsigned long)f.rdi,
            (unsigned long)f.rbp, (unsigned long)f.rsp);
    kprintf("R8 =%016lx R9 =%016lx R10=%016lx R11=%016lx\n", (unsigned long)f.r8, (unsigned long)f.r9,
            (unsigned long)f.r10, (unsigned long)f.r11);
    kprintf("R12=%016lx R13=%016lx R14=%016lx R15=%016lx\n", (unsigned long)f.r12, (unsigned long)f.r13,
            (unsigned long)f.r14, (unsigned long)f.r15);
    kprintf("CS=%04lx SS=%04lx RFLAGS=%016lx CR0=%016lx\n", (unsigned long)f.cs, (unsigned long)f.ss,
            (unsigned long)f.rflags, (unsigned long)read_cr0());
    kprintf("CR2=%016lx CR3=%016lx CR4=%016lx\n", (unsigned long)read_cr2(), (unsigned long)read_cr3(),
            (unsigned long)read_cr4());
    backtrace(f.rbp, f.rip);
}

void backtrace(u64 rbp, u64 first_rip) {
    kprintf("backtrace:\n");
    int depth = 0;
    if (first_rip) {
        kprintf("  #%d ", depth++);
        print_symbolised(first_rip);
        kprintf("\n");
    }
    // Frames live in the kernel half; anything else means the chain is broken.
    while (rbp >= 0xFFFF800000000000ull && depth < 32) {
        const u64* frame = (const u64*)rbp;
        u64 ret = frame[1];
        if (ret < 0xFFFFFFFF80000000ull) break;
        kprintf("  #%d ", depth++);
        print_symbolised(ret);
        kprintf("\n");
        if (frame[0] <= rbp) break;     // stacks grow down; a non-increasing rbp is corruption
        rbp = frame[0];
    }
    if (depth == 0) kprintf("  (empty)\n");
}
