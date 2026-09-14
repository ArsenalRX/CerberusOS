// Thin CPUID wrapper. Brand string leaves 0x80000002-4 are trimmed of leading
// spaces because vendors pad them.
#include <arch/x86_64/cpuid.h>
#include <lib/string.h>

CpuidRegs cpuid(u32 leaf, u32 subleaf) {
    CpuidRegs r;
    asm volatile("cpuid" : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx) : "a"(leaf), "c"(subleaf));
    return r;
}

u32 cpuid_max_leaf() { return cpuid(0).eax; }
u32 cpuid_max_ext_leaf() { return cpuid(0x80000000).eax; }

void cpuid_vendor(char* buf) {
    CpuidRegs r = cpuid(0);
    memcpy(buf + 0, &r.ebx, 4);
    memcpy(buf + 4, &r.edx, 4);
    memcpy(buf + 8, &r.ecx, 4);
    buf[12] = 0;
}

void cpuid_brand(char* buf) {
    if (cpuid_max_ext_leaf() < 0x80000004) {
        strlcpy(buf, "unknown", 49);
        return;
    }
    char raw[49];
    for (u32 i = 0; i < 3; i++) {
        CpuidRegs r = cpuid(0x80000002 + i);
        memcpy(raw + i * 16 + 0, &r.eax, 4);
        memcpy(raw + i * 16 + 4, &r.ebx, 4);
        memcpy(raw + i * 16 + 8, &r.ecx, 4);
        memcpy(raw + i * 16 + 12, &r.edx, 4);
    }
    raw[48] = 0;
    const char* p = raw;
    while (*p == ' ') p++;
    strlcpy(buf, p, 49);
}
