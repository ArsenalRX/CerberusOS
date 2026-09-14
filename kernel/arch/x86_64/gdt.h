// Global Descriptor Table and Task State Segment, one set per CPU.
//
// Selector layout is dictated by syscall/sysret: SYSCALL loads CS from
// STAR[47:32] and SS from that +8; SYSRET (64-bit) loads CS from
// STAR[63:48]+16 and SS from STAR[63:48]+8. With STAR[63:48] = 0x18 that
// gives user SS = 0x20 and user CS = 0x28, so the slots below are fixed.
#pragma once

#include <lib/types.h>

namespace seg {
constexpr u16 NULL_SEL = 0x00;
constexpr u16 KCODE = 0x08;
constexpr u16 KDATA = 0x10;
constexpr u16 UCODE32 = 0x18;   // placeholder so the sysret arithmetic works out
constexpr u16 UDATA = 0x20 | 3;
constexpr u16 UCODE = 0x28 | 3;
constexpr u16 TSS = 0x30;
} // namespace seg

// IST slots used by the IDT (1-based as the hardware counts them).
namespace ist {
constexpr u8 NONE = 0;
constexpr u8 DOUBLE_FAULT = 1;
constexpr u8 NMI = 2;
constexpr u8 MACHINE_CHECK = 3;
constexpr u8 COUNT = 3;
} // namespace ist

constexpr usize KERNEL_STACK_SIZE = 16 * KIB;
constexpr usize IST_STACK_SIZE = 16 * KIB;

// Builds and loads a CPU's GDT and TSS. kernel_stack_top is the ring-0 stack
// used on ring 3 -> ring 0 transitions; ist_tops holds the top of one stack
// per IST slot (index 0 = slot 1). Must run before interrupts_init on that
// CPU, with interrupts disabled.
void gdt_init_cpu(usize cpu, vaddr_t kernel_stack_top, const vaddr_t ist_tops[ist::COUNT]);

// Convenience for the bootstrap CPU: uses statically allocated stacks.
void gdt_init_bsp();

// Changes the ring-0 stack for a CPU (the scheduler does this per thread).
void tss_set_kernel_stack(usize cpu, vaddr_t stack_top);
