// CPUID queries: vendor string, brand string, and a few feature bits used by
// early boot. All are pure register reads; safe anywhere.
#pragma once

#include <lib/types.h>

struct CpuidRegs {
    u32 eax, ebx, ecx, edx;
};

CpuidRegs cpuid(u32 leaf, u32 subleaf = 0);

// Writes the 12-character vendor id ("GenuineIntel") into buf (>= 13 bytes).
void cpuid_vendor(char* buf);
// Writes the brand string (up to 48 chars) into buf (>= 49 bytes); falls back
// to "unknown" when the extended leaves are absent.
void cpuid_brand(char* buf);
u32 cpuid_max_leaf();
u32 cpuid_max_ext_leaf();
