// See pagecache.h.
#include <fs/pagecache.h>
#include <fs/vfs.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <sched/sched.h>

namespace {

constexpr u32 BUCKETS = 4096;
Page* g_hash[BUCKETS];
Page* g_lru_head = nullptr;     // least recently used
Page* g_lru_tail = nullptr;     // most recently used
PageCacheStats g_stats;
Vnode* g_last_owner = nullptr;  // for spotting sequential reads
u64 g_last_index = 0;

u32 bucket(const Vnode* owner, u64 index) {
    u64 h = ((u64)owner >> 4) * 0x9E3779B97F4A7C15ull ^ index * 0xC2B2AE3D27D4EB4Full;
    return (u32)(h >> 40) % BUCKETS;
}

void lru_unlink(Page* p) {
    if (p->lru_prev) p->lru_prev->lru_next = p->lru_next;
    else g_lru_head = p->lru_next;
    if (p->lru_next) p->lru_next->lru_prev = p->lru_prev;
    else g_lru_tail = p->lru_prev;
    p->lru_prev = p->lru_next = nullptr;
}

void lru_append(Page* p) {
    p->lru_prev = g_lru_tail;
    p->lru_next = nullptr;
    if (g_lru_tail) g_lru_tail->lru_next = p;
    else g_lru_head = p;
    g_lru_tail = p;
}

Page* find(const Vnode* owner, u64 index) {
    for (Page* p = g_hash[bucket(owner, index)]; p; p = p->hash_next)
        if (p->owner == owner && p->index == index) return p;
    return nullptr;
}

void hash_remove(Page* p) {
    Page** link = &g_hash[bucket(p->owner, p->index)];
    while (*link && *link != p) link = &(*link)->hash_next;
    if (*link) *link = p->hash_next;
}

void free_page(Page* p) {
    hash_remove(p);
    lru_unlink(p);
    if (p->dirty) g_stats.dirty--;
    kfree(p->data);
    kfree(p);
    g_stats.pages--;
}

Result<void> write_back(Page* p) {
    if (!p->dirty) return {};
    Result<void> r = p->io->flush(p->owner, p->index, p->data);
    if (!r.ok()) return r;
    p->dirty = false;
    g_stats.dirty--;
    g_stats.writebacks++;
    return {};
}

// Makes room for one more page: evicts the oldest unused page (writing it
// back first if dirty). Gives up quietly if every page is in use.
void make_room() {
    while (g_stats.pages >= g_stats.max_pages) {
        Page* victim = g_lru_head;
        while (victim && (victim->refs || victim->held)) victim = victim->lru_next;
        if (!victim) return;
        if (victim->dirty && !write_back(victim).ok()) {
            // Cannot write it: keep it, try the next one.
            lru_unlink(victim);
            lru_append(victim);
            return;
        }
        free_page(victim);
        g_stats.evictions++;
    }
}

Page* new_page(Vnode* owner, u64 index, const PageIo* io) {
    make_room();
    Page* p = (Page*)kzalloc(sizeof(Page));
    if (!p) return nullptr;
    p->data = (u8*)kmalloc(PAGE_SIZE);
    if (!p->data) {
        kfree(p);
        return nullptr;
    }
    p->owner = owner;
    p->index = index;
    p->io = io;
    u32 b = bucket(owner, index);
    p->hash_next = g_hash[b];
    g_hash[b] = p;
    lru_append(p);
    g_stats.pages++;
    return p;
}

void writeback_main(void*) {
    for (;;) {
        thread_sleep_ms(5000);
        vfs_sync();
    }
}

} // namespace

void page_cache_init() {
    // A quarter of RAM, at least 256 pages.
    u64 frames = pmm_stats().usable_frames;
    g_stats.max_pages = frames / 4 > 256 ? frames / 4 : 256;
}

void page_cache_start_writeback() {
    Result<Thread*> t = kthread_create(writeback_main, nullptr, "writeback", prio::LOW, nullptr, true);
    if (!t.ok()) kprintf("pagecache: could not start the write-back thread\n");
}

Result<Page*> page_get(Vnode* owner, u64 index, const PageIo* io, bool fill) {
    Page* p = find(owner, index);
    if (p) {
        g_stats.hits++;
        p->refs++;
        lru_unlink(p);
        lru_append(p);
        if (fill && !p->uptodate) {
            u8* const one[1] = {p->data};
            Result<void> r = io->fill(owner, index, 1, one);
            if (!r.ok()) {
                p->refs--;
                return r.error();
            }
            p->uptodate = true;
        }
        return p;
    }
    p = new_page(owner, index, io);
    if (!p) return Error::NoMemory;
    p->refs = 1;
    if (!fill) return p;
    g_stats.misses++;

    // Sequential access: read the following pages in the same request.
    Page* batch[PAGE_CACHE_READAHEAD];
    u8* datas[PAGE_CACHE_READAHEAD];
    u32 count = 1;
    batch[0] = p;
    datas[0] = p->data;
    bool sequential = owner == g_last_owner && index == g_last_index + 1;
    if (sequential) {
        while (count < PAGE_CACHE_READAHEAD && !find(owner, index + count) && g_stats.pages + 1 < g_stats.max_pages) {
            Page* ra = new_page(owner, index + count, io);
            if (!ra) break;
            ra->refs = 1;           // held until filled
            batch[count] = ra;
            datas[count] = ra->data;
            count++;
        }
    }
    g_last_owner = owner;
    g_last_index = index + count - 1;
    Result<void> r = io->fill(owner, index, count, datas);
    for (u32 i = 0; i < count; i++) {
        if (i) batch[i]->refs--;
        if (r.ok()) batch[i]->uptodate = true;
    }
    if (count > 1) g_stats.readahead += count - 1;
    if (!r.ok()) {
        // Leave the pages not up to date; the next access retries.
        p->refs--;
        return r.error();
    }
    return p;
}

void page_put(Page* p) {
    ASSERT_ALWAYS(p->refs > 0);
    p->refs--;
}

void page_hold(Page* p, bool held) { p->held = held; }

Result<void> page_write(Page* p) { return write_back(p); }

void page_mark_dirty(Page* p) {
    p->uptodate = true;
    if (!p->dirty) {
        p->dirty = true;
        g_stats.dirty++;
    }
}

Result<void> page_sync_owner(Vnode* owner) {
    // Gather this owner's dirty pages and write them in index order, so a
    // file reaches the disk front to back.
    usize n = 0;
    for (Page* p = g_lru_head; p; p = p->lru_next) n += p->owner == owner && p->dirty && !p->held;
    if (!n) return {};
    Page** list = (Page**)kmalloc(n * sizeof(Page*));
    if (!list) {
        // No memory for the list: write them in cache order instead.
        for (Page* p = g_lru_head; p; p = p->lru_next)
            if (p->owner == owner && p->dirty && !p->held) {
                Result<void> r = write_back(p);
                if (!r.ok()) return r;
            }
        return {};
    }
    usize k = 0;
    for (Page* p = g_lru_head; p && k < n; p = p->lru_next)
        if (p->owner == owner && p->dirty && !p->held) list[k++] = p;
    // Shell sort by index: no recursion, fine for tens of thousands.
    for (usize gap = k / 2; gap > 0; gap /= 2)
        for (usize i = gap; i < k; i++)
            for (usize j = i; j >= gap && list[j - gap]->index > list[j]->index; j -= gap) {
                Page* t = list[j];
                list[j] = list[j - gap];
                list[j - gap] = t;
            }
    Result<void> r;
    for (usize i = 0; i < k && r.ok(); i++) r = write_back(list[i]);
    kfree(list);
    return r;
}

Result<void> page_sync_all() {
    Result<void> first;
    for (Page* p = g_lru_head; p; p = p->lru_next) {
        if (!p->dirty || p->held) continue;
        Result<void> r = write_back(p);
        if (!r.ok() && first.ok()) first = r;
    }
    return first;
}

void page_drop_owner(Vnode* owner, u64 from) {
    Page* p = g_lru_head;
    while (p) {
        Page* next = p->lru_next;
        if (p->owner == owner && p->index >= from) {
            ASSERT_ALWAYS(p->refs == 0);
            free_page(p);
        }
        p = next;
    }
    if (g_last_owner == owner) g_last_owner = nullptr;
}

PageCacheStats page_cache_stats() { return g_stats; }
