// Page tables, VMAs, demand paging and copy-on-write. See vmm.h for the
// rules. Layout of this file: page-table primitives, frame ownership, raw
// AddressSpace operations, VMA operations, fault handling, clone/destroy,
// and vmm_init, which adopts and hardens the bootloader's tables.
//
// Two software bits live in present leaf entries:
//   PTE_COW    - the frame is shared after clone(); the first write copies it.
//   PTE_NOFREE - the frame is not owned by the VMM (raw map() and device
//                mappings); unmapping must not free it.
// Frames the VMM owns are always mapped with 4 KiB leaves and carry a
// reference count in g_refs (one per address space that maps them).
//
// Locking (lib/lock_order.h): each AddressSpace has a spinlock covering its
// page tables and VMA list. The reference counts are shared between spaces
// and have their own lock, taken inside a space lock. A change that removes
// or restricts a translation is followed, before the space lock is released
// and before any frame is given back, by a TLB shootdown to the other CPUs
// that may have cached it.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpuid.h>
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <boot/bootinfo.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <mm/early_map.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/probe.h>
#include <mm/usercopy.h>
#include <mm/vmm.h>
#include <proc/process.h>

extern "C" {
extern char __kernel_start[], __kernel_end[];
extern char __text_start[], __text_end[];
extern char __data_start[], __data_end[];
}

namespace {

constexpr u64 PTE_P = 1ull << 0;
constexpr u64 PTE_W = 1ull << 1;
constexpr u64 PTE_U = 1ull << 2;
constexpr u64 PTE_PWT = 1ull << 3;
constexpr u64 PTE_PCD = 1ull << 4;
constexpr u64 PTE_A = 1ull << 5;
constexpr u64 PTE_D = 1ull << 6;
constexpr u64 PTE_HUGE = 1ull << 7;         // PS in levels 2-3; the PAT bit in a 4 KiB leaf
constexpr u64 PTE_G = 1ull << 8;
constexpr u64 PTE_COW = 1ull << 9;
constexpr u64 PTE_NOFREE = 1ull << 10;
constexpr u64 PTE_PAT_HUGE = 1ull << 12;    // the PAT bit in a 2 MiB / 1 GiB leaf
constexpr u64 PTE_NX = 1ull << 63;

constexpr u64 ADDR_4K = 0x000FFFFFFFFFF000ull;
constexpr u64 ADDR_2M = 0x000FFFFFFFE00000ull;
constexpr u64 ADDR_1G = 0x000FFFFFC0000000ull;
constexpr u64 SIZE_2M = 2 * MIB;

constexpr u64 CR0_WP = 1ull << 16;
constexpr u64 CR4_PGE = 1ull << 7;
constexpr u64 CR4_LA57 = 1ull << 12;
constexpr u64 EFER_NXE = 1ull << 11;

constexpr u64 PF_PRESENT = 1 << 0;
constexpr u64 PF_WRITE = 1 << 1;
constexpr u64 PF_USER = 1 << 2;
constexpr u64 PF_RESERVED = 1 << 3;
constexpr u64 PF_FETCH = 1 << 4;

AddressSpace g_kernel_space;
VmmStats g_stats;               // updated with atomic adds: no one lock covers every space
u32* g_refs = nullptr;          // per-frame count of address spaces mapping an owned frame
u64 g_ref_frames = 0;
Spinlock g_frames_lock = SPINLOCK_RANKED(lock_rank::VMM_FRAMES);   // protects g_refs
bool g_need_flush = false;      // a huge leaf of the kernel half was split (kernel-space lock held):
                                // every CPU must flush its whole TLB

inline void stat_add(u64& field, i64 n) { __atomic_fetch_add(&field, (u64)n, __ATOMIC_RELAXED); }

// The address space loaded on this CPU. Interrupts must be off.
inline AddressSpace* current_space() {
    AddressSpace* as = percpu()->space;
    return as ? as : &g_kernel_space;
}


// ------------------------------------------------------- table primitives --
inline u64 level_size(int level) { return 1ull << (12 + 9 * (level - 1)); }
inline usize index_of(vaddr_t v, int level) { return (v >> (12 + 9 * (level - 1))) & 511; }
inline u64* table_at(paddr_t p) { return (u64*)hhdm_virt(p); }
inline u64 leaf_addr_mask(int level) { return level == 1 ? ADDR_4K : level == 2 ? ADDR_2M : ADDR_1G; }

paddr_t alloc_table() {
    paddr_t p = pmm_alloc_zeroed(1);
    if (p != PMM_NO_MEMORY) stat_add(g_stats.table_frames, 1);
    return p;
}

void free_table(paddr_t p) {
    pmm_free(p, 1);
    stat_add(g_stats.table_frames, -1);
}

void flush_all() {
    u64 cr4 = read_cr4();
    if (cr4 & CR4_PGE) {        // global entries survive a CR3 reload
        write_cr4(cr4 & ~CR4_PGE);
        write_cr4(cr4);
    } else {
        write_cr3(read_cr3());
    }
    g_need_flush = false;
}

// This CPU's whole TLB, and everyone else's.
void flush_everywhere() {
    flush_all();
    tlb_shootdown(nullptr, 0, 0);
}

// This CPU's TLB entry for v; the caller follows up with tlb_shootdown.
inline void flush_page(const AddressSpace* as, vaddr_t v) {
    if (as->is_kernel() || as == current_space()) invlpg(v);
}

struct Slot {
    u64* entry;
    int level;
};

// Walks to the deepest existing entry for v: a present leaf (level 1, or a
// huge leaf at level 2-3), or the first non-present entry on the way down.
Slot lookup(paddr_t root, vaddr_t v) {
    u64* t = table_at(root);
    for (int level = 4;; level--) {
        u64* e = &t[index_of(v, level)];
        if (level == 1 || !(*e & PTE_P) || (*e & PTE_HUGE)) return {e, level};
        t = table_at(*e & ADDR_4K);
    }
}

// Replaces the huge leaf *e (level 2 or 3) with a table of 512 smaller
// leaves covering the same range with the same attributes.
bool split_huge(u64* e, int level) {
    paddr_t t = alloc_table();
    if (t == PMM_NO_MEMORY) return false;
    u64 old = *e;
    u64 base = old & leaf_addr_mask(level);
    u64 flags = old & ~leaf_addr_mask(level);
    if (level == 2) {
        // 2 MiB -> 4 KiB: PS goes away and the PAT bit moves from bit 12 to bit 7.
        bool pat = flags & PTE_PAT_HUGE;
        flags &= ~(PTE_HUGE | PTE_PAT_HUGE);
        if (pat) flags |= PTE_HUGE;
    }
    u64* nt = table_at(t);
    u64 child = level_size(level - 1);
    for (usize i = 0; i < 512; i++) nt[i] = (base + i * child) | flags;
    *e = t | PTE_P | PTE_W | (old & PTE_U);
    g_need_flush = true;
    return true;
}

// Returns the entry for v at `target` level (1 or 2), creating tables and
// splitting larger leaves on the way. nullptr means out of memory.
u64* ensure(paddr_t root, vaddr_t v, int target, bool user) {
    u64* t = table_at(root);
    for (int level = 4; level > target; level--) {
        u64* e = &t[index_of(v, level)];
        if (!(*e & PTE_P)) {
            paddr_t n = alloc_table();
            if (n == PMM_NO_MEMORY) return nullptr;
            *e = n | PTE_P | PTE_W | (user ? PTE_U : 0);
        } else if (*e & PTE_HUGE) {
            if (!split_huge(e, level)) return nullptr;
        }
        t = table_at(*e & ADDR_4K);
    }
    return &t[index_of(v, target)];
}

u64 leaf_flags(u32 f) {
    u64 e = PTE_P;
    if (f & vm::WRITE) e |= PTE_W;
    if (f & vm::USER) e |= PTE_U;
    if (!(f & vm::EXEC)) e |= PTE_NX;
    if (f & vm::WRITE_THROUGH) e |= PTE_PWT;
    if (f & vm::CACHE_DISABLE) e |= PTE_PCD;
    if (f & vm::GLOBAL) e |= PTE_G;
    return e;
}

bool range_valid(vaddr_t virt, usize size, u32 flags, bool kernel_space) {
    if (!size || ((virt | size) & (PAGE_SIZE - 1))) return false;
    vaddr_t end = virt + size;
    if (end < virt) return false;
    if (flags & ~vm::ALL) return false;
    if ((flags & vm::WRITE) && (flags & vm::EXEC)) return false;      // W^X
    if (flags & vm::USER) return !kernel_space && virt >= USER_MIN && end <= USER_MAX;
    return kernel_space && virt >= KERNEL_HALF;
}

// -------------------------------------------------------- frame ownership --
inline u32& ref_of(paddr_t p) {
    u64 f = p / PAGE_SIZE;
    ASSERT_ALWAYS(f < g_ref_frames);
    return g_refs[f];
}

// A zeroed frame owned by the VMM, with one reference.
paddr_t anon_alloc() {
    paddr_t p = pmm_alloc_zeroed(1);
    if (p == PMM_NO_MEMORY) return p;
    ref_of(p) = 1;              // nobody else can see the frame yet
    stat_add(g_stats.anon_frames, 1);
    return p;
}

void anon_release(paddr_t p) {
    g_frames_lock.lock();
    u32& r = ref_of(p);
    ASSERT_ALWAYS(r > 0);
    bool last = --r == 0;
    g_frames_lock.unlock();
    if (last) {
        pmm_free(p, 1);
        stat_add(g_stats.anon_frames, -1);
    }
}

// Clears every mapping in [virt, virt + size), freeing frames the VMM owns.
// Fails only if a huge leaf must be split and no table frame is available.
// The space lock is held. Frames are given back only after the other CPUs
// have dropped their translations: a frame that is free may be handed to
// someone else at once, and a stale TLB entry would then reach into it.
Result<void> clear_range(AddressSpace* as, vaddr_t virt, usize size) {
    constexpr usize BATCH = 64;
    paddr_t frames[BATCH];
    usize pending = 0;
    bool dirty = false;                 // entries cleared since the last shootdown
    vaddr_t settled = virt;             // other CPUs have been told about everything below this
    auto settle = [&](vaddr_t upto) {
        if (dirty) tlb_shootdown(as, settled, upto - settled);
        for (usize i = 0; i < pending; i++) anon_release(frames[i]);
        pending = 0;
        dirty = false;
        settled = upto;
    };

    vaddr_t end = virt + size;
    vaddr_t v = virt;
    while (v < end) {
        Slot s = lookup(as->root(), v);
        u64 span = level_size(s.level);
        vaddr_t slot_start = align_down(v, span);
        vaddr_t slot_end = slot_start + span;
        if (!(*s.entry & PTE_P)) {
            if (slot_end <= v) break;       // wrapped past the top of the address space
            v = slot_end;
            continue;
        }
        if (s.level > 1 && (slot_start < v || slot_end > end || slot_end <= slot_start)) {
            if (!split_huge(s.entry, s.level)) {
                settle(v);
                return Error::NoMemory;
            }
            continue;
        }
        u64 e = *s.entry;
        *s.entry = 0;
        dirty = true;
        flush_page(as, v);
        if (!(e & PTE_NOFREE)) {
            ASSERT_ALWAYS(s.level == 1);
            frames[pending++] = e & ADDR_4K;
            if (pending == BATCH) settle(slot_end);
        }
        if (slot_end <= v) break;
        v = slot_end;
    }
    settle(end);
    if (g_need_flush) flush_everywhere();
    return {};
}

// Frees a page-table page of the user half and everything below it.
void free_user_table(paddr_t table, int level) {
    u64* t = table_at(table);
    for (usize i = 0; i < 512; i++) {
        u64 e = t[i];
        if (!(e & PTE_P)) continue;
        if (level == 1) {
            if (!(e & PTE_NOFREE)) anon_release(e & ADDR_4K);
        } else {
            ASSERT_ALWAYS(!(e & PTE_HUGE));     // user mappings are always 4 KiB
            free_user_table(e & ADDR_4K, level - 1);
        }
    }
    free_table(table);
}

// ------------------------------------------------------------ page faults --
void page_fault(InterruptFrame* f, void*) {
    vaddr_t addr = read_cr2();
    bool from_user = f->cs & 3;
    bool user_addr = addr < KERNEL_HALF;
    if (from_user) {
        // A user program touched memory it has not been given yet (demand
        // paging, copy-on-write) or is not allowed to touch at all.
        if (user_addr && current_space()->handle_fault(addr, f->error)) return;
        user_exception(f);
    }
    if (user_addr) {
        // Ring 0 may touch user memory only inside the user-copy routines;
        // only there is a fault resolved, or turned into an error return.
        // Anywhere else it is a kernel bug (and SMAP makes the CPU say so).
        if (usercopy_is_access(f->rip)) {
            if (current_space()->handle_fault(addr, f->error)) return;
            if (usercopy_fixup(f, addr, f->error)) return;
        }
    } else if (g_kernel_space.handle_fault(addr, f->error)) {
        return;
    }
    if (probe_fixup(f, addr, f->error)) return;
    exception_fatal(f);
}

// ------------------------------------------------------- boot-time harden --
struct HardenCounts {
    u64 leaves_1g, leaves_2m, leaves_4k;
};

inline vaddr_t canonical(u64 v) { return (v & (1ull << 47)) ? (v | 0xFFFF000000000000ull) : v; }

// Sets NX on every leaf below `table` that lies outside the kernel image.
// Huge leaves that overlap the image are split so the image can be mapped
// by section afterwards.
void harden_table(u64* table, int level, vaddr_t base, usize first, HardenCounts& c) {
    vaddr_t kstart = (vaddr_t)__kernel_start, kend = align_up((vaddr_t)__kernel_end, PAGE_SIZE);
    for (usize i = first; i < 512; i++) {
        u64* e = &table[i];
        if (!(*e & PTE_P)) continue;
        u64 span = level_size(level);
        vaddr_t va = canonical(base + i * span);
        bool leaf = level == 1 || (*e & PTE_HUGE);
        if (leaf) {
            bool in_image = va < kend && va + span > kstart;
            if (in_image && level > 1) {
                if (!split_huge(e, level)) PANIC("vmm: out of memory splitting the kernel image mapping");
                leaf = false;
            } else if (in_image) {
                continue;           // set by section in vmm_init
            } else {
                *e |= PTE_NX;
                if (level == 3) c.leaves_1g++;
                else if (level == 2) c.leaves_2m++;
                else c.leaves_4k++;
                continue;
            }
        }
        harden_table(table_at(*e & ADDR_4K), level - 1, va, 0, c);
    }
}

} // namespace

// ============================================================ AddressSpace ==

Result<AddressSpace*> AddressSpace::create() {
    AddressSpace* as = (AddressSpace*)kzalloc(sizeof(AddressSpace));
    paddr_t root = as ? alloc_table() : PMM_NO_MEMORY;
    if (root == PMM_NO_MEMORY) {
        if (as) kfree(as);
        return Error::NoMemory;
    }
    // The kernel half is shared: every upper PML4 slot was populated at
    // vmm_init, so copying the 256 entries once is enough forever.
    memcpy(table_at(root) + 256, table_at(g_kernel_space.root_) + 256, 256 * sizeof(u64));
    as->root_ = root;
    as->vmas_ = nullptr;
    as->kernel_ = false;
    as->lock_.rank = lock_rank::VMM_USER;
    return as;
}

void AddressSpace::destroy() {
    // Nobody else refers to the space any more, so no lock is needed; it
    // must not be loaded on any CPU.
    ASSERT_ALWAYS(!kernel_);
    for (u32 cpu = 0; cpu < MAX_CPUS; cpu++) ASSERT_ALWAYS(g_percpu[cpu].space != this);
    u64* pml4 = table_at(root_);
    for (usize i = 0; i < 256; i++)
        if (pml4[i] & PTE_P) free_user_table(pml4[i] & ADDR_4K, 3);
    free_table(root_);
    for (Vma* v = vmas_; v;) {
        Vma* next = v->next;
        kfree(v);
        v = next;
    }
    kfree(this);
}

Result<void> AddressSpace::map(vaddr_t virt, paddr_t phys, usize size, u32 flags) {
    lock_.lock();
    Result<void> r = map_locked(virt, phys, size, flags);
    lock_.unlock();
    return r;
}

Result<void> AddressSpace::map_locked(vaddr_t virt, paddr_t phys, usize size, u32 flags) {
    if (!range_valid(virt, size, flags, kernel_) || (phys & (PAGE_SIZE - 1))) return Error::Invalid;
    u64 lf = leaf_flags(flags) | PTE_NOFREE;
    usize done = 0;
    Error err = Error::None;
    while (done < size) {
        vaddr_t v = virt + done;
        paddr_t p = phys + done;
        Slot s = lookup(root_, v);
        if (*s.entry & PTE_P) {
            err = Error::Exists;
            break;
        }
        // 2 MiB leaf for aligned kernel mappings when no page table is in the way.
        bool huge = kernel_ && s.level >= 2 && !((v | p) & (SIZE_2M - 1)) && size - done >= SIZE_2M;
        u64* e = ensure(root_, v, huge ? 2 : 1, flags & vm::USER);
        if (!e) {
            err = Error::NoMemory;
            break;
        }
        if (huge) {
            *e = p | lf | PTE_HUGE;
            stat_add(g_stats.huge_mappings, 1);
            done += SIZE_2M;
        } else {
            *e = p | lf;
            done += PAGE_SIZE;
        }
    }
    if (err != Error::None && done) (void)clear_range(this, virt, done);
    if (g_need_flush) flush_everywhere();
    if (err != Error::None) return err;
    return {};
}

Result<void> AddressSpace::unmap(vaddr_t virt, usize size) {
    if (!size || ((virt | size) & (PAGE_SIZE - 1)) || virt + size < virt) return Error::Invalid;
    if (kernel_ ? virt < KERNEL_HALF : virt + size > USER_MAX) return Error::Invalid;
    lock_.lock();
    Result<void> r = clear_range(this, virt, size);
    lock_.unlock();
    return r;
}

Result<void> AddressSpace::protect(vaddr_t virt, usize size, u32 flags) {
    lock_.lock();
    Result<void> r = protect_locked(virt, size, flags);
    lock_.unlock();
    return r;
}

Result<void> AddressSpace::protect_locked(vaddr_t virt, usize size, u32 flags) {
    if (!range_valid(virt, size, flags, kernel_)) return Error::Invalid;
    bool changed = false;
    vaddr_t end = virt + size;
    vaddr_t v = virt;
    Error err = Error::None;
    while (v < end) {
        Slot s = lookup(root_, v);
        u64 span = level_size(s.level);
        vaddr_t slot_start = align_down(v, span);
        vaddr_t slot_end = slot_start + span;
        if (*s.entry & PTE_P) {
            if (s.level > 1 && (slot_start < v || slot_end > end)) {
                if (!split_huge(s.entry, s.level)) {
                    err = Error::NoMemory;
                    break;
                }
                continue;
            }
            u64 e = *s.entry;
            // Keep the address, cache attributes and software bits; replace
            // only the permission bits.
            u64 keep = e & (leaf_addr_mask(s.level) | PTE_A | PTE_D | PTE_COW | PTE_NOFREE | PTE_HUGE |
                            PTE_PWT | PTE_PCD | (s.level > 1 ? PTE_PAT_HUGE : 0));
            u64 perm = leaf_flags(flags & (vm::WRITE | vm::USER | vm::EXEC | vm::GLOBAL));
            if (e & PTE_COW) perm &= ~PTE_W;        // still shared: the write fault copies first
            *s.entry = keep | perm;
            flush_page(this, v);
            changed = true;
        }
        if (slot_end <= v) break;
        v = slot_end;
    }
    if (g_need_flush) flush_everywhere();
    else if (changed) tlb_shootdown(this, virt, size);
    if (err != Error::None) return err;
    return {};
}

Result<paddr_t> AddressSpace::translate(vaddr_t virt) const {
    lock_.lock();
    Slot s = lookup(root_, virt);
    u64 e = *s.entry;
    lock_.unlock();
    if (!(e & PTE_P)) return Error::NotFound;
    return (paddr_t)((e & leaf_addr_mask(s.level)) + (virt & (level_size(s.level) - 1)));
}

// -------------------------------------------------------------------- VMAs --

const Vma* AddressSpace::find_vma(vaddr_t addr) const {
    for (const Vma* v = vmas_; v && v->start <= addr; v = v->next)
        if (addr < v->end) return v;
    return nullptr;
}

Vma* AddressSpace::find_vma_mut(vaddr_t addr) { return const_cast<Vma*>(find_vma(addr)); }

usize AddressSpace::vma_count() const {
    usize n = 0;
    for (const Vma* v = vmas_; v; v = v->next) n++;
    return n;
}

bool AddressSpace::range_free(vaddr_t start, vaddr_t end) const {
    for (const Vma* v = vmas_; v && v->start < end; v = v->next)
        if (v->end > start) return false;
    return true;
}

Result<void> AddressSpace::insert_vma(vaddr_t start, vaddr_t end, u32 prot, VmaKind kind, paddr_t phys) {
    Vma* n = (Vma*)kzalloc(sizeof(Vma));
    if (!n) return Error::NoMemory;
    n->start = start;
    n->end = end;
    n->prot = prot;
    n->kind = kind;
    n->phys = phys;
    Vma** link = &vmas_;
    while (*link && (*link)->start < start) link = &(*link)->next;
    n->next = *link;
    *link = n;
    return {};
}

// Splits the VMA containing addr into two at addr; no-op if addr is already
// a boundary or lies in no VMA.
Result<void> AddressSpace::split_vma_at(vaddr_t addr) {
    Vma* v = find_vma_mut(addr);
    if (!v || v->start == addr) return {};
    Vma* n = (Vma*)kzalloc(sizeof(Vma));
    if (!n) return Error::NoMemory;
    *n = *v;
    n->start = addr;
    if (v->kind == VmaKind::Device) n->phys = v->phys + (addr - v->start);
    v->end = addr;
    n->next = v->next;
    v->next = n;
    return {};
}

// First-fit search for `len` free bytes, starting at hint (or the space's
// default base) and wrapping once to the default base.
Result<vaddr_t> AddressSpace::find_gap(vaddr_t hint, usize len) const {
    vaddr_t base = kernel_ ? KERNEL_VM_BASE : USER_MMAP_BASE;
    vaddr_t starts[2] = {hint ? align_up(hint, PAGE_SIZE) : base, base};
    for (vaddr_t start : starts) {
        if (start < low_limit() || start >= high_limit()) continue;
        vaddr_t candidate = start;
        for (const Vma* v = vmas_; v; v = v->next) {
            if (v->end <= candidate) continue;
            if (v->start >= candidate && v->start - candidate >= len) break;
            candidate = v->end;
        }
        if (candidate <= high_limit() && high_limit() - candidate >= len) return candidate;
    }
    return Error::NoSpace;
}

// Unmaps the pages of [start, end) and drops every VMA inside it. The range
// boundaries must already be VMA boundaries (split_vma_at).
void AddressSpace::release_range(vaddr_t start, vaddr_t end) {
    Vma** link = &vmas_;
    while (*link && (*link)->start < end) {
        Vma* v = *link;
        if (v->start >= start && v->end <= end) {
            *link = v->next;
            kfree(v);
        } else {
            link = &v->next;
        }
    }
}

Result<void> AddressSpace::populate(vaddr_t start, vaddr_t end, u32 prot) {
    for (vaddr_t v = start; v < end; v += PAGE_SIZE) {
        Slot s = lookup(root_, v);
        if (*s.entry & PTE_P) continue;
        paddr_t f = anon_alloc();
        if (f == PMM_NO_MEMORY) return Error::NoMemory;
        u64* e = ensure(root_, v, 1, prot & vm::USER);
        if (!e) {
            anon_release(f);
            return Error::NoMemory;
        }
        *e = f | leaf_flags(prot);
    }
    return {};
}

Result<vaddr_t> AddressSpace::mmap(vaddr_t hint, usize len, u32 prot, u32 flags) {
    if (!len || (flags & ~mmap_flag::ALL) || (prot & ~(vm::WRITE | vm::EXEC))) return Error::Invalid;
    if ((prot & vm::WRITE) && (prot & vm::EXEC)) return Error::Invalid;          // W^X
    usize size = align_up(len, PAGE_SIZE);
    usize guard = (flags & mmap_flag::GUARD_BELOW) ? PAGE_SIZE : 0;
    usize total = size + guard;
    if (size < len || total < size) return Error::Invalid;
    if (!kernel_) prot |= vm::USER;

    lock_.lock();
    vaddr_t base;
    if (flags & mmap_flag::FIXED) {
        vaddr_t end = hint + size;
        if ((hint & (PAGE_SIZE - 1)) || hint < guard || end < hint || hint - guard < low_limit() ||
            end > high_limit()) {
            lock_.unlock();
            return Error::Invalid;
        }
        base = hint - guard;
        if (!range_free(base, end)) {
            lock_.unlock();
            return Error::Exists;
        }
    } else {
        Result<vaddr_t> gap = find_gap(hint, total);
        if (!gap.ok()) {
            lock_.unlock();
            return gap.error();
        }
        base = gap.value();
    }

    Error err = Error::None;
    if (guard) err = insert_vma(base, base + guard, 0, VmaKind::Guard, 0).error();
    if (err == Error::None) err = insert_vma(base + guard, base + total, prot, VmaKind::Anonymous, 0).error();
    // Kernel memory is never demand-paged: a fault in an interrupt handler
    // must not depend on the allocator.
    if (err == Error::None && (kernel_ || (flags & mmap_flag::POPULATE)))
        err = populate(base + guard, base + total, prot).error();
    if (err != Error::None) {
        (void)clear_range(this, base, total);
        release_range(base, base + total);
        lock_.unlock();
        return err;
    }
    if (g_need_flush) flush_everywhere();
    lock_.unlock();
    return base + guard;
}

Result<vaddr_t> AddressSpace::mmap_device(paddr_t phys, usize len, u32 prot) {
    if (!len || (prot & ~(vm::WRITE | vm::WRITE_THROUGH | vm::CACHE_DISABLE))) return Error::Invalid;
    paddr_t first = align_down(phys, PAGE_SIZE);
    usize offset = phys - first;
    usize size = align_up(len + offset, PAGE_SIZE);
    if (len + offset < len || size < len || first + size < first) return Error::Invalid;
    if (!kernel_) prot |= vm::USER;

    lock_.lock();
    Result<vaddr_t> gap = find_gap(0, size);
    if (!gap.ok()) {
        lock_.unlock();
        return gap.error();
    }
    vaddr_t base = gap.value();
    Error err = insert_vma(base, base + size, prot, VmaKind::Device, first).error();
    if (err == Error::None) {
        err = map_locked(base, first, size, prot).error();
        if (err != Error::None) release_range(base, base + size);
    }
    lock_.unlock();
    if (err != Error::None) return err;
    return base + offset;
}

Result<void> AddressSpace::munmap(vaddr_t addr, usize len) {
    usize size = align_up(len, PAGE_SIZE);
    vaddr_t end = addr + size;
    if (!len || size < len || (addr & (PAGE_SIZE - 1)) || end < addr || addr < low_limit() || end > high_limit())
        return Error::Invalid;
    lock_.lock();
    Error err = split_vma_at(addr).error();
    if (err == Error::None) err = split_vma_at(end).error();
    if (err == Error::None) err = clear_range(this, addr, size).error();
    if (err == Error::None) release_range(addr, end);
    lock_.unlock();
    if (err != Error::None) return err;
    return {};
}

Result<void> AddressSpace::mprotect(vaddr_t addr, usize len, u32 prot) {
    usize size = align_up(len, PAGE_SIZE);
    vaddr_t end = addr + size;
    if (!len || size < len || (addr & (PAGE_SIZE - 1)) || end < addr || (prot & ~(vm::WRITE | vm::EXEC)))
        return Error::Invalid;
    if ((prot & vm::WRITE) && (prot & vm::EXEC)) return Error::Invalid;          // W^X

    lock_.lock();
    // The whole range must be backed by mappable VMAs, with no holes.
    Error err = Error::None;
    for (vaddr_t a = addr; a < end;) {
        const Vma* v = find_vma(a);
        if (!v) {
            err = Error::NotFound;
            break;
        }
        if (v->kind == VmaKind::Guard || (v->kind == VmaKind::Device && (prot & vm::EXEC))) {
            err = Error::Perm;
            break;
        }
        a = v->end;
    }
    if (err == Error::None) err = split_vma_at(addr).error();
    if (err == Error::None) err = split_vma_at(end).error();
    if (err == Error::None) {
        for (Vma* v = find_vma_mut(addr); v && v->start < end; v = v->next) {
            v->prot = (v->prot & ~(vm::WRITE | vm::EXEC)) | prot;
            err = protect_locked(v->start, v->end - v->start, v->prot).error();
            if (err != Error::None) break;
        }
    }
    lock_.unlock();
    if (err != Error::None) return err;
    return {};
}

// ------------------------------------------------------------------ faults --

bool AddressSpace::handle_fault(vaddr_t addr, u64 error) {
    if (error & PF_RESERVED) return false;
    u64 irq = interrupts_save();
    // A fault taken while this CPU is itself editing the space is a kernel
    // bug, and waiting for the lock would wait for ever: let it be reported.
    if (lock_.held_by_this_cpu()) {
        interrupts_restore(irq);
        return false;
    }
    lock_.acquire();
    bool resolved = handle_fault_locked(addr, error);
    lock_.release();
    interrupts_restore(irq);
    return resolved;
}

bool AddressSpace::handle_fault_locked(vaddr_t addr, u64 error) {
    const Vma* v = find_vma(addr);
    if (!v || v->kind != VmaKind::Anonymous) return false;
    if ((error & PF_USER) && !(v->prot & vm::USER)) return false;
    if ((error & PF_WRITE) && !(v->prot & vm::WRITE)) return false;
    if ((error & PF_FETCH) && !(v->prot & vm::EXEC)) return false;

    vaddr_t page = align_down(addr, PAGE_SIZE);
    Slot s = lookup(root_, page);

    if (!(error & PF_PRESENT)) {
        if (*s.entry & PTE_P) {             // another CPU resolved it first, or the TLB entry was stale
            invlpg(page);
            return true;
        }
        paddr_t f = anon_alloc();
        if (f == PMM_NO_MEMORY) return false;
        u64* e = ensure(root_, page, 1, v->prot & vm::USER);
        if (!e) {
            anon_release(f);
            return false;
        }
        *e = f | leaf_flags(v->prot);
        stat_add(g_stats.demand_faults, 1);
        return true;
    }

    // The CPU found a present page it was not allowed to use that way.
    if (s.level != 1) return false;
    u64 e = *s.entry;
    if (!(e & PTE_P)) {                     // unmapped by another CPU since: fault again as not-present
        invlpg(page);
        return true;
    }
    if (!(e & PTE_COW)) {
        // Not shared. If the page tables now allow the access, another CPU
        // has already resolved this fault (or raised the permissions) and
        // this CPU's TLB entry was out of date.
        bool allowed = (!(error & PF_WRITE) || (e & PTE_W)) && (!(error & PF_FETCH) || !(e & PTE_NX)) &&
                       (!(error & PF_USER) || (e & PTE_U));
        if (allowed) invlpg(page);
        return allowed;
    }
    // The only other resolvable case: the first write to a page shared by
    // clone().
    if (!(error & PF_WRITE)) return false;
    paddr_t old = e & ADDR_4K;
    g_frames_lock.lock();
    bool sole = ref_of(old) == 1;
    g_frames_lock.unlock();
    if (sole) {
        // Last sharer: take the page over. Nobody can gain a reference in
        // the meantime; only clone() of this space could, and it needs lock_.
        *s.entry = (e | PTE_W) & ~PTE_COW;
        invlpg(page);
    } else {
        // Copy first, while this space still holds its reference: as long
        // as the count is above one no other sharer can make the frame
        // writable, so the copy is of a page that is not changing.
        paddr_t n = pmm_alloc(1);
        if (n == PMM_NO_MEMORY) return false;
        memcpy(hhdm_virt(n), hhdm_virt(old), PAGE_SIZE);
        ref_of(n) = 1;
        stat_add(g_stats.anon_frames, 1);
        *s.entry = n | leaf_flags(v->prot);
        invlpg(page);
        tlb_shootdown(this, page, PAGE_SIZE);       // other CPUs in this space still map the old frame
        anon_release(old);                          // frees it if every other sharer has copied too
        stat_add(g_stats.cow_copies, 1);
    }
    stat_add(g_stats.cow_faults, 1);
    return true;
}

// ------------------------------------------------------------------- clone --

Result<AddressSpace*> AddressSpace::clone() {
    if (kernel_) return Error::Invalid;
    Result<AddressSpace*> made = create();
    if (!made.ok()) return made.error();
    AddressSpace* child = made.value();

    // The child is not visible to anyone else yet, so only this space's lock
    // is needed.
    lock_.lock();
    Error err = Error::None;
    for (Vma* v = vmas_; v && err == Error::None; v = v->next) {
        err = child->insert_vma(v->start, v->end, v->prot, v->kind, v->phys).error();
        if (err != Error::None || v->kind == VmaKind::Guard) continue;
        for (vaddr_t a = v->start; a < v->end;) {
            Slot s = lookup(root_, a);
            if (!(*s.entry & PTE_P)) {
                a = align_down(a, level_size(s.level)) + level_size(s.level);
                continue;
            }
            u64 e = *s.entry;
            if (!(e & PTE_NOFREE)) {
                // Owned frame: both sides lose write access and share it
                // until one of them writes.
                e = (e & ~PTE_W) | PTE_COW;
                *s.entry = e;
            }
            u64* ce = ensure(child->root_, a, 1, true);
            if (!ce) {
                err = Error::NoMemory;
                break;
            }
            if (!(e & PTE_NOFREE)) {
                g_frames_lock.lock();
                ref_of(e & ADDR_4K)++;
                g_frames_lock.unlock();
            }
            *ce = e;
            a += PAGE_SIZE;
        }
    }
    // Drop stale writable TLB entries, here and on any other CPU running
    // this space.
    if (this == current_space()) write_cr3(root_);
    tlb_shootdown(this, 0, 0);
    lock_.unlock();
    if (err != Error::None) {
        child->destroy();
        return err;
    }
    return child;
}

void AddressSpace::activate() {
    u64 irq = interrupts_save();
    // Published before it is loaded: tlb_shootdown decides whom to interrupt
    // by this field, and a CPU that loads CR3 afterwards starts clean.
    __atomic_store_n(&percpu()->space, this, __ATOMIC_RELEASE);
    write_cr3(root_);
    interrupts_restore(irq);
}

// ================================================================== module ==

AddressSpace& vmm_kernel() { return g_kernel_space; }

AddressSpace& vmm_current() {
    u64 irq = interrupts_save();
    AddressSpace* as = current_space();
    interrupts_restore(irq);
    return *as;
}

VmmStats vmm_stats() { return g_stats; }

void vmm_flush_local(vaddr_t start, usize size) {
    constexpr usize MAX_SINGLE = 32 * PAGE_SIZE;    // beyond this one full flush is cheaper
    if (!size || size > MAX_SINGLE) {
        flush_all();
        return;
    }
    for (vaddr_t v = align_down(start, PAGE_SIZE); v < start + size; v += PAGE_SIZE) invlpg(v);
}

void vmm_init_cpu() {
    wrmsr(msr::EFER, rdmsr(msr::EFER) | EFER_NXE);
    write_cr0(read_cr0() | CR0_WP);
    // Reloading CR3 drops whatever this CPU cached before the tables were
    // hardened. (Global entries cannot be flushed this way, so PGE is
    // toggled as well.)
    write_cr3(g_kernel_space.root());
    flush_all();
}

Result<vaddr_t> vmm_alloc_kernel_stack(usize size) {
    Result<vaddr_t> r = g_kernel_space.mmap(0, size, vm::WRITE, mmap_flag::GUARD_BELOW);
    if (!r.ok()) return r.error();
    return r.value() + align_up(size, PAGE_SIZE);
}

void vmm_free_kernel_stack(vaddr_t top, usize size) {
    usize bytes = align_up(size, PAGE_SIZE) + PAGE_SIZE;
    Result<void> r = g_kernel_space.munmap(top - bytes, bytes);
    ASSERT_ALWAYS(r.ok());
}

bool vmm_fault_note(vaddr_t addr) {
    const AddressSpace* spaces[2] = {&g_kernel_space, current_space()};
    for (const AddressSpace* as : spaces) {
        const Vma* v = as->find_vma(addr);
        if (!v || v->kind != VmaKind::Guard) continue;
        const Vma* stack = v->next;
        kprintf("%sstack overflow: %#lx is in the guard page below the stack at %#lx-%#lx\n", as->is_kernel() ? "kernel " : "",
                (unsigned long)addr,
                (unsigned long)(stack ? stack->start : v->end), (unsigned long)(stack ? stack->end : v->end));
        return true;
    }
    return false;
}

void vmm_init() {
    if (cpuid_max_ext_leaf() < 0x80000001 || !(cpuid(0x80000001).edx & (1u << 20)))
        PANIC("vmm: this CPU has no NX bit; W^X cannot be enforced");
    if (read_cr4() & CR4_LA57) PANIC("vmm: 5-level paging is not supported");
    wrmsr(msr::EFER, rdmsr(msr::EFER) | EFER_NXE);
    write_cr0(read_cr0() | CR0_WP);         // ring 0 honours read-only pages too
    ASSERT_ALWAYS(read_rsp() >= KERNEL_HALF);

    // The bootloader leaves the other processors spinning in its own code,
    // which the direct map exposes as ordinary memory. They must be running
    // kernel text before that memory loses execute permission below.
    usize parked = boot_park_aps();

    u64 irq = interrupts_save();
    g_kernel_space.root_ = read_cr3() & ADDR_4K;
    g_kernel_space.vmas_ = nullptr;
    g_kernel_space.kernel_ = true;
    g_kernel_space.lock_.rank = lock_rank::VMM_KERNEL;
    percpu()->space = &g_kernel_space;
    u64* pml4 = table_at(g_kernel_space.root_);

    // The kernel lives entirely in the upper half; anything the bootloader
    // left below it is unreachable attack surface.
    usize cleared = 0;
    for (usize i = 0; i < 256; i++) {
        if (pml4[i] & PTE_P) {
            pml4[i] = 0;
            cleared++;
        }
    }
    // Populate every kernel PML4 slot now, so the kernel half of each later
    // address space (a copy of these 256 entries) never goes stale.
    for (usize i = 256; i < 512; i++) {
        if (pml4[i] & PTE_P) continue;
        paddr_t t = alloc_table();
        if (t == PMM_NO_MEMORY) PANIC("vmm: out of memory for kernel page tables");
        pml4[i] = t | PTE_P | PTE_W;
    }

    HardenCounts counts{};
    harden_table(pml4, 4, 0, 256, counts);

    // The kernel image, page by page: text is executable and read-only,
    // data and bss are writable and non-executable, everything else
    // (bootloader requests, rodata, the symbol table) is read-only data.
    vaddr_t kstart = (vaddr_t)__kernel_start, kend = align_up((vaddr_t)__kernel_end, PAGE_SIZE);
    vaddr_t text_start = (vaddr_t)__text_start, text_end = align_up((vaddr_t)__text_end, PAGE_SIZE);
    vaddr_t data_start = (vaddr_t)__data_start, data_end = align_up((vaddr_t)__data_end, PAGE_SIZE);
    u64 text_pages = 0, ro_pages = 0, data_pages = 0;
    for (vaddr_t va = kstart; va < kend; va += PAGE_SIZE) {
        Slot s = lookup(g_kernel_space.root_, va);
        if (!(*s.entry & PTE_P) || s.level != 1) PANIC("vmm: kernel image page %#lx is not mapped", (unsigned long)va);
        u64 e = *s.entry & ~(PTE_W | PTE_U | PTE_NX);
        if (va >= text_start && va < text_end) {
            text_pages++;
        } else if (va >= data_start && va < data_end) {
            e |= PTE_W | PTE_NX;
            data_pages++;
        } else {
            e |= PTE_NX;
            ro_pages++;
        }
        *s.entry = e;
    }
    flush_all();

    g_ref_frames = pmm_stats().total_frames;
    usize ref_pages = align_up(g_ref_frames * sizeof(u32), PAGE_SIZE) / PAGE_SIZE;
    paddr_t refs = pmm_alloc_zeroed(ref_pages);
    if (refs == PMM_NO_MEMORY) PANIC("vmm: out of memory for the frame reference table");
    g_refs = (u32*)hhdm_virt(refs);

    if (!interrupt_register(14, page_fault)) PANIC("vmm: page-fault vector already claimed");
    interrupts_restore(irq);

    kprintf("vmm: kernel space adopted; NX and WP on; %lu lower-half slot(s) cleared; %lu other cpu(s) parked "
            "in kernel text\n",
            (unsigned long)cleared, (unsigned long)parked);
    kprintf("vmm: image %lu pages text (r-x), %lu read-only (r--), %lu data (rw-)\n", (unsigned long)text_pages,
            (unsigned long)ro_pages, (unsigned long)data_pages);
    kprintf("vmm: rest of the kernel half is non-executable: %lu x 1 GiB, %lu x 2 MiB, %lu x 4 KiB leaves\n",
            (unsigned long)counts.leaves_1g, (unsigned long)counts.leaves_2m, (unsigned long)counts.leaves_4k);
}
