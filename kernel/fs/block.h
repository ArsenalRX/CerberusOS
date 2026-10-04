// Block devices (disks). A driver registers a BlockDevice; it becomes
// /dev/disk/<name> (sda, sdb, ...) with minor number = registration order.
//
// Two ways in:
//   - byte access through the device node (read/write on /dev/disk/sda,
//     and file systems' metadata), cached in the page cache under the
//     device vnode, one page = 4 KiB of the disk;
//   - raw sector access (block_dev_read/write), uncached, used by file
//     systems for file data that the page cache already holds under the
//     file's own vnode, so the same bytes are never cached twice.
//
// Every request is bounds-checked against the device size here, so drivers
// see only valid ranges.
#pragma once

#include <lib/result.h>
#include <lib/types.h>

struct Vnode;
struct PageIo;

struct BlockDevice {
    char name[8];               // "sda"
    char model[41];             // from the device, trimmed
    u32 sector_size;            // bytes; 512 for every driver so far
    u64 sectors;
    Result<void> (*read)(BlockDevice* d, u64 lba, u32 count, void* buf);
    Result<void> (*write)(BlockDevice* d, u64 lba, u32 count, const void* buf);
    Result<void> (*flush)(BlockDevice* d);
    void* drv;                  // driver state
    u32 max_sectors;            // per request
    u64 reads, writes;          // sectors, for statistics
};

constexpr u32 BLOCK_MAX_DEVICES = 8;
// ioctl requests on block device nodes.
constexpr u32 BLKGETSIZE64 = 0x80081272;    // u64 size in bytes (Linux's number)

// Registers a device and returns its minor number. NoSpace if full.
Result<u32> block_register(BlockDevice* d);
u32 block_count();
BlockDevice* block_get(u32 minor);
bool block_present(u32 minor);
// A mounted file system claims its disk; while claimed, the disk's node
// cannot be opened for writing (so mkfs cannot format a mounted disk) and it
// cannot be mounted twice. claim returns false if already claimed.
bool block_claim(u32 minor);
void block_unclaim(u32 minor);
bool block_claimed(u32 minor);

// Raw sector I/O, uncached. Invalid if out of range.
Result<void> block_dev_read(u32 minor, u64 lba, u32 count, void* buf);
Result<void> block_dev_write(u32 minor, u64 lba, u32 count, const void* buf);
Result<void> block_dev_flush(u32 minor);

// Cached byte I/O through the device vnode (`node`). Caller holds the VFS
// lock (the VFS takes it for block device nodes).
Result<usize> block_read_bytes(u32 minor, Vnode* node, u64 off, void* buf, usize n);
Result<usize> block_write_bytes(u32 minor, Vnode* node, u64 off, const void* buf, usize n);
// Writes the node's dirty pages and flushes the device's write cache.
Result<void> block_sync(u32 minor, Vnode* node);
Result<i64> block_ioctl(u32 minor, u32 request, u64 arg);
// The page-cache backing for device vnodes (page index = 4 KiB block).
const PageIo* block_page_io();
