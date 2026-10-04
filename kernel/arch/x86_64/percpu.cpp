// See percpu.h.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/percpu.h>

namespace {
PerCpu g_bsp;
}

static_assert(__builtin_offsetof(PerCpu, kernel_rsp) == 0);
static_assert(__builtin_offsetof(PerCpu, user_rsp) == 8);

void percpu_init_bsp() {
    g_bsp.cpu_id = 0;
    wrmsr(msr::GS_BASE, (u64)&g_bsp);
    wrmsr(msr::KERNEL_GS_BASE, 0);      // what user mode sees after the first swapgs
}

void percpu_set_kernel_stack(vaddr_t stack_top) { g_bsp.kernel_rsp = stack_top; }
