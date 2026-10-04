// Privileged x86-64 instructions and MSR/control-register access as inline
// functions. Everything here is a single instruction; safe in any context.
#pragma once

#include <lib/types.h>

namespace msr {
constexpr u32 APIC_BASE = 0x1B;
constexpr u32 PAT = 0x277;      // page attribute table: the cache type behind each PTE PAT/PCD/PWT combination
constexpr u32 EFER = 0xC0000080;
constexpr u32 STAR = 0xC0000081;
constexpr u32 LSTAR = 0xC0000082;
constexpr u32 SFMASK = 0xC0000084;
constexpr u32 FS_BASE = 0xC0000100;
constexpr u32 GS_BASE = 0xC0000101;
constexpr u32 KERNEL_GS_BASE = 0xC0000102;
} // namespace msr

static inline u64 rdmsr(u32 msr) {
    u32 lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}
static inline void wrmsr(u32 msr, u64 v) {
    asm volatile("wrmsr" ::"c"(msr), "a"((u32)v), "d"((u32)(v >> 32)) : "memory");
}

static inline u64 read_cr0() { u64 v; asm volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline u64 read_cr2() { u64 v; asm volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline u64 read_cr3() { u64 v; asm volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline u64 read_cr4() { u64 v; asm volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr0(u64 v) { asm volatile("mov %0, %%cr0" ::"r"(v) : "memory"); }
static inline void write_cr3(u64 v) { asm volatile("mov %0, %%cr3" ::"r"(v) : "memory"); }
static inline void write_cr4(u64 v) { asm volatile("mov %0, %%cr4" ::"r"(v) : "memory"); }

static inline u64 read_rflags() {
    u64 f;
    asm volatile("pushfq; popq %0" : "=r"(f));
    return f;
}
static inline u64 read_rbp() {
    u64 v;
    asm volatile("mov %%rbp, %0" : "=r"(v));
    return v;
}
static inline u64 read_rsp() {
    u64 v;
    asm volatile("mov %%rsp, %0" : "=r"(v));
    return v;
}

static inline void interrupts_enable() { asm volatile("sti" ::: "memory"); }
static inline void interrupts_disable() { asm volatile("cli" ::: "memory"); }
static inline bool interrupts_enabled() { return read_rflags() & (1 << 9); }
// Saves the interrupt flag and disables interrupts; pair with interrupts_restore.
static inline u64 interrupts_save() {
    u64 f = read_rflags();
    interrupts_disable();
    return f;
}
static inline void interrupts_restore(u64 flags) {
    if (flags & (1 << 9)) interrupts_enable();
}

static inline void cpu_halt() { asm volatile("hlt"); }
static inline void cpu_relax() { asm volatile("pause"); }
static inline void invlpg(vaddr_t v) { asm volatile("invlpg (%0)" ::"r"(v) : "memory"); }
static inline void memory_barrier() { asm volatile("mfence" ::: "memory"); }
static inline u64 rdtsc() {
    u32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}
