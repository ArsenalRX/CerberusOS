// Slab allocator and large-allocation path. See kheap.h for the contract.
//
// Slab page layout:   [Slab header, 32 bytes][object][object]...
// Object layout, release:  [payload: class size]
// Object layout, debug:    [Track, 16][red zone, 16][payload: class size][red zone, 16]
// A free object's first 8 bytes hold the encoded link to the next free
// object of the same slab; in debug builds the rest of it is 0xEF.
//
// Large allocations are always page-aligned and slab payloads never are
// (the header occupies the start of the page), which is how kfree tells
// them apart.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpuid.h>
#include <boot/bootinfo.h>
#include <lib/csprng.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <lib/symbols.h>
#include <mm/early_map.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

namespace {

constexpr int CLASS_COUNT = 8;
constexpr u32 CLASS_SIZE[CLASS_COUNT] = {16, 32, 64, 128, 256, 512, 1024, 2048};

constexpr u16 SLAB_MAGIC = 0x51AB;
constexpr u8 POISON_ALLOC = 0xDE;
constexpr u8 POISON_FREE = 0xEF;
constexpr u8 RED_ZONE = 0xBB;

#ifdef LUMEN_DEBUG
constexpr usize RED_ZONE_SIZE = 16;
constexpr u32 STATE_ALLOCATED = 0xA110C8ED;
constexpr u32 STATE_FREE = 0xF4EEF4EE;
struct Track {
    u64 caller;         // overlaps the free-list link while the object is free
    u32 requested;
    u32 state;
};
constexpr usize OBJECT_OVERHEAD = sizeof(Track) + 2 * RED_ZONE_SIZE;
static_assert(sizeof(Track) + RED_ZONE_SIZE == KHEAP_PAYLOAD_OFFSET);
#else
constexpr usize OBJECT_OVERHEAD = 0;
#endif

struct Slab {
    Slab* next;
    Slab* prev;
    u8* free_head;      // first free object, nullptr when the slab is full
    u16 inuse;
    u16 capacity;
    u8 cls;
    u8 on_full_list;
    u16 magic;
};
static_assert(sizeof(Slab) == 32);

struct Cache {
    Slab* partial;      // slabs with at least one free object
    Slab* full;
    u64 slabs;
    u64 inuse;
};

struct LargeNode {
    LargeNode* next;
    void* addr;
    usize pages;
    usize requested;
    u64 caller;
    bool vmapped;       // true: VMM mapping; false: contiguous frames via the direct map
};

Cache g_caches[CLASS_COUNT];
LargeNode* g_large = nullptr;
KheapStats g_stats;
u64 g_secret = 0;
bool g_ready = false;
u64 g_ram_top = 0;             // end of physical RAM; heap pointers lie below it in the direct map

inline paddr_t phys_of(const void* p) { return (u64)p - g_boot_info.hhdm_offset; }
inline usize stride_of(int cls) { return CLASS_SIZE[cls] + OBJECT_OVERHEAD; }
inline u8* first_object(Slab* s) { return (u8*)s + sizeof(Slab); }
inline u64 encode_link(const u8* next, const u8* slot) { return (u64)next ^ g_secret ^ (u64)slot; }

int class_for(usize size) {
    for (int c = 0; c < CLASS_COUNT; c++)
        if (size <= CLASS_SIZE[c]) return c;
    return -1;
}

// True if `obj` is the start of an object of slab `s`.
bool object_in_slab(Slab* s, const u8* obj) {
    const u8* first = first_object(s);
    usize stride = stride_of(s->cls);
    return obj >= first && obj < first + (usize)s->capacity * stride && (usize)(obj - first) % stride == 0;
}

void list_remove(Slab** head, Slab* s) {
    if (s->prev) s->prev->next = s->next;
    else *head = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = nullptr;
}

void list_push(Slab** head, Slab* s) {
    s->prev = nullptr;
    s->next = *head;
    if (*head) (*head)->prev = s;
    *head = s;
}

#ifdef LUMEN_DEBUG
bool all_bytes(const u8* p, usize n, u8 value) {
    for (usize i = 0; i < n; i++)
        if (p[i] != value) return false;
    return true;
}

void print_site(u64 addr) {
    u64 off = 0;
    const char* name = symbols_lookup(addr, &off);
    if (name) kprintf("%s+%#lx", name, (unsigned long)off);
    else kprintf("%#lx", (unsigned long)addr);
}

#endif

Slab* new_slab(int cls) {
    paddr_t p = pmm_alloc(1);
    if (p == PMM_NO_MEMORY) return nullptr;
    Slab* s = (Slab*)hhdm_virt(p);
    usize stride = stride_of(cls);
#ifdef LUMEN_DEBUG
    memset(s, POISON_FREE, PAGE_SIZE);
#endif
    s->next = s->prev = nullptr;
    s->inuse = 0;
    s->capacity = (u16)((PAGE_SIZE - sizeof(Slab)) / stride);
    s->cls = (u8)cls;
    s->on_full_list = 0;
    s->magic = SLAB_MAGIC;
    // Thread the free list through the objects, lowest address first.
    u8* first = first_object(s);
    for (usize i = 0; i < s->capacity; i++) {
        u8* obj = first + i * stride;
        u8* next = i + 1 < s->capacity ? obj + stride : nullptr;
        *(u64*)obj = encode_link(next, obj);
#ifdef LUMEN_DEBUG
        ((Track*)obj)->state = STATE_FREE;
        ((Track*)obj)->requested = 0;
#endif
    }
    s->free_head = first;
    g_caches[cls].slabs++;
    g_stats.slab_pages++;
    return s;
}

void release_slab(Cache& c, Slab* s) {
    list_remove(&c.partial, s);
    s->magic = 0;
    c.slabs--;
    g_stats.slab_pages--;
    pmm_free(phys_of(s), 1);
}

// Returns the payload pointer, or nullptr when out of memory.
void* slab_alloc(int cls, usize requested, u64 caller) {
    Cache& c = g_caches[cls];
    Slab* s = c.partial;
    if (!s) {
        s = new_slab(cls);
        if (!s) return nullptr;
        list_push(&c.partial, s);
    }
    u8* obj = s->free_head;
    if (s->magic != SLAB_MAGIC || !object_in_slab(s, obj))
        PANIC("heap: corrupted free list in the %u-byte slab at %p (head %p)", CLASS_SIZE[cls], (void*)s,
              (void*)obj);
    u8* next = (u8*)(*(u64*)obj ^ g_secret ^ (u64)obj);
    if (next && !object_in_slab(s, next))
        PANIC("heap: corrupted free list in the %u-byte slab at %p (object %p links to %p)", CLASS_SIZE[cls],
              (void*)s, (void*)obj, (void*)next);
    s->free_head = next;
    s->inuse++;
    c.inuse++;
    if (!next) {
        list_remove(&c.partial, s);
        list_push(&c.full, s);
        s->on_full_list = 1;
    }
    g_stats.live_objects++;
    g_stats.total_allocs++;

#ifdef LUMEN_DEBUG
    Track* t = (Track*)obj;
    usize stride = stride_of(cls);
    if (t->state != STATE_FREE || !all_bytes(obj + sizeof(Track), stride - sizeof(Track), POISON_FREE))
        PANIC("heap: write after free detected in a %u-byte object at %p", CLASS_SIZE[cls],
              (void*)(obj + KHEAP_PAYLOAD_OFFSET));
    t->caller = caller;
    t->requested = (u32)requested;
    t->state = STATE_ALLOCATED;
    u8* payload = obj + KHEAP_PAYLOAD_OFFSET;
    memset(obj + sizeof(Track), RED_ZONE, RED_ZONE_SIZE);
    memset(payload, POISON_ALLOC, requested);
    memset(payload + requested, RED_ZONE, stride - KHEAP_PAYLOAD_OFFSET - requested);
    return payload;
#else
    (void)requested;
    (void)caller;
    return obj;
#endif
}

#ifdef LUMEN_DEBUG
// Both red zones and the slack after the requested size must be untouched.
bool red_zones_intact(Slab* s, const u8* obj) {
    const Track* t = (const Track*)obj;
    usize stride = stride_of(s->cls);
    if (t->requested > CLASS_SIZE[s->cls]) return false;
    return all_bytes(obj + sizeof(Track), RED_ZONE_SIZE, RED_ZONE) &&
           all_bytes(obj + KHEAP_PAYLOAD_OFFSET + t->requested, stride - KHEAP_PAYLOAD_OFFSET - t->requested,
                     RED_ZONE);
}
#endif

// Finds the slab a payload pointer belongs to; nullptr if it is not one.
Slab* slab_of(const void* ptr, u8** obj_out) {
    u64 v = (u64)ptr;
    u64 hhdm = g_boot_info.hhdm_offset;
    if (v < hhdm || v - hhdm >= g_ram_top) return nullptr;
    Slab* s = (Slab*)align_down(v, PAGE_SIZE);
    if (s->magic != SLAB_MAGIC || s->cls >= CLASS_COUNT) return nullptr;
    u8* obj = (u8*)ptr - KHEAP_PAYLOAD_OFFSET;
    if (!object_in_slab(s, obj)) return nullptr;
    *obj_out = obj;
    return s;
}

void slab_free(void* ptr) {
    u8* obj = nullptr;
    Slab* s = slab_of(ptr, &obj);
    if (!s) PANIC("kfree: %p was not allocated by the kernel heap", ptr);
    Cache& c = g_caches[s->cls];
#ifdef LUMEN_DEBUG
    Track* t = (Track*)obj;
    if (t->state != STATE_ALLOCATED) PANIC("kfree: double free or invalid free of %p", ptr);
    if (!red_zones_intact(s, obj)) {
        kprintf("heap: buffer overrun around the %u-byte object at %p (requested %u bytes), allocated by ",
                CLASS_SIZE[s->cls], ptr, t->requested);
        print_site(t->caller);
        kprintf("\n");
        PANIC("heap: red zone damaged");
    }
    memset(obj + sizeof(Track), POISON_FREE, stride_of(s->cls) - sizeof(Track));
    t->requested = 0;
    t->state = STATE_FREE;
#else
    if (obj == s->free_head) PANIC("kfree: double free of %p", ptr);
#endif
    *(u64*)obj = encode_link(s->free_head, obj);
    s->free_head = obj;
    if (s->on_full_list) {
        list_remove(&c.full, s);
        list_push(&c.partial, s);
        s->on_full_list = 0;
    }
    s->inuse--;
    c.inuse--;
    g_stats.live_objects--;
    g_stats.total_frees++;
    // Give an empty slab back unless it is the only one with free objects,
    // so a cache that hovers around empty does not thrash the frame allocator.
    if (s->inuse == 0 && (s->next || s->prev)) release_slab(c, s);
}

LargeNode** find_large(const void* addr) {
    for (LargeNode** link = &g_large; *link; link = &(*link)->next)
        if ((*link)->addr == addr) return link;
    return nullptr;
}

void* large_alloc(usize size, u64 caller) {
    usize pages = align_up(size, PAGE_SIZE) / PAGE_SIZE;
    if (pages * PAGE_SIZE < size) return nullptr;
    LargeNode* node = (LargeNode*)slab_alloc(class_for(sizeof(LargeNode)), sizeof(LargeNode), (u64)&large_alloc);
    if (!node) return nullptr;
    void* addr = nullptr;
    bool vmapped = false;
    paddr_t phys = pmm_alloc(pages);
    if (phys != PMM_NO_MEMORY) {
        addr = hhdm_virt(phys);
    } else {
        // No contiguous run: fall back to scattered frames behind a mapping.
        Result<vaddr_t> r = vmm_kernel().mmap(0, pages * PAGE_SIZE, vm::WRITE, 0);
        if (r.ok()) {
            addr = (void*)r.value();
            vmapped = true;
        }
    }
    if (!addr) {
        slab_free(node);
        return nullptr;
    }
    node->addr = addr;
    node->pages = pages;
    node->requested = size;
    node->caller = caller;
    node->vmapped = vmapped;
    node->next = g_large;
    g_large = node;
    g_stats.large_pages += pages;
    g_stats.live_objects++;
    g_stats.total_allocs++;
#ifdef LUMEN_DEBUG
    memset(addr, POISON_ALLOC, size);
    memset((u8*)addr + size, RED_ZONE, pages * PAGE_SIZE - size);
#endif
    return addr;
}

#ifdef LUMEN_DEBUG
bool large_slack_intact(const LargeNode* n) {
    return all_bytes((const u8*)n->addr + n->requested, n->pages * PAGE_SIZE - n->requested, RED_ZONE);
}
#endif

void large_free(void* ptr) {
    LargeNode** link = find_large(ptr);
    if (!link) PANIC("kfree: %p was not allocated by the kernel heap (or was already freed)", ptr);
    LargeNode* n = *link;
#ifdef LUMEN_DEBUG
    if (!large_slack_intact(n)) {
        kprintf("heap: buffer overrun past the %lu-byte allocation at %p, allocated by ",
                (unsigned long)n->requested, ptr);
        print_site(n->caller);
        kprintf("\n");
        PANIC("heap: red zone damaged");
    }
    memset(ptr, POISON_FREE, n->pages * PAGE_SIZE);
#endif
    *link = n->next;
    if (n->vmapped) {
        Result<void> r = vmm_kernel().munmap((vaddr_t)ptr, n->pages * PAGE_SIZE);
        ASSERT_ALWAYS(r.ok());
    } else {
        pmm_free(phys_of(ptr), n->pages);
    }
    g_stats.large_pages -= n->pages;
    g_stats.live_objects--;
    g_stats.total_frees++;
    slab_free(n);
}

void* alloc(usize size, u64 caller) {
    if (!size) return nullptr;
    ASSERT_ALWAYS(g_ready);
    u64 irq = interrupts_save();
    int cls = class_for(size);
    void* p = cls >= 0 ? slab_alloc(cls, size, caller) : large_alloc(size, caller);
    interrupts_restore(irq);
    return p;
}

inline bool is_large_pointer(const void* p) { return ((u64)p & (PAGE_SIZE - 1)) == 0; }

} // namespace

void kheap_init() {
    // Per-boot secret for the free-list links, from the kernel CSPRNG.
    g_secret = csprng_u64();
    g_ram_top = g_boot_info.total_bytes();
    g_ready = true;
    kprintf("kheap: slab classes 16-%lu bytes, %lu bytes of overhead per object (%s build)\n",
            (unsigned long)KMALLOC_MAX_SLAB,
            (unsigned long)OBJECT_OVERHEAD, OBJECT_OVERHEAD ? "debug" : "release");
}

__attribute__((noinline)) void* kmalloc(usize size) { return alloc(size, (u64)__builtin_return_address(0)); }

__attribute__((noinline)) void* kzalloc(usize size) {
    void* p = alloc(size, (u64)__builtin_return_address(0));
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void* ptr) {
    if (!ptr) return;
    u64 irq = interrupts_save();
    if (is_large_pointer(ptr)) large_free(ptr);
    else slab_free(ptr);
    interrupts_restore(irq);
}

usize ksize(const void* ptr) {
    if (!ptr) return 0;
    usize size = 0;
    u64 irq = interrupts_save();
    if (is_large_pointer(ptr)) {
        LargeNode** link = find_large(ptr);
        if (link) size = (*link)->pages * PAGE_SIZE;
    } else {
        u8* obj = nullptr;
        Slab* s = slab_of(ptr, &obj);
        if (s) size = CLASS_SIZE[s->cls];
    }
    interrupts_restore(irq);
    return size;
}

__attribute__((noinline)) void* krealloc(void* ptr, usize size) {
    u64 caller = (u64)__builtin_return_address(0);
    if (!ptr) return alloc(size, caller);
    if (!size) {
        kfree(ptr);
        return nullptr;
    }
    usize old = ksize(ptr);
    if (!old) PANIC("krealloc: %p was not allocated by the kernel heap", ptr);
    // Stay in place when the new size still belongs to the same size class
    // (or the same number of pages); otherwise move.
    bool same = is_large_pointer(ptr) ? (size > KMALLOC_MAX_SLAB && align_up(size, PAGE_SIZE) == old)
                                      : (class_for(size) >= 0 && CLASS_SIZE[class_for(size)] == old);
    if (same) {
#ifdef LUMEN_DEBUG
        u64 irq = interrupts_save();
        if (is_large_pointer(ptr)) {
            LargeNode* n = *find_large(ptr);
            if (size > n->requested) memset((u8*)ptr + n->requested, POISON_ALLOC, size - n->requested);
            n->requested = size;
            memset((u8*)ptr + size, RED_ZONE, old - size);
        } else {
            Track* t = (Track*)((u8*)ptr - KHEAP_PAYLOAD_OFFSET);
            if (size > t->requested) memset((u8*)ptr + t->requested, POISON_ALLOC, size - t->requested);
            t->requested = (u32)size;
            memset((u8*)ptr + size, RED_ZONE, old - size);
        }
        interrupts_restore(irq);
#endif
        return ptr;
    }
    void* fresh = alloc(size, caller);
    if (!fresh) return nullptr;
    usize keep = min(size, old);
#ifdef LUMEN_DEBUG
    // Only the bytes the caller asked for are meaningful; the rest is red zone.
    u64 irq = interrupts_save();
    usize requested = is_large_pointer(ptr) ? (*find_large(ptr))->requested
                                            : ((Track*)((u8*)ptr - KHEAP_PAYLOAD_OFFSET))->requested;
    interrupts_restore(irq);
    keep = min(keep, requested);
#endif
    memcpy(fresh, ptr, keep);
    kfree(ptr);
    return fresh;
}

void kfree_sensitive(void* ptr, usize size) {
    if (!ptr) return;
    volatile u8* p = (volatile u8*)ptr;     // volatile: the wipe must not be optimised away
    for (usize i = 0; i < size; i++) p[i] = 0;
    kfree(ptr);
}

bool kheap_check(const void* ptr) {
    if (!ptr) return false;
    bool ok = false;
    u64 irq = interrupts_save();
    if (is_large_pointer(ptr)) {
        LargeNode** link = find_large(ptr);
        ok = link != nullptr;
#ifdef LUMEN_DEBUG
        if (ok) ok = large_slack_intact(*link);
#endif
    } else {
        u8* obj = nullptr;
        Slab* s = slab_of(ptr, &obj);
        ok = s != nullptr;
#ifdef LUMEN_DEBUG
        if (ok) ok = ((Track*)obj)->state == STATE_ALLOCATED && red_zones_intact(s, obj);
#endif
    }
    interrupts_restore(irq);
    return ok;
}

KheapStats kheap_stats() {
    u64 irq = interrupts_save();
    KheapStats s = g_stats;
    interrupts_restore(irq);
    return s;
}

void kheap_report() {
    u64 irq = interrupts_save();
    kprintf("kernel heap: %lu slab page(s), %lu large page(s), %lu live allocation(s), %lu allocs / %lu frees "
            "since boot\n",
            (unsigned long)g_stats.slab_pages, (unsigned long)g_stats.large_pages,
            (unsigned long)g_stats.live_objects, (unsigned long)g_stats.total_allocs,
            (unsigned long)g_stats.total_frees);
    for (int c = 0; c < CLASS_COUNT; c++) {
        if (!g_caches[c].slabs) continue;
        kprintf("  %4u bytes: %lu in use, %lu slab(s)\n", CLASS_SIZE[c], (unsigned long)g_caches[c].inuse,
                (unsigned long)g_caches[c].slabs);
    }
#ifdef LUMEN_DEBUG
    // Outstanding allocations grouped by call site. The table is static so
    // the report itself allocates nothing.
    struct Site {
        u64 caller;
        u64 count;
        u64 bytes;
    };
    static Site sites[128];
    usize used = 0;
    u64 unlisted = 0;
    auto note = [&](u64 caller, u64 bytes) {
        for (usize i = 0; i < used; i++) {
            if (sites[i].caller == caller) {
                sites[i].count++;
                sites[i].bytes += bytes;
                return;
            }
        }
        if (used == sizeof sites / sizeof sites[0]) {
            unlisted++;
            return;
        }
        sites[used++] = {caller, 1, bytes};
    };
    for (int c = 0; c < CLASS_COUNT; c++) {
        Slab* lists[2] = {g_caches[c].partial, g_caches[c].full};
        for (Slab* head : lists) {
            for (Slab* s = head; s; s = s->next) {
                u8* first = first_object(s);
                for (usize i = 0; i < s->capacity; i++) {
                    Track* t = (Track*)(first + i * stride_of(c));
                    if (t->state == STATE_ALLOCATED) note(t->caller, t->requested);
                }
            }
        }
    }
    for (LargeNode* n = g_large; n; n = n->next) note(n->caller, n->requested);
    kprintf("  outstanding by call site:\n");
    for (usize i = 0; i < used; i++) {
        kprintf("    %6lu allocation(s), %8lu bytes  ", (unsigned long)sites[i].count, (unsigned long)sites[i].bytes);
        print_site(sites[i].caller);
        kprintf("\n");
    }
    if (unlisted) kprintf("    (%lu more allocation(s) from call sites beyond the table)\n", (unsigned long)unlisted);
#endif
    interrupts_restore(irq);
}
