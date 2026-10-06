// Phase 4 acceptance (SPEC §5 and §5A), kept current with phase 7: map/write/
// read/unmap with the right fault address, W^X, the null guard, kernel image
// protections, demand paging, zero-on-reuse, VMA splitting, guard pages,
// copy-on-write, 2 MiB kernel mappings, SMAP, and no leaked frames.
//
// User memory is reached only through the user-copy routines (the kernel may
// not touch it any other way); a failed copy reports the fault it took.
// Faults on kernel addresses that are expected go through mm/probe.h.
#include <arch/x86_64/cpufeatures.h>
#include <kernel/ktest.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/early_map.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/probe.h>
#include <mm/usercopy.h>
#include <mm/vmm.h>

extern "C" {
extern char __text_start[];
}

namespace {

constexpr u64 PF_PRESENT = 1, PF_WRITE = 2, PF_FETCH = 16;
constexpr vaddr_t RAW_USER = 0x40000000ull;
constexpr vaddr_t RAW_KERNEL = 0xFFFFD00000000000ull;      // far from anything mmap hands out

const char g_ro_text[] = "read-only data lives here";
u8 g_data_bytes[64];
u8 g_page[PAGE_SIZE];

bool uput(vaddr_t at, const void* from, usize n) { return copy_to_user(at, from, n).ok(); }
bool uget(vaddr_t at, void* to, usize n) { return copy_from_user(to, at, n).ok(); }
bool uput8(vaddr_t at, u8 v) { return uput(at, &v, 1); }
// Reads one user byte; a failed read returns a value no test writes.
u8 uget8(vaddr_t at) {
    u8 v = 0;
    return uget(at, &v, 1) ? v : 0xEE;
}

bool user_page_is_zero(vaddr_t at) {
    if (!uget(at, g_page, PAGE_SIZE)) return false;
    for (usize i = 0; i < PAGE_SIZE; i++)
        if (g_page[i]) return false;
    return true;
}

bool user_str_is(vaddr_t at, const char* expect) {
    char buf[32];
    usize n = strlen(expect) + 1;
    return n <= sizeof buf && uget(at, buf, n) && memcmp(buf, expect, n) == 0;
}

bool user_str_put(vaddr_t at, const char* s) { return uput(at, s, strlen(s) + 1); }

} // namespace

int ktest_vmm(int, char**) {
    AddressSpace& k = vmm_kernel();
    PmmStats pm0 = pmm_stats();
    VmmStats vs0 = vmm_stats();
    KheapStats hs0 = kheap_stats();
    u8 byte = 0;

    Result<AddressSpace*> made = AddressSpace::create();
    KTEST_CHECK(made.ok());
    AddressSpace* as = made.value();
    as->activate();

    // --- raw map, write, read back, unmap, fault with the right address ---
    paddr_t frame = pmm_alloc_zeroed(1);
    KTEST_CHECK(frame != PMM_NO_MEMORY);
    KTEST_CHECK(as->map(RAW_USER, frame, PAGE_SIZE, vm::WRITE | vm::USER).ok());
    KTEST_CHECK(as->map(RAW_USER, frame, PAGE_SIZE, vm::WRITE | vm::USER).error() == Error::Exists);
    u64 word = 0x1122334455667788ull, back = 0;
    KTEST_CHECK(uput(RAW_USER, &word, sizeof word) && uget(RAW_USER, &back, sizeof back) && back == word);
    KTEST_CHECK(*(u64*)hhdm_virt(frame) == word);
    Result<paddr_t> tr = as->translate(RAW_USER + 0x123);
    KTEST_CHECK(tr.ok() && tr.value() == frame + 0x123);

    // --- SMAP: the kernel cannot touch that page except through a user copy ---
    if (g_cpu.smap) {
        KTEST_CHECK(!probe_read((void*)RAW_USER, &byte));
        KTEST_CHECK(probe_last_fault().addr == RAW_USER && (probe_last_fault().error & PF_PRESENT));
        kprintf("  SMAP: a direct kernel read of a mapped user page faults; the user-copy routines succeed\n");
    } else {
        kprintf("  SMAP: not supported by this CPU; skipped\n");
    }

    KTEST_CHECK(as->unmap(RAW_USER, PAGE_SIZE).ok());
    KTEST_CHECK(!uget(RAW_USER, &byte, 1));
    UsercopyFault uf = usercopy_last_fault();
    KTEST_CHECK(uf.addr == RAW_USER && !(uf.error & PF_PRESENT));
    KTEST_CHECK(as->translate(RAW_USER).error() == Error::NotFound);
    kprintf("  map/write/read/unmap: access after unmap faulted at %#lx (not-present)\n", (unsigned long)uf.addr);

    // --- W^X and the null guard ---
    KTEST_CHECK(as->map(RAW_USER, frame, PAGE_SIZE, vm::WRITE | vm::EXEC | vm::USER).error() == Error::Invalid);
    KTEST_CHECK(k.map(RAW_KERNEL, frame, PAGE_SIZE, vm::WRITE | vm::EXEC).error() == Error::Invalid);
    KTEST_CHECK(as->mmap(0, PAGE_SIZE, vm::WRITE | vm::EXEC, 0).error() == Error::Invalid);
    KTEST_CHECK(as->map(0, frame, PAGE_SIZE, vm::USER).error() == Error::Invalid);
    KTEST_CHECK(as->mmap(0x1000, PAGE_SIZE, vm::WRITE, mmap_flag::FIXED).error() == Error::Invalid);
    KTEST_CHECK(!uget(8, &byte, 1));
    pmm_free(frame, 1);
    kprintf("  W+X requests rejected (map, mmap); the first 64 KiB cannot be mapped\n");

    // --- kernel image protections ---
    KTEST_CHECK(!probe_write((void*)g_ro_text, (u8)g_ro_text[0]));
    ProbeFault pf = probe_last_fault();
    KTEST_CHECK((pf.error & (PF_PRESENT | PF_WRITE)) == (PF_PRESENT | PF_WRITE));
    KTEST_CHECK(probe_read(__text_start, &byte));
    KTEST_CHECK(!probe_write(__text_start, byte));
    pf = probe_last_fault();
    KTEST_CHECK((pf.error & (PF_PRESENT | PF_WRITE)) == (PF_PRESENT | PF_WRITE));
    KTEST_CHECK(!probe_exec(g_data_bytes));
    pf = probe_last_fault();
    KTEST_CHECK(pf.addr == (vaddr_t)g_data_bytes && (pf.error & PF_FETCH) && (pf.error & PF_PRESENT));
    kprintf("  kernel image: writes to .rodata and .text fault; a jump into .data faults "
            "(instruction fetch, error %#lx)\n", (unsigned long)pf.error);

    // --- demand paging ---
    Result<vaddr_t> reg = as->mmap(0, 64 * PAGE_SIZE, vm::WRITE, 0);
    KTEST_CHECK(reg.ok());
    vaddr_t region = reg.value();
    VmmStats before = vmm_stats();
    KTEST_CHECK(as->translate(region + 5 * PAGE_SIZE).error() == Error::NotFound);
    KTEST_CHECK(uget8(region + 5 * PAGE_SIZE) == 0);
    KTEST_CHECK(uput8(region + 1 * PAGE_SIZE, 1) && uput8(region + 2 * PAGE_SIZE, 2) &&
                uput8(region + 3 * PAGE_SIZE + 77, 3));
    VmmStats after = vmm_stats();
    KTEST_CHECK(after.demand_faults == before.demand_faults + 4);
    KTEST_CHECK(after.anon_frames == before.anon_frames + 4);
    KTEST_CHECK(as->translate(region + 6 * PAGE_SIZE).error() == Error::NotFound);
    KTEST_CHECK(uget8(region + 3 * PAGE_SIZE + 77) == 3 && uget8(region + 3 * PAGE_SIZE) == 0);
    kprintf("  demand paging: 64 pages reserved, 4 touched, 4 frames allocated\n");

    // --- a reused frame arrives zeroed ---
    // A fault takes a frame from the pre-zeroed pool when one is ready, so
    // the frame just freed need not be the one mapped; whatever is mapped
    // must be all zero, and so must a frame zeroed on demand (count > 1
    // never comes from the pool).
    paddr_t dirty = pmm_alloc(1);
    KTEST_CHECK(dirty != PMM_NO_MEMORY);
    memset(hhdm_virt(dirty), 0xA5, PAGE_SIZE);
    pmm_free(dirty, 1);
    KTEST_CHECK(user_page_is_zero(region + 10 * PAGE_SIZE));
    Result<paddr_t> got = as->translate(region + 10 * PAGE_SIZE);
    KTEST_CHECK(got.ok());
    paddr_t two = pmm_alloc_zeroed(2);
    KTEST_CHECK(two != PMM_NO_MEMORY);
    bool clean = true;
    for (usize i = 0; i < 2 * PAGE_SIZE; i++) clean &= ((const u8*)hhdm_virt(two))[i] == 0;
    KTEST_CHECK(clean);
    pmm_free(two, 2);
    kprintf("  zeroing: frame %#lx held 0xA5 bytes, was freed, and the next user page came all zero (%u frames pre-zeroed)\n",
            (unsigned long)dirty, pmm_zeroed_ready());

    // --- data pages are not executable; mprotect ---
    KTEST_CHECK(!probe_exec((void*)(region + 1 * PAGE_SIZE)));
    KTEST_CHECK(probe_last_fault().error & PF_FETCH);
    KTEST_CHECK(as->mprotect(region, 4 * PAGE_SIZE, 0).ok());
    KTEST_CHECK(!uput8(region + 1 * PAGE_SIZE, 9));
    KTEST_CHECK(uget8(region + 1 * PAGE_SIZE) == 1);
    KTEST_CHECK(as->mprotect(region, 4 * PAGE_SIZE, vm::WRITE | vm::EXEC).error() == Error::Invalid);
    KTEST_CHECK(as->mprotect(region, 4 * PAGE_SIZE, vm::WRITE).ok());
    KTEST_CHECK(uput8(region + 1 * PAGE_SIZE, 11) && uget8(region + 1 * PAGE_SIZE) == 11);
    kprintf("  mprotect: read-only pages refuse writes; W+X refused; writable again works\n");

    // --- munmap splits a VMA ---
    usize vmas_before = as->vma_count();
    KTEST_CHECK(as->munmap(region + 20 * PAGE_SIZE, 2 * PAGE_SIZE).ok());
    KTEST_CHECK(as->vma_count() == vmas_before + 1);
    KTEST_CHECK(as->find_vma(region + 19 * PAGE_SIZE) && as->find_vma(region + 22 * PAGE_SIZE));
    KTEST_CHECK(!as->find_vma(region + 20 * PAGE_SIZE) && !as->find_vma(region + 21 * PAGE_SIZE));
    KTEST_CHECK(!uget(region + 20 * PAGE_SIZE, &byte, 1));
    kprintf("  munmap: hole punched in the middle of a region; the hole faults\n");

    // --- guard page below a stack-style mapping ---
    Result<vaddr_t> st = as->mmap(0, 4 * PAGE_SIZE, vm::WRITE, mmap_flag::GUARD_BELOW);
    KTEST_CHECK(st.ok());
    const Vma* guard = as->find_vma(st.value() - 1);
    KTEST_CHECK(guard && guard->kind == VmaKind::Guard);
    KTEST_CHECK(uput8(st.value(), 1));
    KTEST_CHECK(!uput8(st.value() - 8, 1));
    kprintf("  guard page: the page below the mapping is reserved and faults\n");

    // --- copy-on-write clone ---
    vaddr_t cow = region + 30 * PAGE_SIZE;
    KTEST_CHECK(user_str_put(cow, "original"));
    VmmStats c0 = vmm_stats();
    Result<AddressSpace*> cl = as->clone();
    KTEST_CHECK(cl.ok());
    AddressSpace* child = cl.value();
    Result<paddr_t> shared_parent = as->translate(cow), shared_child = child->translate(cow);
    KTEST_CHECK(shared_parent.ok() && shared_child.ok() && shared_parent.value() == shared_child.value());
    KTEST_CHECK(user_str_put(cow, "parent-wrote"));
    child->activate();
    KTEST_CHECK(user_str_is(cow, "original"));
    KTEST_CHECK(uget8(region + 1 * PAGE_SIZE) == 11);
    KTEST_CHECK(user_str_put(cow, "child-wrote"));
    KTEST_CHECK(user_str_is(cow, "child-wrote"));
    as->activate();
    KTEST_CHECK(user_str_is(cow, "parent-wrote"));
    VmmStats c1 = vmm_stats();
    Result<paddr_t> own_parent = as->translate(cow), own_child = child->translate(cow);
    KTEST_CHECK(own_parent.ok() && own_child.ok() && own_parent.value() != own_child.value());
    KTEST_CHECK(c1.cow_faults == c0.cow_faults + 2 && c1.cow_copies == c0.cow_copies + 1);
    // A page neither side wrote is still one shared frame.
    Result<paddr_t> ro_parent = as->translate(region + 2 * PAGE_SIZE), ro_child = child->translate(region + 2 * PAGE_SIZE);
    KTEST_CHECK(ro_parent.ok() && ro_child.ok() && ro_parent.value() == ro_child.value());
    kprintf("  copy-on-write: parent and child each see only their own write "
            "(2 write faults, 1 page copied; untouched pages stay shared)\n");

    // --- 2 MiB kernel mapping ---
    paddr_t big = pmm_alloc(1024);
    KTEST_CHECK(big != PMM_NO_MEMORY);
    paddr_t aligned = align_up(big, 2 * MIB);
    VmmStats h0 = vmm_stats();
    KTEST_CHECK(k.map(RAW_KERNEL, aligned, 2 * MIB, vm::WRITE).ok());
    KTEST_CHECK(vmm_stats().huge_mappings == h0.huge_mappings + 1);
    volatile u64* far_word = (volatile u64*)(RAW_KERNEL + MIB + 8);
    *far_word = 0xCAFEF00Dull;
    KTEST_CHECK(*far_word == 0xCAFEF00Dull && *(u64*)hhdm_virt(aligned + MIB + 8) == 0xCAFEF00Dull);
    Result<paddr_t> ht = k.translate(RAW_KERNEL + 0x123456);
    KTEST_CHECK(ht.ok() && ht.value() == aligned + 0x123456);
    KTEST_CHECK(k.unmap(RAW_KERNEL, 2 * MIB).ok());
    KTEST_CHECK(!probe_read((void*)RAW_KERNEL, &byte));
    pmm_free(big, 1024);
    kprintf("  2 MiB page: one huge leaf maps an aligned 2 MiB kernel range\n");

    // --- kernel stack with a guard page ---
    Result<vaddr_t> ks = vmm_alloc_kernel_stack(16 * KIB);
    KTEST_CHECK(ks.ok());
    *(volatile u64*)(ks.value() - 8) = 1;
    const Vma* kguard = k.find_vma(ks.value() - 16 * KIB - 1);
    KTEST_CHECK(kguard && kguard->kind == VmaKind::Guard);
    vmm_free_kernel_stack(ks.value(), 16 * KIB);
    KTEST_CHECK(!k.find_vma(ks.value() - 8));

    // --- teardown: every frame comes back ---
    k.activate();
    child->destroy();
    as->destroy();
    PmmStats pm1 = pmm_stats();
    VmmStats vs1 = vmm_stats();
    KheapStats hs1 = kheap_stats();
    u64 kept = (vs1.table_frames - vs0.table_frames) + (hs1.slab_pages - hs0.slab_pages);
    KTEST_CHECK(vs1.anon_frames == vs0.anon_frames);
    KTEST_CHECK(hs1.live_objects == hs0.live_objects);      // every VMA and address space was freed
    KTEST_CHECK(pm1.used_frames - pm0.used_frames == kept);
    kprintf("  teardown: all user frames freed; %lu frame(s) kept as kernel page tables and heap slabs\n",
            (unsigned long)kept);
    return 0;
}
