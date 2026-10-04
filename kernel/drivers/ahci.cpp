// See ahci.h. Register layout from the AHCI 1.3.1 specification.
#include <drivers/ahci.h>
#include <drivers/pci.h>
#include <drivers/refclock.h>
#include <fs/block.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/early_map.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <sched/sync.h>

namespace {

// HBA (global) registers.
constexpr u32 HBA_CAP = 0x00, HBA_GHC = 0x04, HBA_PI = 0x0C;
constexpr u32 GHC_AE = 1u << 31;
constexpr u32 CAP_S64A = 1u << 31;
// Port registers, at 0x100 + port * 0x80.
constexpr u32 P_CLB = 0x00, P_CLBU = 0x04, P_FB = 0x08, P_FBU = 0x0C, P_IS = 0x10, P_IE = 0x14, P_CMD = 0x18,
              P_TFD = 0x20, P_SIG = 0x24, P_SSTS = 0x28, P_SERR = 0x30, P_CI = 0x38;
constexpr u32 CMD_ST = 1u << 0, CMD_FRE = 1u << 4, CMD_FR = 1u << 14, CMD_CR = 1u << 15;
constexpr u32 TFD_ERR = 1u << 0, TFD_DRQ = 1u << 3, TFD_BSY = 1u << 7;
constexpr u32 IS_TFES = 1u << 30;
constexpr u32 SIG_ATA = 0x00000101;

constexpr u8 FIS_REG_H2D = 0x27;
constexpr u8 ATA_READ_DMA_EXT = 0x25, ATA_WRITE_DMA_EXT = 0x35, ATA_FLUSH_EXT = 0xEA, ATA_IDENTIFY = 0xEC;

constexpr u32 BOUNCE_SECTORS = 128;             // 64 KiB per command
constexpr u64 TIMEOUT_US = 5000000;

struct CmdHeader {                              // command list entry, 32 bytes
    u16 flags;                                  // CFL (FIS length in dwords) | W (write) bit 6
    u16 prdtl;                                  // PRDT entries
    volatile u32 prdbc;                         // bytes transferred
    u32 ctba, ctbau;                            // command table address
    u32 reserved[4];
};

struct Prd {
    u32 dba, dbau;
    u32 reserved;
    u32 dbc;                                    // byte count - 1, bit 31 = interrupt
};

struct CmdTable {
    u8 cfis[64];
    u8 acmd[16];
    u8 reserved[48];
    Prd prdt[1];
};

struct Port {
    volatile u8* regs;
    CmdHeader* list;                            // 1 KiB, in its own frame
    CmdTable* table;
    paddr_t table_phys;
    u8* bounce;
    paddr_t bounce_phys;
    Mutex lock;
    BlockDevice dev;
};

inline u32 rd(volatile u8* base, u32 off) { return *(volatile u32*)(base + off); }
inline void wr(volatile u8* base, u32 off, u32 v) { *(volatile u32*)(base + off) = v; }

bool wait_clear(volatile u8* regs, u32 off, u32 bits) {
    u64 start = refclock_now_us();
    while (rd(regs, off) & bits)
        if (refclock_now_us() - start > TIMEOUT_US) return false;
    return true;
}

bool stop_port(volatile u8* regs) {
    wr(regs, P_CMD, rd(regs, P_CMD) & ~CMD_ST);
    if (!wait_clear(regs, P_CMD, CMD_CR)) return false;
    wr(regs, P_CMD, rd(regs, P_CMD) & ~CMD_FRE);
    return wait_clear(regs, P_CMD, CMD_FR);
}

// Issues one command in slot 0 and waits for it. `bytes` of data move
// through the bounce buffer (direction given by `write`).
Result<void> issue(Port* p, u8 command, u64 lba, u32 count, u32 bytes, bool write) {
    volatile u8* r = p->regs;
    if (!wait_clear(r, P_TFD, TFD_BSY | TFD_DRQ)) return Error::IO;
    CmdHeader* h = &p->list[0];
    h->flags = (u16)(5 | (write ? 1 << 6 : 0));     // a 5-dword register FIS
    h->prdtl = bytes ? 1 : 0;
    h->prdbc = 0;
    memset(p->table, 0, sizeof(CmdTable));
    u8* f = p->table->cfis;
    f[0] = FIS_REG_H2D;
    f[1] = 1 << 7;                                  // command, not control
    f[2] = command;
    f[4] = (u8)lba;
    f[5] = (u8)(lba >> 8);
    f[6] = (u8)(lba >> 16);
    f[7] = 1 << 6;                                  // LBA mode
    f[8] = (u8)(lba >> 24);
    f[9] = (u8)(lba >> 32);
    f[10] = (u8)(lba >> 40);
    f[12] = (u8)count;
    f[13] = (u8)(count >> 8);
    if (bytes) {
        p->table->prdt[0].dba = (u32)p->bounce_phys;
        p->table->prdt[0].dbau = (u32)(p->bounce_phys >> 32);
        p->table->prdt[0].dbc = bytes - 1;
    }
    wr(r, P_IS, 0xFFFFFFFF);                         // clear old status
    asm volatile("mfence" ::: "memory");
    wr(r, P_CI, 1);
    u64 start = refclock_now_us();
    for (;;) {
        if (rd(r, P_IS) & IS_TFES) return Error::IO;
        if (!(rd(r, P_CI) & 1)) break;
        if (refclock_now_us() - start > TIMEOUT_US) return Error::IO;
        asm volatile("pause");
    }
    if (rd(r, P_TFD) & TFD_ERR) return Error::IO;
    return {};
}

Result<void> ahci_read(BlockDevice* bd, u64 lba, u32 count, void* buf) {
    Port* p = (Port*)bd->drv;
    MutexGuard g(p->lock);
    Result<void> r = issue(p, ATA_READ_DMA_EXT, lba, count, count * 512, false);
    if (r.ok()) memcpy(buf, p->bounce, (usize)count * 512);
    return r;
}

Result<void> ahci_write(BlockDevice* bd, u64 lba, u32 count, const void* buf) {
    Port* p = (Port*)bd->drv;
    MutexGuard g(p->lock);
    memcpy(p->bounce, buf, (usize)count * 512);
    return issue(p, ATA_WRITE_DMA_EXT, lba, count, count * 512, true);
}

Result<void> ahci_flush(BlockDevice* bd) {
    Port* p = (Port*)bd->drv;
    MutexGuard g(p->lock);
    return issue(p, ATA_FLUSH_EXT, 0, 0, 0, false);
}

void setup_port(volatile u8* hba, u32 index, bool s64a) {
    volatile u8* r = hba + 0x100 + index * 0x80;
    u32 ssts = rd(r, P_SSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) return;     // no device, or not active
    if (rd(r, P_SIG) != SIG_ATA) return;                          // ATAPI (CD), port multiplier...
    if (!stop_port(r)) return;

    // Command list + FIS area in one frame; the command table in another;
    // the bounce buffer contiguous. Below 4 GiB unless 64-bit DMA works.
    paddr_t base = pmm_alloc_zeroed(1);
    paddr_t table = pmm_alloc_zeroed(1);
    paddr_t bounce = pmm_alloc(BOUNCE_SECTORS * 512 / PAGE_SIZE);
    if (base == PMM_NO_MEMORY || table == PMM_NO_MEMORY || bounce == PMM_NO_MEMORY) return;
    if (!s64a && (base >> 32 || table >> 32 || (bounce + BOUNCE_SECTORS * 512) >> 32)) {
        kprintf("ahci: port %u: memory above 4 GiB and no 64-bit DMA; port not used\n", index);
        return;
    }
    Port* p = (Port*)kzalloc(sizeof(Port));
    if (!p) return;
    p->regs = r;
    p->list = (CmdHeader*)hhdm_virt(base);
    p->table = (CmdTable*)hhdm_virt(table);
    p->table_phys = table;
    p->bounce = (u8*)hhdm_virt(bounce);
    p->bounce_phys = bounce;
    p->list[0].ctba = (u32)table;
    p->list[0].ctbau = (u32)(table >> 32);
    paddr_t fis = base + 1024;
    wr(r, P_CLB, (u32)base);
    wr(r, P_CLBU, (u32)(base >> 32));
    wr(r, P_FB, (u32)fis);
    wr(r, P_FBU, (u32)(fis >> 32));
    wr(r, P_SERR, 0xFFFFFFFF);
    wr(r, P_IS, 0xFFFFFFFF);
    wr(r, P_IE, 0);                                     // polled
    wr(r, P_CMD, rd(r, P_CMD) | CMD_FRE);
    wr(r, P_CMD, rd(r, P_CMD) | CMD_ST);

    if (!issue(p, ATA_IDENTIFY, 0, 0, 512, false).ok()) {
        kprintf("ahci: port %u: IDENTIFY failed\n", index);
        kfree(p);
        return;
    }
    const u16* id = (const u16*)p->bounce;
    bool lba48 = id[83] & (1 << 10);
    u64 sectors = lba48 ? (u64)id[100] | (u64)id[101] << 16 | (u64)id[102] << 32 | (u64)id[103] << 48
                        : (u64)id[60] | (u64)id[61] << 16;
    if (!lba48 || sectors == 0 || sectors > (1ull << 48)) {
        kprintf("ahci: port %u: disk without 48-bit addressing; not used\n", index);
        kfree(p);
        return;
    }
    for (int i = 0; i < 20; i++) {
        char a = (char)(id[27 + i] >> 8), b = (char)(id[27 + i] & 0xFF);
        p->dev.model[2 * i] = (a >= 0x20 && a < 0x7F) ? a : ' ';
        p->dev.model[2 * i + 1] = (b >= 0x20 && b < 0x7F) ? b : ' ';
    }
    for (int i = 39; i >= 0 && p->dev.model[i] == ' '; i--) p->dev.model[i] = 0;
    p->dev.sector_size = 512;
    p->dev.sectors = sectors;
    p->dev.read = ahci_read;
    p->dev.write = ahci_write;
    p->dev.flush = ahci_flush;
    p->dev.drv = p;
    p->dev.max_sectors = BOUNCE_SECTORS;
    Result<u32> minor = block_register(&p->dev);
    if (!minor.ok()) {
        kfree(p);
        return;
    }
    kprintf("ahci: %s: port %u, %lu MiB, \"%s\"\n", p->dev.name, index, (unsigned long)(sectors / 2048), p->dev.model);
}

} // namespace

void ahci_init() {
    for (u32 n = 0;; n++) {
        const PciDevice* d = pci_find_class(0x01, 0x06, 0x01, n);
        if (!d) break;
        u64 abar = pci_bar_address(*d, 5);
        if (!abar) continue;
        pci_enable_dma(*d);
        volatile u8* hba = (volatile u8*)early_map(abar, 0x1100, MapCache::Uncached);
        wr(hba, HBA_GHC, rd(hba, HBA_GHC) | GHC_AE);
        u32 cap = rd(hba, HBA_CAP);
        u32 pi = rd(hba, HBA_PI);
        for (u32 port = 0; port < 32; port++)
            if (pi & (1u << port)) setup_port(hba, port, cap & CAP_S64A);
    }
}
