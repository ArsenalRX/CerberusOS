// See ata.h.
#include <arch/x86_64/io.h>
#include <drivers/ata.h>
#include <drivers/refclock.h>
#include <fs/block.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <sched/sync.h>

namespace {

// Register offsets from the command block base.
constexpr u16 REG_DATA = 0, REG_ERROR = 1, REG_COUNT = 2, REG_LBA0 = 3, REG_LBA1 = 4, REG_LBA2 = 5, REG_DRIVE = 6,
              REG_STATUS = 7, REG_COMMAND = 7;
constexpr u8 ST_ERR = 1 << 0, ST_DRQ = 1 << 3, ST_DF = 1 << 5, ST_BSY = 1 << 7;
constexpr u8 CMD_READ_EXT = 0x24, CMD_WRITE_EXT = 0x34, CMD_READ = 0x20, CMD_WRITE = 0x30, CMD_FLUSH = 0xE7,
             CMD_FLUSH_EXT = 0xEA, CMD_IDENTIFY = 0xEC;
constexpr u8 CTRL_NIEN = 1 << 1;
constexpr u64 TIMEOUT_US = 5000000;

struct Channel {
    u16 base, ctrl;
    Mutex lock;
};
Channel g_channels[2] = {{0x1F0, 0x3F6, {}}, {0x170, 0x376, {}}};

struct Disk {
    Channel* ch;
    bool slave;
    bool lba48;
    BlockDevice dev;
};

void delay_400ns(Channel* ch) {
    for (int i = 0; i < 4; i++) (void)inb(ch->ctrl);
}

// Waits until BSY clears; with `drq`, also until DRQ sets. IO on error,
// device fault or timeout.
Result<void> wait_ready(Channel* ch, bool drq) {
    u64 start = refclock_now_us();
    for (;;) {
        u8 st = inb(ch->base + REG_STATUS);
        if (!(st & ST_BSY)) {
            if (st & (ST_ERR | ST_DF)) return Error::IO;
            if (!drq || (st & ST_DRQ)) return {};
        }
        if (refclock_now_us() - start > TIMEOUT_US) return Error::IO;
        asm volatile("pause");
    }
}

void select(Disk* d, u64 lba, bool lba_mode) {
    u8 v = 0xA0 | (lba_mode ? 0x40 : 0) | (d->slave ? 0x10 : 0);
    if (lba_mode && !d->lba48) v |= (u8)((lba >> 24) & 0x0F);
    outb(d->ch->base + REG_DRIVE, v);
    delay_400ns(d->ch);
}

void set_address(Disk* d, u64 lba, u32 count) {
    u16 b = d->ch->base;
    if (d->lba48) {
        outb(b + REG_COUNT, (u8)(count >> 8));
        outb(b + REG_LBA0, (u8)(lba >> 24));
        outb(b + REG_LBA1, (u8)(lba >> 32));
        outb(b + REG_LBA2, (u8)(lba >> 40));
    }
    outb(b + REG_COUNT, (u8)count);
    outb(b + REG_LBA0, (u8)lba);
    outb(b + REG_LBA1, (u8)(lba >> 8));
    outb(b + REG_LBA2, (u8)(lba >> 16));
}

Result<void> ata_read(BlockDevice* bd, u64 lba, u32 count, void* buf) {
    Disk* d = (Disk*)bd->drv;
    MutexGuard g(d->ch->lock);
    select(d, lba, true);
    Result<void> r = wait_ready(d->ch, false);
    if (!r.ok()) return r;
    set_address(d, lba, count);
    outb(d->ch->base + REG_COMMAND, d->lba48 ? CMD_READ_EXT : CMD_READ);
    u8* p = (u8*)buf;
    for (u32 i = 0; i < count; i++) {
        delay_400ns(d->ch);
        r = wait_ready(d->ch, true);
        if (!r.ok()) return r;
        insw(d->ch->base + REG_DATA, p, 256);
        p += 512;
    }
    return {};
}

Result<void> ata_write(BlockDevice* bd, u64 lba, u32 count, const void* buf) {
    Disk* d = (Disk*)bd->drv;
    MutexGuard g(d->ch->lock);
    select(d, lba, true);
    Result<void> r = wait_ready(d->ch, false);
    if (!r.ok()) return r;
    set_address(d, lba, count);
    outb(d->ch->base + REG_COMMAND, d->lba48 ? CMD_WRITE_EXT : CMD_WRITE);
    const u8* p = (const u8*)buf;
    for (u32 i = 0; i < count; i++) {
        delay_400ns(d->ch);
        r = wait_ready(d->ch, true);
        if (!r.ok()) return r;
        outsw(d->ch->base + REG_DATA, p, 256);
        p += 512;
    }
    delay_400ns(d->ch);
    return wait_ready(d->ch, false);
}

Result<void> ata_flush(BlockDevice* bd) {
    Disk* d = (Disk*)bd->drv;
    MutexGuard g(d->ch->lock);
    select(d, 0, true);
    Result<void> r = wait_ready(d->ch, false);
    if (!r.ok()) return r;
    outb(d->ch->base + REG_COMMAND, d->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
    delay_400ns(d->ch);
    return wait_ready(d->ch, false);
}

void probe(Channel* ch, bool slave) {
    if (inb(ch->base + REG_STATUS) == 0xFF) return;         // floating bus: no controller
    outb(ch->ctrl, CTRL_NIEN);                              // polled: no interrupts
    outb(ch->base + REG_DRIVE, 0xA0 | (slave ? 0x10 : 0));
    delay_400ns(ch);
    outb(ch->base + REG_COUNT, 0);
    outb(ch->base + REG_LBA0, 0);
    outb(ch->base + REG_LBA1, 0);
    outb(ch->base + REG_LBA2, 0);
    outb(ch->base + REG_COMMAND, CMD_IDENTIFY);
    delay_400ns(ch);
    if (inb(ch->base + REG_STATUS) == 0) return;            // no device
    u64 start = refclock_now_us();
    while (inb(ch->base + REG_STATUS) & ST_BSY)
        if (refclock_now_us() - start > 1000000) return;
    // ATAPI and SATA-bridge devices put a signature here and abort IDENTIFY.
    if (inb(ch->base + REG_LBA1) || inb(ch->base + REG_LBA2)) return;
    if (!wait_ready(ch, true).ok()) return;
    u16 id[256];
    insw(ch->base + REG_DATA, id, 256);

    bool lba48 = id[83] & (1 << 10);
    u64 sectors = lba48 ? (u64)id[100] | (u64)id[101] << 16 | (u64)id[102] << 32 | (u64)id[103] << 48
                        : (u64)id[60] | (u64)id[61] << 16;
    if (!(id[49] & (1 << 9)) || sectors == 0) return;        // no LBA addressing: not supported
    if (sectors > (1ull << 48)) return;                      // nonsense from the device

    Disk* d = (Disk*)kzalloc(sizeof(Disk));
    if (!d) return;
    d->ch = ch;
    d->slave = slave;
    d->lba48 = lba48;
    // Model: words 27-46, two characters per word, high byte first.
    for (int i = 0; i < 20; i++) {
        char a = (char)(id[27 + i] >> 8), b = (char)(id[27 + i] & 0xFF);
        d->dev.model[2 * i] = (a >= 0x20 && a < 0x7F) ? a : ' ';
        d->dev.model[2 * i + 1] = (b >= 0x20 && b < 0x7F) ? b : ' ';
    }
    for (int i = 39; i >= 0 && d->dev.model[i] == ' '; i--) d->dev.model[i] = 0;
    d->dev.sector_size = 512;
    d->dev.sectors = sectors;
    d->dev.read = ata_read;
    d->dev.write = ata_write;
    d->dev.flush = ata_flush;
    d->dev.drv = d;
    d->dev.max_sectors = lba48 ? 256 : 255;
    Result<u32> minor = block_register(&d->dev);
    if (!minor.ok()) {
        kfree(d);
        return;
    }
    kprintf("ata: %s: %s %s, %lu MiB (%s), \"%s\"\n", d->dev.name, ch->base == 0x1F0 ? "primary" : "secondary",
            slave ? "slave" : "master", (unsigned long)(sectors / 2048), lba48 ? "LBA48" : "LBA28", d->dev.model);
}

} // namespace

void ata_init() {
    for (Channel& ch : g_channels) {
        probe(&ch, false);
        probe(&ch, true);
    }
    if (!block_count()) kprintf("ata: no disks\n");
}
