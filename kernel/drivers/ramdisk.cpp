// See ramdisk.h.
#include <drivers/ramdisk.h>
#include <fs/block.h>
#include <lib/string.h>
#include <mm/kheap.h>

namespace {

struct Disk {
    RamDisk rd;
    BlockDevice dev;
};

Result<void> rd_read(BlockDevice* bd, u64 lba, u32 count, void* buf) {
    Disk* d = (Disk*)bd->drv;
    memcpy(buf, d->rd.data + lba * 512, (usize)count * 512);
    return {};
}

Result<void> rd_write(BlockDevice* bd, u64 lba, u32 count, const void* buf) {
    Disk* d = (Disk*)bd->drv;
    memcpy(d->rd.data + lba * 512, buf, (usize)count * 512);
    return {};
}

} // namespace

Result<RamDisk*> ramdisk_create(u64 bytes) {
    Disk* d = (Disk*)kzalloc(sizeof(Disk));
    if (!d) return Error::NoMemory;
    d->rd.data = (u8*)kzalloc(bytes);
    if (!d->rd.data) {
        kfree(d);
        return Error::NoMemory;
    }
    d->rd.bytes = bytes;
    strlcpy(d->dev.model, "RAM disk", sizeof d->dev.model);
    d->dev.sector_size = 512;
    d->dev.sectors = bytes / 512;
    d->dev.read = rd_read;
    d->dev.write = rd_write;
    d->dev.drv = d;
    d->dev.max_sectors = 1024;
    Result<u32> minor = block_register(&d->dev);
    if (!minor.ok()) {
        kfree(d->rd.data);
        kfree(d);
        return minor.error();
    }
    d->rd.minor = minor.value();
    return &d->rd;
}
