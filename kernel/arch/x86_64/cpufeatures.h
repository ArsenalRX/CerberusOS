// CPU feature detection and the control-register setup that depends on it
// (SPEC §19.3 and §20.8). Detected once at boot; code paths choose from
// g_cpu rather than calling CPUID again.
#pragma once

#include <lib/types.h>

struct CpuFeatures {
    bool nx;
    bool smep;          // supervisor-mode execution prevention
    bool smap;          // supervisor-mode access prevention
    bool umip;          // user-mode instruction prevention (sgdt, sidt, ... fault in ring 3)
    bool rdrand;
    bool rdseed;
    bool erms;          // fast rep movsb/stosb
    bool pcid;
    bool invariant_tsc;
    bool x2apic;
};

extern CpuFeatures g_cpu;

// Fills g_cpu and turns on what the CPU offers:
//   - SMEP and SMAP: the kernel can no longer execute user pages, nor touch
//     them outside the user-copy routines (mm/usercopy.h);
//   - UMIP;
//   - the FPU/SSE unit for user programs (the kernel itself never uses it).
// Call once per CPU, early, with interrupts off.
void cpu_features_init();

// FPU/SSE state of user threads. The kernel is built without SSE, so the
// registers only change hands between user threads: the state is saved and
// loaded when a different user thread is about to run, not on every switch.
constexpr usize FPU_STATE_SIZE = 512;      // FXSAVE area
// Copies the clean power-on state into a new thread's save area.
void fpu_init_state(u8* area);
void fpu_save(u8* area);
void fpu_restore(const u8* area);
