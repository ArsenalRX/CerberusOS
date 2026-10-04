// See percpu.h.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/percpu.h>

PerCpu g_percpu[MAX_CPUS];

static_assert(__builtin_offsetof(PerCpu, kernel_rsp) == 0);
static_assert(__builtin_offsetof(PerCpu, user_rsp) == 8);
static_assert(__builtin_offsetof(PerCpu, self) == 16);
static_assert(__builtin_offsetof(PerCpu, current) == 24);
static_assert(__builtin_offsetof(PerCpu, cpu_id) == 32);

void percpu_init(u32 cpu, u32 lapic_id) {
    PerCpu& p = g_percpu[cpu];
    p.self = &p;
    p.cpu_id = cpu;
    p.lapic_id = lapic_id;
    wrmsr(msr::GS_BASE, (u64)&p);
    wrmsr(msr::KERNEL_GS_BASE, 0);      // what user mode sees after the first swapgs
}
