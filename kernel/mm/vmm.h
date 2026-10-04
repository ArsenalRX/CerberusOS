// Virtual memory manager: 4-level page tables wrapped in AddressSpace, a
// per-space list of virtual memory areas (VMAs), demand paging, copy-on-write
// cloning, and guard pages (SPEC phase 4 and §5A).
//
// Security rules enforced here (SPEC §19.3):
//   - W^X: no mapping is ever both writable and executable.
//   - Every page is non-executable unless vm::EXEC is asked for.
//   - Anonymous memory is zeroed before it becomes visible.
//   - The first 64 KiB of a user address space can never be mapped.
//   - The kernel image is mapped text RX, read-only data R, data RW+NX.
//
// The kernel half (the upper 256 PML4 slots) is shared by every address
// space; kernel mappings are made through vmm_kernel() only.
//
// Concurrency: every address space has a spinlock that covers its page
// tables and VMA list; every operation takes it. Removing or restricting a
// translation also makes the other CPUs that may have cached it drop it
// (TLB shootdown, arch/x86_64/smp.h) before the operation returns. Nothing
// here sleeps. Only handle_fault and vmm_fault_note are called from
// interrupt context. find_vma and vma_count take no lock: they are for the
// fault path (which holds it) and for tests.
#pragma once

#include <lib/result.h>
#include <lib/types.h>
#include <sched/sync.h>

// Page permissions. Readable is implied; the default is read-only,
// kernel-only, non-executable, cached.
namespace vm {
constexpr u32 WRITE = 1 << 0;
constexpr u32 USER = 1 << 1;
constexpr u32 EXEC = 1 << 2;
constexpr u32 WRITE_THROUGH = 1 << 3;
constexpr u32 CACHE_DISABLE = 1 << 4;
constexpr u32 GLOBAL = 1 << 5;
constexpr u32 ALL = WRITE | USER | EXEC | WRITE_THROUGH | CACHE_DISABLE | GLOBAL;
} // namespace vm

namespace mmap_flag {
constexpr u32 FIXED = 1 << 0;        // use `hint` exactly or fail; never replaces a mapping
constexpr u32 POPULATE = 1 << 1;     // allocate every page now instead of on first touch
constexpr u32 GUARD_BELOW = 1 << 2;  // reserve an unmapped guard page under the region (stacks)
constexpr u32 ALL = FIXED | POPULATE | GUARD_BELOW;
} // namespace mmap_flag

constexpr vaddr_t USER_MIN = 0x10000;                    // null guard: first 64 KiB
constexpr vaddr_t USER_MAX = 0x0000800000000000ull;      // exclusive
constexpr vaddr_t USER_MMAP_BASE = 0x0000100000000000ull;
constexpr vaddr_t KERNEL_HALF = 0xFFFF800000000000ull;
// Kernel virtual allocations (SPEC §6.1 "kernel heap / vmalloc"). The first
// GiB of that region belongs to early_map.
constexpr vaddr_t KERNEL_VM_BASE = 0xFFFFC00040000000ull;
constexpr vaddr_t KERNEL_VM_END = 0xFFFFFFFF80000000ull;

enum class VmaKind : u8 {
    Anonymous,  // zero-filled memory, allocated on first touch
    Device,     // a fixed physical range (MMIO, framebuffer); never freed
    Guard,      // reserved and never mapped; touching it is a stack overflow
};

struct Vma {
    vaddr_t start, end;     // page-aligned, end exclusive
    u32 prot;               // vm:: flags
    VmaKind kind;
    paddr_t phys;           // Device: physical address of `start`
    Vma* next;              // sorted by start, never overlapping
};

struct VmmStats {
    u64 table_frames;       // page-table pages allocated by the VMM
    u64 anon_frames;        // frames backing anonymous memory
    u64 demand_faults;      // not-present faults resolved by allocating a page
    u64 cow_faults;         // write faults resolved by copy-on-write
    u64 cow_copies;         // of those, how many had to copy the page
    u64 huge_mappings;      // 2 MiB leaves created by map()
};

class AddressSpace {
public:
    // A new user address space with an empty lower half and the shared
    // kernel half. Error::NoMemory if no frame is available.
    static Result<AddressSpace*> create();
    // Frees every user page, page table and VMA. The space must not be the
    // active one. The kernel space cannot be destroyed.
    void destroy();

    // --- raw page-table operations (no VMA is created or consulted) ---
    // The caller owns `phys`; unmap() does not free it. All addresses and
    // sizes must be page-aligned. Lower-half addresses require vm::USER and
    // upper-half addresses forbid it.
    // Errors: Invalid (alignment, range, W+X, null guard), Exists (a page in
    // the range is already mapped), NoMemory (page tables).
    Result<void> map(vaddr_t virt, paddr_t phys, usize size, u32 flags);
    // Removes mappings; pages that are not mapped are skipped.
    Result<void> unmap(vaddr_t virt, usize size);
    // Changes permissions of the mapped pages in the range. Invalid on W+X.
    Result<void> protect(vaddr_t virt, usize size, u32 flags);
    // Physical address behind a virtual one. NotFound if unmapped.
    Result<paddr_t> translate(vaddr_t virt) const;

    // --- VMA-level operations (what mmap/munmap/mprotect syscalls use) ---
    // Reserves `len` bytes (rounded up to pages) of zero-filled memory with
    // permissions `prot` (vm::WRITE / vm::EXEC; USER is implied by the kind
    // of space). Pages appear on first touch unless POPULATE is given; the
    // kernel space always populates. Returns the start address.
    // Errors: Invalid, Exists (FIXED range in use), NoSpace, NoMemory.
    Result<vaddr_t> mmap(vaddr_t hint, usize len, u32 prot, u32 flags);
    // Maps the physical range [phys, phys+len) at a free address, eagerly.
    Result<vaddr_t> mmap_device(paddr_t phys, usize len, u32 prot);
    // Removes [addr, addr+len) from the space, splitting VMAs as needed and
    // freeing anonymous pages no other space still shares.
    Result<void> munmap(vaddr_t addr, usize len);
    // Changes permissions of [addr, addr+len); the whole range must be
    // covered by Anonymous or Device VMAs. Invalid on W+X.
    Result<void> mprotect(vaddr_t addr, usize len, u32 prot);

    // A copy for fork(): same VMAs; anonymous pages are shared read-only and
    // copied on the first write by either side.
    Result<AddressSpace*> clone();

    // Loads this space into CR3 on the calling CPU and makes it the one
    // faults there resolve against. The scheduler reloads it whenever the
    // calling thread is switched back in, on whichever CPU.
    void activate();

    // Page-fault entry: returns true if the fault was resolved (demand page
    // or copy-on-write) and the instruction can be retried. Interrupt-safe.
    bool handle_fault(vaddr_t addr, u64 error_code);

    const Vma* find_vma(vaddr_t addr) const;
    bool is_kernel() const { return kernel_; }
    paddr_t root() const { return root_; }
    usize vma_count() const;

private:
    friend void vmm_init();

    Result<void> map_locked(vaddr_t virt, paddr_t phys, usize size, u32 flags);
    Result<void> protect_locked(vaddr_t virt, usize size, u32 flags);
    bool handle_fault_locked(vaddr_t addr, u64 error_code);
    Vma* find_vma_mut(vaddr_t addr);
    Result<void> insert_vma(vaddr_t start, vaddr_t end, u32 prot, VmaKind kind, paddr_t phys);
    Result<void> split_vma_at(vaddr_t addr);
    Result<vaddr_t> find_gap(vaddr_t hint, usize len) const;
    bool range_free(vaddr_t start, vaddr_t end) const;
    void release_range(vaddr_t start, vaddr_t end);
    Result<void> populate(vaddr_t start, vaddr_t end, u32 prot);
    vaddr_t low_limit() const { return kernel_ ? KERNEL_VM_BASE : USER_MIN; }
    vaddr_t high_limit() const { return kernel_ ? KERNEL_VM_END : USER_MAX; }

    paddr_t root_;      // physical address of the PML4
    Vma* vmas_;
    bool kernel_;
    mutable Spinlock lock_;     // page tables and VMA list; rank VMM_USER or VMM_KERNEL
};

// Adopts the bootloader's page tables as the kernel address space, hardens
// them (NX everywhere outside kernel text, kernel image by section, lower
// half emptied, CR0.WP and EFER.NXE on) and installs the page-fault handler.
// Requires pmm_init and interrupts_init. Panics if the CPU lacks NX.
void vmm_init();

// The same control-register settings on another CPU, and the kernel space
// loaded afresh. First thing that CPU does; needs no per-CPU data.
void vmm_init_cpu();

AddressSpace& vmm_kernel();
// The address space loaded on the calling CPU, which is the calling
// thread's own.
AddressSpace& vmm_current();
// Drops this CPU's cached translations for [start, start + size); size 0
// means all of them. Used by TLB shootdown. Interrupts must be off.
void vmm_flush_local(vaddr_t start, usize size);

// A kernel stack of `size` bytes with a guard page below it. Returns the
// address just past the top (the initial stack pointer).
Result<vaddr_t> vmm_alloc_kernel_stack(usize size);
// Frees a stack returned by vmm_alloc_kernel_stack, guard page included.
void vmm_free_kernel_stack(vaddr_t top, usize size);

VmmStats vmm_stats();

// If `addr` lies in a guard page, prints one line naming the stack it
// protects and returns true. Called from the fatal exception path for #PF
// and #DF. Interrupt-safe.
bool vmm_fault_note(vaddr_t addr);
