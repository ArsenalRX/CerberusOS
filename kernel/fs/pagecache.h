// The page cache (SPEC §5A phase 9): one cache of 4 KiB pages for file
// contents and for block devices, keyed by (vnode, page index). File reads
// and writes, file system metadata and device node I/O all go through it,
// so each byte of a disk is cached at most once.
//
//   - A miss reads the page through the owner's PageIo; sequential misses
//     read ahead up to READAHEAD pages in one request.
//   - Writes mark pages dirty; dirty pages reach the disk on fsync/sync, on
//     eviction, and from the write-back thread (every 5 s), never later.
//   - Size is bounded (a quarter of RAM); clean unused pages are evicted
//     oldest first.
//
// Every function must be called with the VFS lock held.
#pragma once

#include <lib/result.h>
#include <lib/types.h>

struct Vnode;

constexpr u32 PAGE_CACHE_READAHEAD = 16;

struct PageIo {
    // Fills `count` consecutive pages starting at `index` (count >= 1).
    // Pages past the end of the object are zero-filled by the callee.
    Result<void> (*fill)(Vnode* owner, u64 index, u32 count, u8* const* pages);
    // Writes one page back.
    Result<void> (*flush)(Vnode* owner, u64 index, const u8* page);
};

struct Page {
    Vnode* owner;
    u64 index;
    u8* data;                   // PAGE_SIZE bytes
    const PageIo* io;
    u32 refs;
    bool dirty, uptodate;
    bool held;                  // part of an uncommitted journal transaction: never written back
    Page* hash_next;
    Page* lru_prev;
    Page* lru_next;
};

struct PageCacheStats {
    u64 pages, max_pages, dirty;
    u64 hits, misses, readahead, writebacks, evictions;
};

void page_cache_init();
// Starts the write-back thread (needs the scheduler).
void page_cache_start_writeback();

// The page (index) of `owner`, with a reference. With fill, a page not yet
// read is read first (IO on failure); without fill the page may come back
// not up to date (the caller is about to overwrite all of it).
Result<Page*> page_get(Vnode* owner, u64 index, const PageIo* io, bool fill);
void page_put(Page* p);
void page_mark_dirty(Page* p);
// Holds a dirty page back from write-back and eviction (a file system's
// journal does this until the transaction that changed it is committed).
void page_hold(Page* p, bool held);
// Writes one dirty page now, held or not.
Result<void> page_write(Page* p);
// Writes back every dirty page of `owner`, in index order.
Result<void> page_sync_owner(Vnode* owner);
// Writes back every dirty page in the cache.
Result<void> page_sync_all();
// Drops pages of `owner` from index `from` on (truncate, delete). Dirty
// pages are discarded, not written.
void page_drop_owner(Vnode* owner, u64 from);
PageCacheStats page_cache_stats();
