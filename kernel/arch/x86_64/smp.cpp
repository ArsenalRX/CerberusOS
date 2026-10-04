// See smp.h.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpufeatures.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <boot/bootinfo.h>
#include <drivers/lapic.h>
#include <drivers/refclock.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <mm/vmm.h>
#include <sched/sched.h>
#include <sched/sync.h>
#include <syscall/syscall.h>

namespace {

struct ApStacks {
    vaddr_t kernel;                 // ring-0 stack in the TSS until the scheduler sets a thread's
    vaddr_t ist[ist::COUNT];
};
ApStacks g_ap_stacks[MAX_CPUS];

u32 g_cpu_count = 1;                // processors in use, the bootstrap CPU included
volatile u32 g_reported = 1;        // processors that finished their own set-up
volatile u32 g_sched_go = 0;

// One shootdown at a time. The request is read by the target CPUs while the
// initiator holds the lock and waits for them.
Spinlock g_tlb_lock = SPINLOCK_RANKED(lock_rank::TLB);
struct {
    vaddr_t start;
    usize size;
} g_tlb_request;
u64 g_tlb_shootdowns = 0;

void resched_handler(InterruptFrame*, void*) { lapic_eoi(); }

void tlb_handler(InterruptFrame*, void*) {
    smp_poll();
    lapic_eoi();
}

void nmi_handler(InterruptFrame* f, void*) {
    // The only NMI this kernel sends is "stop": another CPU is reporting a
    // fatal error. Anything else is hardware asking for attention.
    if (panic_in_progress()) {
        for (;;) asm volatile("cli; hlt");
    }
    exception_fatal(f);
}

} // namespace

u32 smp_cpu_count() { return g_cpu_count; }
bool smp_cpu_present(u32 cpu) { return cpu < MAX_CPUS && g_percpu[cpu].online; }
u64 smp_tlb_shootdowns() { return g_tlb_shootdowns; }

void smp_init() {
    PerCpu* me = percpu();
    me->lapic_id = lapic_id();
    me->online = 1;
    if (!interrupt_register(vec::IPI_RESCHED, resched_handler) || !interrupt_register(vec::IPI_TLB, tlb_handler) ||
        !interrupt_register(2, nmi_handler))
        PANIC("smp: an inter-processor interrupt vector is already claimed");

    usize waiting = boot_ap_count();
    if (waiting > MAX_CPUS - 1) waiting = MAX_CPUS - 1;
    if (waiting == 0) {
        kprintf("smp: 1 CPU; nothing to start\n");
        return;
    }
    for (usize cpu = 1; cpu <= waiting; cpu++) {
        ApStacks& s = g_ap_stacks[cpu];
        Result<vaddr_t> k = vmm_alloc_kernel_stack(KERNEL_STACK_SIZE);
        if (!k.ok()) PANIC("smp: out of memory for cpu %lu's stacks", (unsigned long)cpu);
        s.kernel = k.value();
        for (usize i = 0; i < ist::COUNT; i++) {
            Result<vaddr_t> stack = vmm_alloc_kernel_stack(IST_STACK_SIZE);
            if (!stack.ok()) PANIC("smp: out of memory for cpu %lu's stacks", (unsigned long)cpu);
            s.ist[i] = stack.value();
        }
    }

    boot_release_aps();
    u64 deadline = refclock_now_us() + 5000000;
    while (__atomic_load_n(&g_reported, __ATOMIC_ACQUIRE) < waiting + 1 && refclock_now_us() < deadline) cpu_relax();
    u32 up = __atomic_load_n(&g_reported, __ATOMIC_ACQUIRE);
    if (up < waiting + 1)
        kprintf("smp: WARNING: %lu of %lu other processor(s) did not report in; continuing without them\n",
                (unsigned long)(waiting + 1 - up), (unsigned long)waiting);
    // Ids are handed out in the bootloader's order, so the highest id that
    // might be present is the number that were released.
    g_cpu_count = (u32)waiting + 1;
    kprintf("smp: %u CPUs online\n", up);
}

[[noreturn]] void smp_ap_main(u32 cpu, u32 lapic) {
    // Still on the bootloader's stack, with its GDT and no per-CPU data:
    // nothing here may take a lock or print until percpu_init has run.
    vmm_init_cpu();
    gdt_init_cpu(cpu, g_ap_stacks[cpu].kernel, g_ap_stacks[cpu].ist);
    percpu_init(cpu, lapic);
    percpu()->space = &vmm_kernel();
    interrupts_load();
    cpu_features_init_cpu();
    syscall_init();
    lapic_init_cpu();
    percpu()->online = 1;
    kprintf("smp: cpu %u online (lapic id %u)\n", cpu, lapic);
    __atomic_add_fetch(&g_reported, 1u, __ATOMIC_RELEASE);

    // Sleep until the scheduler exists. The check and the halt cannot be
    // separated by the wake-up interrupt: `sti` takes effect only after the
    // instruction that follows it.
    for (;;) {
        interrupts_disable();
        if (__atomic_load_n(&g_sched_go, __ATOMIC_ACQUIRE)) break;
        asm volatile("sti; hlt");
    }
    lapic_timer_start_cpu();
    sched_enter_ap();
}

void smp_start_scheduling() {
    __atomic_store_n(&g_sched_go, 1u, __ATOMIC_RELEASE);
    u32 me = percpu_cpu_id();
    for (u32 cpu = 0; cpu < g_cpu_count; cpu++)
        if (cpu != me && g_percpu[cpu].online) lapic_send_ipi(g_percpu[cpu].lapic_id, vec::IPI_RESCHED);
}

void smp_send_resched(u32 cpu) { lapic_send_ipi(g_percpu[cpu].lapic_id, vec::IPI_RESCHED); }

void smp_stop_others() {
    if (g_cpu_count > 1) lapic_send_nmi_to_others();
}

void smp_poll() {
    PerCpu* me = percpu();
    if (!__atomic_load_n(&me->tlb_pending, __ATOMIC_ACQUIRE)) return;
    // The initiator holds g_tlb_lock until this CPU clears its flag, so the
    // request cannot change underneath.
    vmm_flush_local(g_tlb_request.start, g_tlb_request.size);
    __atomic_store_n(&me->tlb_pending, 0u, __ATOMIC_RELEASE);
}

void tlb_shootdown(const AddressSpace* as, vaddr_t start, usize size) {
    if (g_cpu_count == 1) return;
    u64 irq = interrupts_save();
    u32 me = percpu_cpu_id();
    bool everyone = !as || as->is_kernel();
    u32 targets = 0;
    for (u32 cpu = 0; cpu < g_cpu_count; cpu++) {
        if (cpu == me || !g_percpu[cpu].online) continue;
        // A CPU that loads `as` after this check starts with an empty TLB,
        // and it publishes the space before loading it, so none is missed.
        if (everyone || __atomic_load_n(&g_percpu[cpu].space, __ATOMIC_ACQUIRE) == as) targets |= 1u << cpu;
    }
    if (targets) {
        g_tlb_lock.acquire();
        g_tlb_request = {start, size};
        g_tlb_shootdowns++;
        for (u32 cpu = 0; cpu < g_cpu_count; cpu++) {
            if (!(targets & (1u << cpu))) continue;
            __atomic_store_n(&g_percpu[cpu].tlb_pending, 1u, __ATOMIC_RELEASE);
            lapic_send_ipi(g_percpu[cpu].lapic_id, vec::IPI_TLB);
        }
        for (u32 cpu = 0; cpu < g_cpu_count; cpu++) {
            if (!(targets & (1u << cpu))) continue;
            while (__atomic_load_n(&g_percpu[cpu].tlb_pending, __ATOMIC_ACQUIRE)) {
                cpu_relax();
                if (panic_in_progress()) break;
            }
        }
        g_tlb_lock.release();
    }
    interrupts_restore(irq);
}
