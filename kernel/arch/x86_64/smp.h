// More than one CPU (SPEC phase 8): bringing the other processors up, the
// interrupts CPUs send each other, and TLB shootdown.
//
// Start-up. The bootloader starts every processor and leaves it waiting; at
// boot each one is moved into a short wait loop in kernel text
// (boot_park_aps, called from vmm_init). smp_init releases them: each loads
// its own GDT, TSS, IDT, per-CPU data and control registers, enables its
// local APIC, reports in, and sleeps until smp_start_scheduling, when it
// starts its timer and enters the scheduler.
//
// Inter-processor interrupts:
//   vec::IPI_RESCHED  "look at your run queue": a thread was queued for a
//                     CPU that should run it now. The handler does nothing;
//                     the switch happens on the way out of the interrupt.
//   vec::IPI_TLB      "flush your TLB": see tlb_shootdown.
//   NMI               sent to all others by a panic: they stop.
#pragma once

#include <lib/types.h>

class AddressSpace;

// Bootstrap CPU, after lapic_init and before the timer is calibrated: brings
// every other processor to the point where it waits for the scheduler.
// A processor that does not report in within a few seconds is left out with
// a warning.
void smp_init();
// Bootstrap CPU, from sched_start: the waiting processors enter the scheduler.
void smp_start_scheduling();
// Processors in use (1 until smp_init has run).
u32 smp_cpu_count();
// True if the CPU with this id came up.
bool smp_cpu_present(u32 cpu);

// Entry of an application processor, called from its wait loop in
// boot/limine_requests.cpp. Never returns.
[[noreturn]] void smp_ap_main(u32 cpu, u32 lapic_id);

void smp_send_resched(u32 cpu);
// Stops every other CPU for good (panic).
void smp_stop_others();

// Makes every other CPU that could hold a stale translation for
// [start, start + size) of `as` drop it, and returns once they all have.
// For a user space those are the CPUs that have it loaded; for the kernel
// space (or as == nullptr), all of them. size == 0 means everything. The
// caller has already flushed its own TLB. Callable with spinlocks held and
// interrupts off: a CPU that is waiting for a spinlock keeps answering
// (smp_poll), so holding the address-space lock here cannot deadlock.
void tlb_shootdown(const AddressSpace* as, vaddr_t start, usize size);
// Services a pending flush request for this CPU, if any. Called from every
// loop that waits with interrupts off. Interrupts must be off.
void smp_poll();
// Shootdown requests made since boot (diagnostics and tests).
u64 smp_tlb_shootdowns();
