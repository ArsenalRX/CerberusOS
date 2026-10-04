// See block.h.
#include <fs/block.h>
#include <fs/dev.h>
#include <fs/pagecache.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/usercopy.h>

namespace {

BlockDevice* g_devices[BLOCK_MAX_DEVICES];
bool g_claimed[BLOCK_MAX_DEVICES];
u32 g_count = 0;

constexpr u32 SECTORS_PER_PAGE = PAGE_SIZE / 512;

Result<void> check_range(BlockDevice* d, u64 lba, u32 count) {
    if (count == 0 || lba >= d->sectors || count > d->sectors - lba) return Error::Invalid;
    return {};
}

u64 device_bytes(BlockDevice* d) { return d->sectors * d->sector_size; }

// Page-cache backing for device vnodes: page i = sectors [8i, 8i+8).
Result<void> dev_fill(Vnode* owner, u64 index, u32 count, u8* const* pages) {
    u32 minor = dev::minor_of(owner->rdev);
    BlockDevice* d = block_get(minor);
    if (!d) return Error::NoDevice;
    u64 lba = index * SECTORS_PER_PAGE;
    // Pages past the end of the disk read as zeros.
    u32 whole = 0;
    while (whole < count && lba + (u64)(whole + 1) * SECTORS_PER_PAGE <= d->sectors) whole++;
    if (whole) {
        // One request into a bounce buffer when there are several pages.
        if (whole == 1) {
            Result<void> r = block_dev_read(minor, lba, SECTORS_PER_PAGE, pages[0]);
            if (!r.ok()) return r;
        } else {
            u8* bounce = (u8*)kmalloc((usize)whole * PAGE_SIZE);
            if (!bounce) {
                for (u32 i = 0; i < whole; i++) {
                    Result<void> r = block_dev_read(minor, lba + (u64)i * SECTORS_PER_PAGE, SECTORS_PER_PAGE, pages[i]);
                    if (!r.ok()) return r;
                }
            } else {
                Result<void> r = block_dev_read(minor, lba, whole * SECTORS_PER_PAGE, bounce);
                if (r.ok())
                    for (u32 i = 0; i < whole; i++) memcpy(pages[i], bounce + (usize)i * PAGE_SIZE, PAGE_SIZE);
                kfree(bounce);
                if (!r.ok()) return r;
            }
        }
    }
    for (u32 i = whole; i < count; i++) memset(pages[i], 0, PAGE_SIZE);
    return {};
}

Result<void> dev_flush(Vnode* owner, u64 index, const u8* page) {
    u32 minor = dev::minor_of(owner->rdev);
    BlockDevice* d = block_get(minor);
    if (!d) return Error::NoDevice;
    u64 lba = index * SECTORS_PER_PAGE;
    if (lba >= d->sectors) return {};
    u32 count = d->sectors - lba < SECTORS_PER_PAGE ? (u32)(d->sectors - lba) : SECTORS_PER_PAGE;
    return block_dev_write(minor, lba, count, page);
}

const PageIo g_dev_io = {dev_fill, dev_flush};

} // namespace

Result<u32> block_register(BlockDevice* d) {
    if (g_count == BLOCK_MAX_DEVICES) return Error::NoSpace;
    u32 minor = g_count++;
    d->name[0] = 's';
    d->name[1] = 'd';
    d->name[2] = (char)('a' + minor);
    d->name[3] = 0;
    if (!d->max_sectors) d->max_sectors = 256;
    g_devices[minor] = d;
    return minor;
}

u32 block_count() { return g_count; }
BlockDevice* block_get(u32 minor) { return minor < g_count ? g_devices[minor] : nullptr; }
bool block_present(u32 minor) { return block_get(minor) != nullptr; }

bool block_claim(u32 minor) {
    if (minor >= g_count || g_claimed[minor]) return false;
    g_claimed[minor] = true;
    return true;
}

void block_unclaim(u32 minor) {
    if (minor < g_count) g_claimed[minor] = false;
}

bool block_claimed(u32 minor) { return minor < g_count && g_claimed[minor]; }

Result<void> block_dev_read(u32 minor, u64 lba, u32 count, void* buf) {
    BlockDevice* d = block_get(minor);
    if (!d) return Error::NoDevice;
    Result<void> ok = check_range(d, lba, count);
    if (!ok.ok()) return ok;
    u8* p = (u8*)buf;
    while (count) {
        u32 n = count < d->max_sectors ? count : d->max_sectors;
        Result<void> r = d->read(d, lba, n, p);
        if (!r.ok()) return r;
        d->reads += n;
        lba += n;
        count -= n;
        p += (usize)n * d->sector_size;
    }
    return {};
}

Result<void> block_dev_write(u32 minor, u64 lba, u32 count, const void* buf) {
    BlockDevice* d = block_get(minor);
    if (!d) return Error::NoDevice;
    Result<void> ok = check_range(d, lba, count);
    if (!ok.ok()) return ok;
    const u8* p = (const u8*)buf;
    while (count) {
        u32 n = count < d->max_sectors ? count : d->max_sectors;
        Result<void> r = d->write(d, lba, n, p);
        if (!r.ok()) return r;
        d->writes += n;
        lba += n;
        count -= n;
        p += (usize)n * d->sector_size;
    }
    return {};
}

Result<void> block_dev_flush(u32 minor) {
    BlockDevice* d = block_get(minor);
    if (!d) return Error::NoDevice;
    return d->flush ? d->flush(d) : Result<void>();
}

Result<usize> block_read_bytes(u32 minor, Vnode* node, u64 off, void* buf, usize n) {
    BlockDevice* d = block_get(minor);
    if (!d) return Error::NoDevice;
    u64 size = device_bytes(d);
    if (off >= size) return (usize)0;
    if (n > size - off) n = (usize)(size - off);
    usize done = 0;
    while (done < n) {
        u64 at = off + done;
        Result<Page*> pg = page_get(node, at / PAGE_SIZE, &g_dev_io, true);
        if (!pg.ok()) return done ? Result<usize>(done) : Result<usize>(pg.error());
        usize in = (usize)(at % PAGE_SIZE);
        usize take = PAGE_SIZE - in < n - done ? PAGE_SIZE - in : n - done;
        memcpy((u8*)buf + done, pg.value()->data + in, take);
        page_put(pg.value());
        done += take;
    }
    return done;
}

Result<usize> block_write_bytes(u32 minor, Vnode* node, u64 off, const void* buf, usize n) {
    BlockDevice* d = block_get(minor);
    if (!d) return Error::NoDevice;
    u64 size = device_bytes(d);
    if (off >= size) return n ? Result<usize>(Error::NoSpace) : Result<usize>((usize)0);
    if (n > size - off) n = (usize)(size - off);
    usize done = 0;
    while (done < n) {
        u64 at = off + done;
        usize in = (usize)(at % PAGE_SIZE);
        usize take = PAGE_SIZE - in < n - done ? PAGE_SIZE - in : n - done;
        // A whole page is replaced without reading it first.
        Result<Page*> pg = page_get(node, at / PAGE_SIZE, &g_dev_io, take != PAGE_SIZE);
        if (!pg.ok()) return done ? Result<usize>(done) : Result<usize>(pg.error());
        memcpy(pg.value()->data + in, (const u8*)buf + done, take);
        page_mark_dirty(pg.value());
        page_put(pg.value());
        done += take;
    }
    return done;
}

Result<void> block_sync(u32 minor, Vnode* node) {
    Result<void> r = page_sync_owner(node);
    if (!r.ok()) return r;
    return block_dev_flush(minor);
}

Result<i64> block_ioctl(u32 minor, u32 request, u64 arg) {
    BlockDevice* d = block_get(minor);
    if (!d) return Error::NoDevice;
    if (request == BLKGETSIZE64) {
        u64 bytes = device_bytes(d);
        Result<void> r = copy_to_user(arg, &bytes, sizeof bytes);
        return r.ok() ? Result<i64>(0) : Result<i64>(r.error());
    }
    return Error::NotSupported;
}

const PageIo* block_page_io() { return &g_dev_io; }
