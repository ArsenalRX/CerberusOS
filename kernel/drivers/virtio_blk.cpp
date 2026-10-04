// virtio-blk: the paravirtual disk (QEMU's fast path). One request at a
// time on queue 0, polled. See virtio.h for the transport.
#include <drivers/driver.h>
#include <drivers/refclock.h>
#include <drivers/virtio.h>
#include <drivers/virtio_blk.h>
#include <fs/block.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/early_map.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <sched/sync.h>

namespace {

constexpr u64 F_FLUSH = 1ull << 9;
constexpr u32 T_IN = 0, T_OUT = 1, T_FLUSH = 4;
constexpr u32 BOUNCE_PAGES = 16;                // 64 KiB per request
constexpr u64 TIMEOUT_US = 5000000;

struct Request {                                // lives in one DMA frame
    u32 type;
    u32 reserved;
    u64 sector;
    u8 status;
};

struct Disk {
    virtio::Device vdev;
    virtio::Queue queue;
    Request* req;
    paddr_t req_phys;
    u8* bounce;
    paddr_t bounce_phys;
    Mutex lock;
    BlockDevice dev;
};

Result<void> request(Disk* d, u32 type, u64 sector, u32 bytes) {
    d->req->type = type;
    d->req->reserved = 0;
    d->req->sector = sector;
    d->req->status = 0xFF;
    virtio::Desc* desc = d->queue.desc;
    desc[0] = {d->req_phys, 16, virtio::DESC_NEXT, 1};
    u16 last = 1;
    if (bytes) {
        desc[1] = {d->bounce_phys, bytes, (u16)(virtio::DESC_NEXT | (type == T_IN ? virtio::DESC_WRITE : 0)), 2};
        last = 2;
    }
    desc[last] = {d->req_phys + 16, 1, virtio::DESC_WRITE, 0};
    desc[0].next = bytes ? 1 : last;
    virtio::submit(&d->queue, 0);
    u64 start = refclock_now_us();
    u16 head;
    while (!virtio::poll_used(&d->queue, &head)) {
        if (refclock_now_us() - start > TIMEOUT_US) return Error::IO;
        asm volatile("pause");
    }
    if (head != 0 || d->req->status != 0) return Error::IO;
    return {};
}

Result<void> vblk_read(BlockDevice* bd, u64 lba, u32 count, void* buf) {
    Disk* d = (Disk*)bd->drv;
    MutexGuard g(d->lock);
    Result<void> r = request(d, T_IN, lba, count * 512);
    if (r.ok()) memcpy(buf, d->bounce, (usize)count * 512);
    return r;
}

Result<void> vblk_write(BlockDevice* bd, u64 lba, u32 count, const void* buf) {
    Disk* d = (Disk*)bd->drv;
    MutexGuard g(d->lock);
    memcpy(d->bounce, buf, (usize)count * 512);
    return request(d, T_OUT, lba, count * 512);
}

Result<void> vblk_flush(BlockDevice* bd) {
    Disk* d = (Disk*)bd->drv;
    if (!(d->vdev.features & F_FLUSH)) return {};
    MutexGuard g(d->lock);
    return request(d, T_FLUSH, 0, 0);
}

bool probe(const PciDevice* p) {
    // 0x1001: transitional block device; 0x1042: modern block device.
    return p->vendor == virtio::VENDOR && (p->device == 0x1001 || p->device == 0x1042);
}

Result<void> attach(const PciDevice* p) {
    Disk* d = (Disk*)kzalloc(sizeof(Disk));
    if (!d) return Error::NoMemory;
    Result<void> r = virtio::init(&d->vdev, p, F_FLUSH);
    if (r.ok() && d->vdev.device_cfg_len < 8) r = Error::NoDevice;
    if (r.ok()) r = virtio::queue_setup(&d->vdev, &d->queue, 0);
    if (r.ok() && d->queue.size < 4) r = Error::NoDevice;
    paddr_t req = PMM_NO_MEMORY, bounce = PMM_NO_MEMORY;
    if (r.ok()) {
        req = pmm_alloc_zeroed(1);
        bounce = pmm_alloc(BOUNCE_PAGES);
        if (req == PMM_NO_MEMORY || bounce == PMM_NO_MEMORY) r = Error::NoMemory;
    }
    if (!r.ok()) {
        if (d->vdev.common) virtio::fail(&d->vdev);
        kfree(d);
        return r;
    }
    d->req = (Request*)hhdm_virt(req);
    d->req_phys = req;
    d->bounce = (u8*)hhdm_virt(bounce);
    d->bounce_phys = bounce;
    virtio::driver_ok(&d->vdev);
    u64 sectors = *(volatile u64*)d->vdev.device_cfg;       // capacity in 512-byte sectors
    if (sectors == 0 || sectors > (1ull << 48)) {
        virtio::fail(&d->vdev);
        kfree(d);
        return Error::NoDevice;
    }
    strlcpy(d->dev.model, "virtio disk", sizeof d->dev.model);
    d->dev.sector_size = 512;
    d->dev.sectors = sectors;
    d->dev.read = vblk_read;
    d->dev.write = vblk_write;
    d->dev.flush = vblk_flush;
    d->dev.drv = d;
    d->dev.max_sectors = BOUNCE_PAGES * PAGE_SIZE / 512;
    Result<u32> minor = block_register(&d->dev);
    if (!minor.ok()) {
        virtio::fail(&d->vdev);
        kfree(d);
        return minor.error();
    }
    kprintf("virtio-blk: %s: %lu MiB (polled)\n", d->dev.name, (unsigned long)(sectors / 2048));
    return {};
}

Driver g_driver = {"virtio-blk", "virtio disks", DriverBus::Pci, 0x01, 0x00, 0xFF, probe, attach, nullptr, 0, nullptr};

} // namespace

void virtio_blk_register() { driver_register(&g_driver); }
