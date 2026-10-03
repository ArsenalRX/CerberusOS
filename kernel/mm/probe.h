// Fault-safe memory probes: touch an address and report whether the access
// faulted instead of crashing the kernel. The page-fault handler first tries
// to resolve a fault normally (demand paging, copy-on-write); only an
// unresolvable fault at one of the probe instructions is turned into a
// "false" return. Used by the kernel self-tests now and by the user-copy
// routines in phase 7. Safe with interrupts on or off; not reentrant from
// interrupt handlers.
#pragma once

#include <arch/x86_64/interrupts.h>
#include <lib/types.h>

struct ProbeFault {
    vaddr_t addr;       // CR2 of the fault the last failed probe took
    u64 error;          // page-fault error code (bit 0 present, 1 write, 4 instruction fetch)
    bool faulted;
};

// Reads one byte. Returns false if the read faulted.
bool probe_read(const void* addr, u8* out);
// Writes one byte. Returns false if the write faulted.
bool probe_write(void* addr, u8 value);
// Calls `addr` as a function. Returns false if fetching its first
// instruction faulted. Only for addresses that are expected to be
// non-executable: if the call succeeds, whatever is there runs.
bool probe_exec(const void* addr);

// Details of the fault taken by the most recent probe.
ProbeFault probe_last_fault();

// Called by the page-fault handler for a fault it could not resolve.
// Returns true if the fault came from a probe and the frame was redirected.
bool probe_fixup(InterruptFrame* frame, vaddr_t addr, u64 error);
