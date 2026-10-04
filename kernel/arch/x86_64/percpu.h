// Per-CPU data reached through the GS segment base. While a CPU runs kernel
// code GS points at its PerCpu; while it runs user code GS holds the user's
// value and the kernel's is parked in KERNEL_GS_BASE. Every entry from ring 3
// (system call, interrupt, exception) and every return to it executes swapgs
// to exchange the two (isr_stubs.asm, usermode.asm).
//
// A thread can be moved to another CPU whenever interrupts are enabled, so a
// PerCpu pointer is only meaningful while they are off. The exceptions are
// the two single-instruction readers below, whose results stay true for the
// calling thread wherever it runs next.
#pragma once

#include <lib/types.h>

class AddressSpace;
struct Thread;

// CPUs the kernel will use (thread affinity is a 32-bit mask). Any beyond
// this stay parked.
constexpr u32 MAX_CPUS = 32;

struct PerCpu {
    u64 kernel_rsp;             // offset 0: top of the running thread's kernel stack (system-call entry loads it)
    u64 user_rsp;               // offset 8: scratch for the user stack pointer during system-call entry
    PerCpu* self;               // offset 16
    Thread* current;            // offset 24: the thread running on this CPU (null until the scheduler starts)
    u32 cpu_id;                 // offset 32: 0 is the bootstrap CPU
    u32 lapic_id;
    AddressSpace* space;        // the address space loaded in CR3
    volatile u32 online;        // set once the CPU can service requests from other CPUs
    volatile u32 tlb_pending;   // another CPU is waiting for this one to flush (smp.cpp)
    u8 held_count;              // spinlock ranks held, in the order taken (lib/lock_order.h)
    u8 held[15];
};

extern PerCpu g_percpu[MAX_CPUS];

// This CPU's data. Interrupts must be off for the result to stay valid.
static inline PerCpu* percpu() {
    PerCpu* p;
    asm volatile("movq %%gs:16, %0" : "=r"(p));
    return p;
}

// The running thread. Safe with interrupts on: the answer follows the thread.
static inline Thread* percpu_current_thread() {
    Thread* t;
    asm volatile("movq %%gs:24, %0" : "=r"(t));
    return t;
}

// The CPU the caller is on at this instant; with interrupts on it may be
// another one by the time the value is used.
static inline u32 percpu_cpu_id() {
    u32 id;
    asm volatile("movl %%gs:32, %0" : "=r"(id));
    return id;
}

// Points GS at g_percpu[cpu]. On the bootstrap CPU this is the first thing
// kernel_main does (locks and kprintf depend on it); on the others it follows
// gdt_init_cpu.
void percpu_init(u32 cpu, u32 lapic_id);
