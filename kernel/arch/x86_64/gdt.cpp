// Per-CPU GDT/TSS storage lives in .bss. Only the bootstrap CPU's stacks are
// static; application processors get theirs from the allocator in phase 8.
#include <arch/x86_64/gdt.h>
#include <boot/bootinfo.h>
#include <lib/panic.h>
#include <lib/string.h>

namespace {

struct __attribute__((packed)) GdtEntry {
    u16 limit_lo;
    u16 base_lo;
    u8 base_mid;
    u8 access;
    u8 gran;        // high limit nibble + flags (G, D/B, L, AVL)
    u8 base_hi;
};

struct __attribute__((packed)) TssDescriptor {
    GdtEntry low;
    u32 base_upper;
    u32 reserved;
};

struct __attribute__((packed)) Tss {
    u32 reserved0;
    u64 rsp[3];
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};

struct __attribute__((packed)) Gdt {
    GdtEntry null;
    GdtEntry kcode;
    GdtEntry kdata;
    GdtEntry ucode32;
    GdtEntry udata;
    GdtEntry ucode;
    TssDescriptor tss;
};

struct __attribute__((packed)) GdtPointer {
    u16 limit;
    u64 base;
};

struct alignas(16) CpuTables {
    Gdt gdt;
    Tss tss;
};

CpuTables g_tables[BootInfo::MAX_CPUS];
alignas(16) u8 g_bsp_kernel_stack[KERNEL_STACK_SIZE];
alignas(16) u8 g_bsp_ist_stacks[ist::COUNT][IST_STACK_SIZE];

constexpr u8 ACCESS_PRESENT = 1 << 7;
constexpr u8 ACCESS_RING3 = 3 << 5;
constexpr u8 ACCESS_CODE_DATA = 1 << 4;
constexpr u8 ACCESS_EXEC = 1 << 3;
constexpr u8 ACCESS_RW = 1 << 1;
constexpr u8 ACCESS_TSS_AVAILABLE = 0x9;
constexpr u8 GRAN_LONG = 1 << 5;
constexpr u8 GRAN_32BIT = 1 << 6;
constexpr u8 GRAN_4K = 1 << 7;

GdtEntry make_entry(u8 access, u8 gran) {
    // Base and limit are ignored in long mode for code/data; a flat 4 GiB
    // limit is filled in anyway so the 32-bit compatibility slot is sane.
    return GdtEntry{0xFFFF, 0, 0, access, (u8)(gran | 0x0F), 0};
}

TssDescriptor make_tss(const Tss* tss) {
    u64 base = (u64)tss;
    u32 limit = sizeof(Tss) - 1;
    TssDescriptor d;
    d.low.limit_lo = (u16)limit;
    d.low.base_lo = (u16)base;
    d.low.base_mid = (u8)(base >> 16);
    d.low.access = ACCESS_PRESENT | ACCESS_TSS_AVAILABLE;
    d.low.gran = (u8)((limit >> 16) & 0x0F);
    d.low.base_hi = (u8)(base >> 24);
    d.base_upper = (u32)(base >> 32);
    d.reserved = 0;
    return d;
}

void load(const Gdt* gdt) {
    GdtPointer ptr{(u16)(sizeof(Gdt) - 1), (u64)gdt};
    asm volatile("lgdt %0" ::"m"(ptr) : "memory");
    // Reload CS with a far return, then the data selectors.
    asm volatile("pushq %0\n"
                 "leaq 1f(%%rip), %%rax\n"
                 "pushq %%rax\n"
                 "lretq\n"
                 "1:\n"
                 "movw %1, %%ax\n"
                 "movw %%ax, %%ds\n"
                 "movw %%ax, %%es\n"
                 "movw %%ax, %%ss\n"
                 "xorw %%ax, %%ax\n"
                 "movw %%ax, %%fs\n"
                 "movw %%ax, %%gs\n"
                 :
                 : "i"((u64)seg::KCODE), "i"((u16)seg::KDATA)
                 : "rax", "memory");
    asm volatile("ltr %0" ::"r"((u16)seg::TSS) : "memory");
}

} // namespace

void gdt_init_cpu(usize cpu, vaddr_t kernel_stack_top, const vaddr_t ist_tops[ist::COUNT]) {
    ASSERT_ALWAYS(cpu < BootInfo::MAX_CPUS);
    CpuTables& t = g_tables[cpu];
    memset(&t, 0, sizeof t);

    t.gdt.kcode = make_entry(ACCESS_PRESENT | ACCESS_CODE_DATA | ACCESS_EXEC | ACCESS_RW, GRAN_LONG);
    t.gdt.kdata = make_entry(ACCESS_PRESENT | ACCESS_CODE_DATA | ACCESS_RW, GRAN_4K | GRAN_32BIT);
    t.gdt.ucode32 = make_entry(ACCESS_PRESENT | ACCESS_RING3 | ACCESS_CODE_DATA | ACCESS_EXEC | ACCESS_RW,
                               GRAN_4K | GRAN_32BIT);
    t.gdt.udata = make_entry(ACCESS_PRESENT | ACCESS_RING3 | ACCESS_CODE_DATA | ACCESS_RW,
                             GRAN_4K | GRAN_32BIT);
    t.gdt.ucode = make_entry(ACCESS_PRESENT | ACCESS_RING3 | ACCESS_CODE_DATA | ACCESS_EXEC | ACCESS_RW,
                             GRAN_LONG);

    t.tss.iomap_base = sizeof(Tss);     // no I/O permission bitmap
    t.tss.rsp[0] = kernel_stack_top;
    for (usize i = 0; i < ist::COUNT; i++) t.tss.ist[i] = ist_tops[i];
    t.gdt.tss = make_tss(&t.tss);

    load(&t.gdt);
}

void gdt_init_bsp() {
    vaddr_t ist_tops[ist::COUNT];
    for (usize i = 0; i < ist::COUNT; i++) ist_tops[i] = (vaddr_t)&g_bsp_ist_stacks[i][IST_STACK_SIZE];
    gdt_init_cpu(0, (vaddr_t)&g_bsp_kernel_stack[KERNEL_STACK_SIZE], ist_tops);
}

void tss_set_kernel_stack(usize cpu, vaddr_t stack_top) { g_tables[cpu].tss.rsp[0] = stack_top; }

void tss_set_ist(usize cpu, u8 slot, vaddr_t stack_top) {
    ASSERT_ALWAYS(cpu < BootInfo::MAX_CPUS && slot >= 1 && slot <= ist::COUNT);
    g_tables[cpu].tss.ist[slot - 1] = stack_top;
}
