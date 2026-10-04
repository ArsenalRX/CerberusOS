// Per-CPU data reached through the GS segment base. While the CPU runs
// kernel code GS points at this CPU's PerCpu; while it runs user code GS
// holds the user's value and the kernel's is parked in KERNEL_GS_BASE. Every
// entry from ring 3 (system call, interrupt, exception) and every return to
// it executes swapgs to exchange the two (isr_stubs.asm, usermode.asm).
// One CPU until phase 8.
#pragma once

#include <lib/types.h>

struct PerCpu {
    u64 kernel_rsp;     // offset 0: top of the running thread's kernel stack (system-call entry loads it)
    u64 user_rsp;       // offset 8: scratch for the user stack pointer during system-call entry
    u32 cpu_id;
};

// Points GS at the bootstrap CPU's PerCpu. Call after the GDT is loaded
// (loading segment registers can clear the GS base).
void percpu_init_bsp();
// The scheduler calls this on every switch, alongside the TSS update.
void percpu_set_kernel_stack(vaddr_t stack_top);
