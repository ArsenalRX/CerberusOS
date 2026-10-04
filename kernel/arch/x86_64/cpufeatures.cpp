// See cpufeatures.h.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpufeatures.h>
#include <arch/x86_64/cpuid.h>
#include <lib/kprintf.h>
#include <lib/string.h>

CpuFeatures g_cpu;

// Read by the assembly stubs (interrupt entry, user copy): stac/clac are only
// valid instructions when SMAP exists.
extern "C" {
u8 g_smap_enabled = 0;
}

namespace {

constexpr u64 CR0_MP = 1ull << 1;
constexpr u64 CR0_EM = 1ull << 2;
constexpr u64 CR0_NE = 1ull << 5;
constexpr u64 CR4_OSFXSR = 1ull << 9;
constexpr u64 CR4_OSXMMEXCPT = 1ull << 10;
constexpr u64 CR4_UMIP = 1ull << 11;
constexpr u64 CR4_SMEP = 1ull << 20;
constexpr u64 CR4_SMAP = 1ull << 21;

alignas(16) u8 g_fpu_clean[FPU_STATE_SIZE];

} // namespace

void cpu_features_init() {
    u32 max = cpuid_max_leaf(), max_ext = cpuid_max_ext_leaf();
    CpuidRegs l1 = cpuid(1);
    CpuidRegs l7 = max >= 7 ? cpuid(7, 0) : CpuidRegs{0, 0, 0, 0};
    CpuidRegs e1 = max_ext >= 0x80000001 ? cpuid(0x80000001) : CpuidRegs{0, 0, 0, 0};
    CpuidRegs e7 = max_ext >= 0x80000007 ? cpuid(0x80000007) : CpuidRegs{0, 0, 0, 0};

    g_cpu.nx = e1.edx & (1u << 20);
    g_cpu.rdrand = l1.ecx & (1u << 30);
    g_cpu.pcid = l1.ecx & (1u << 17);
    g_cpu.x2apic = l1.ecx & (1u << 21);
    g_cpu.smep = l7.ebx & (1u << 7);
    g_cpu.erms = l7.ebx & (1u << 9);
    g_cpu.rdseed = l7.ebx & (1u << 18);
    g_cpu.smap = l7.ebx & (1u << 20);
    g_cpu.umip = l7.ecx & (1u << 2);
    g_cpu.invariant_tsc = e7.edx & (1u << 8);

    // FPU and SSE for user programs: native FPU errors, no emulation, FXSAVE
    // and SSE exceptions enabled. Every x86-64 CPU has SSE2 and FXSR.
    write_cr0((read_cr0() & ~CR0_EM) | CR0_MP | CR0_NE);
    u64 cr4 = read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT;
    if (g_cpu.smep) cr4 |= CR4_SMEP;
    if (g_cpu.smap) cr4 |= CR4_SMAP;
    if (g_cpu.umip) cr4 |= CR4_UMIP;
    write_cr4(cr4);
    if (g_cpu.smap) {
        g_smap_enabled = 1;
        asm volatile("clac");                   // user access stays closed except inside usercopy
    }

    // Capture the clean FPU state every new user thread starts from.
    u32 mxcsr = 0x1F80;                          // all SSE exceptions masked, round to nearest
    asm volatile("fninit; ldmxcsr %0" ::"m"(mxcsr));
    asm volatile("fxsave64 (%0)" ::"r"(g_fpu_clean) : "memory");

    kprintf("cpu: protections on:%s%s%s%s; rdrand %s, rdseed %s\n", g_cpu.nx ? " NX" : "",
            g_cpu.smep ? " SMEP" : "", g_cpu.smap ? " SMAP" : "", g_cpu.umip ? " UMIP" : "",
            g_cpu.rdrand ? "yes" : "no", g_cpu.rdseed ? "yes" : "no");
    if (!g_cpu.smep || !g_cpu.smap)
        kprintf("cpu: WARNING: this CPU lacks %s%s%s; the kernel is not hardware-protected from user memory\n",
                g_cpu.smep ? "" : "SMEP", !g_cpu.smep && !g_cpu.smap ? " and " : "", g_cpu.smap ? "" : "SMAP");
}

void fpu_init_state(u8* area) { memcpy(area, g_fpu_clean, FPU_STATE_SIZE); }
void fpu_save(u8* area) { asm volatile("fxsave64 (%0)" ::"r"(area) : "memory"); }
void fpu_restore(const u8* area) { asm volatile("fxrstor64 (%0)" ::"r"(area) : "memory"); }
