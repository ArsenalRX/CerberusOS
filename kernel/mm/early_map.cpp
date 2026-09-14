// Walks the live PML4 through the HHDM, creating intermediate tables from a
// static pool. Only 4 KiB mappings are created; an existing huge page in the
// way is a bug, not something to work around.
#include <arch/x86_64/cpu.h>
#include <boot/bootinfo.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <mm/early_map.h>

namespace {

constexpr vaddr_t EARLY_MAP_BASE = 0xFFFFC00000000000ull;
constexpr usize POOL_PAGES = 32;

constexpr u64 PTE_PRESENT = 1 << 0;
constexpr u64 PTE_WRITE = 1 << 1;
constexpr u64 PTE_PWT = 1 << 3;
constexpr u64 PTE_PCD = 1 << 4;
constexpr u64 PTE_HUGE = 1 << 7;
constexpr u64 PTE_NX = 1ull << 63;
constexpr u64 PTE_ADDR_MASK = 0x000FFFFFFFFFF000ull;
constexpr u64 EFER_NXE = 1 << 11;

alignas(PAGE_SIZE) u8 g_pool[POOL_PAGES][PAGE_SIZE];
usize g_pool_used = 0;
vaddr_t g_next_virt = EARLY_MAP_BASE;

u64* table_virt(paddr_t p) { return (u64*)hhdm_virt(p); }

paddr_t alloc_table() {
    if (g_pool_used >= POOL_PAGES) PANIC("early_map: page-table pool exhausted (%lu pages)", (unsigned long)POOL_PAGES);
    u8* page = g_pool[g_pool_used++];
    memset(page, 0, PAGE_SIZE);
    return kernel_virt_to_phys(page);
}

u64* next_level(u64* table, usize index) {
    u64 e = table[index];
    if (!(e & PTE_PRESENT)) {
        paddr_t t = alloc_table();
        table[index] = t | PTE_PRESENT | PTE_WRITE;
        return table_virt(t);
    }
    if (e & PTE_HUGE) PANIC("early_map: huge page already maps this range");
    return table_virt(e & PTE_ADDR_MASK);
}

void map_page(vaddr_t v, paddr_t p, u64 flags) {
    u64* pml4 = table_virt(read_cr3() & PTE_ADDR_MASK);
    u64* pdpt = next_level(pml4, (v >> 39) & 511);
    u64* pd = next_level(pdpt, (v >> 30) & 511);
    u64* pt = next_level(pd, (v >> 21) & 511);
    pt[(v >> 12) & 511] = (p & PTE_ADDR_MASK) | flags;
    invlpg(v);
}

} // namespace

paddr_t kernel_virt_to_phys(const void* p) {
    return (u64)p - g_boot_info.kernel_virt_base + g_boot_info.kernel_phys_base;
}

void* hhdm_virt(paddr_t p) { return (void*)(p + g_boot_info.hhdm_offset); }

void* early_map(paddr_t phys, usize size, MapCache cache) {
    if (read_cr4() & (1 << 12)) PANIC("early_map: 5-level paging is not supported");
    paddr_t first = align_down(phys, PAGE_SIZE);
    paddr_t last = align_up(phys + size, PAGE_SIZE);
    u64 flags = PTE_PRESENT | PTE_WRITE;
    if (cache == MapCache::Uncached) flags |= PTE_PCD | PTE_PWT;
    if (rdmsr(msr::EFER) & EFER_NXE) flags |= PTE_NX;

    vaddr_t base = g_next_virt;
    for (paddr_t p = first; p < last; p += PAGE_SIZE, g_next_virt += PAGE_SIZE) map_page(g_next_virt, p, flags);
    return (void*)(base + (phys - first));
}
