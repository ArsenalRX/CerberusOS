// Bitmap allocator. Bit set = frame in use (allocated or reserved). A
// rolling "next free" hint keeps single-frame allocation O(1) in the common
// case; multi-frame allocations scan from the start (first fit, as the spec
// asks) so they reuse low fragments before spreading out.
#include <arch/x86_64/cpu.h>
#include <boot/bootinfo.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <mm/early_map.h>
#include <mm/pmm.h>

namespace {

u64* g_bitmap = nullptr;
u64 g_frames = 0;           // bitmap length in frames
u64 g_words = 0;
u64 g_usable = 0;
u64 g_used = 0;
u64 g_hint = 0;             // frame index to start single-frame searches from

inline bool test(u64 f) { return g_bitmap[f / 64] & (1ull << (f % 64)); }
inline void set(u64 f) { g_bitmap[f / 64] |= 1ull << (f % 64); }
inline void clear(u64 f) { g_bitmap[f / 64] &= ~(1ull << (f % 64)); }

void mark_range(paddr_t base, u64 length, bool used) {
    u64 first = align_up(base, PAGE_SIZE) / PAGE_SIZE;
    u64 last = align_down(base + length, PAGE_SIZE) / PAGE_SIZE;   // exclusive
    if (used) {
        first = align_down(base, PAGE_SIZE) / PAGE_SIZE;
        last = align_up(base + length, PAGE_SIZE) / PAGE_SIZE;
    }
    for (u64 f = first; f < last && f < g_frames; f++) {
        if (used) set(f);
        else clear(f);
    }
}

u64 find_run(usize count, u64 start) {
    u64 run = 0;
    for (u64 f = start; f < g_frames; f++) {
        if (test(f)) {
            run = 0;
            continue;
        }
        if (++run == count) return f + 1 - count;
    }
    return (u64)-1;
}

} // namespace

void pmm_init() {
    const BootInfo& bi = g_boot_info;
    u64 top = bi.total_bytes();
    g_frames = align_up(top, PAGE_SIZE) / PAGE_SIZE;
    g_words = (g_frames + 63) / 64;
    u64 bitmap_bytes = g_words * 8;

    // Find a usable region big enough for the bitmap (skip the first MiB:
    // legacy areas are better left alone).
    paddr_t bitmap_phys = 0;
    for (usize i = 0; i < bi.region_count; i++) {
        const MemoryRegion& r = bi.regions[i];
        if (r.type != MemoryType::Usable) continue;
        paddr_t base = max<paddr_t>(align_up(r.base, PAGE_SIZE), MIB);
        paddr_t end = r.base + r.length;
        if (base < end && end - base >= bitmap_bytes) {
            bitmap_phys = base;
            break;
        }
    }
    if (!bitmap_phys) PANIC("pmm: no usable region can hold a %lu-byte bitmap", (unsigned long)bitmap_bytes);
    g_bitmap = (u64*)hhdm_virt(bitmap_phys);

    // Everything reserved, then usable regions freed, then the bitmap reserved.
    memset(g_bitmap, 0xFF, bitmap_bytes);
    for (usize i = 0; i < bi.region_count; i++)
        if (bi.regions[i].type == MemoryType::Usable) mark_range(bi.regions[i].base, bi.regions[i].length, false);
    mark_range(0, PAGE_SIZE, true);                     // never hand out frame 0
    mark_range(bitmap_phys, bitmap_bytes, true);

    g_usable = 0;
    for (u64 f = 0; f < g_frames; f++)
        if (!test(f)) g_usable++;
    g_used = 0;
    g_hint = 0;

    kprintf("pmm: %lu frames tracked (%lu MiB), %lu usable (%lu MiB), bitmap %lu KiB at %#lx\n",
            (unsigned long)g_frames, (unsigned long)(g_frames * PAGE_SIZE / MIB), (unsigned long)g_usable,
            (unsigned long)(g_usable * PAGE_SIZE / MIB), (unsigned long)(bitmap_bytes / KIB),
            (unsigned long)bitmap_phys);
}

paddr_t pmm_alloc(usize count) {
    if (!count || !g_bitmap) return PMM_NO_MEMORY;
    u64 flags = interrupts_save();
    u64 f = (u64)-1;
    if (count == 1) {
        f = find_run(1, g_hint);
        if (f == (u64)-1) f = find_run(1, 0);
    } else {
        f = find_run(count, 0);
    }
    if (f == (u64)-1) {
        interrupts_restore(flags);
        return PMM_NO_MEMORY;
    }
    for (u64 i = 0; i < count; i++) set(f + i);
    g_used += count;
    if (count == 1) g_hint = f + 1;
    interrupts_restore(flags);
    return f * PAGE_SIZE;
}

paddr_t pmm_alloc_zeroed(usize count) {
    paddr_t p = pmm_alloc(count);
    if (p != PMM_NO_MEMORY) memset(hhdm_virt(p), 0, count * PAGE_SIZE);
    return p;
}

void pmm_free(paddr_t addr, usize count) {
    if (!count) return;
    ASSERT_ALWAYS((addr % PAGE_SIZE) == 0);
    u64 f = addr / PAGE_SIZE;
    ASSERT_ALWAYS(f + count <= g_frames);
    u64 flags = interrupts_save();
    for (u64 i = 0; i < count; i++) {
        ASSERT(test(f + i));            // double free
        clear(f + i);
    }
    g_used -= count;
    if (f < g_hint) g_hint = f;
    interrupts_restore(flags);
}

PmmStats pmm_stats() {
    PmmStats s{};
    u64 flags = interrupts_save();
    s.total_frames = g_frames;
    s.usable_frames = g_usable;
    s.used_frames = g_used;
    s.free_frames = g_usable - g_used;
    s.reserved_frames = g_frames - g_usable;
    u64 run = 0, best = 0;
    for (u64 f = 0; f < g_frames; f++) {
        if (test(f)) run = 0;
        else if (++run > best) best = run;
    }
    s.largest_free_run = best;
    interrupts_restore(flags);
    return s;
}

u64 pmm_bitmap_checksum() {
    u64 h = 1469598103934665603ull;
    for (u64 i = 0; i < g_words; i++) {
        h ^= g_bitmap[i];
        h *= 1099511628211ull;
    }
    return h;
}
