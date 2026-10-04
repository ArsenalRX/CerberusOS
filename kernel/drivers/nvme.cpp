// NVMe disks (the storage of most current PCs; QEMU `-device nvme`). One
// admin queue and one I/O queue, one command at a time, polled. Namespace 1
// only, 512-byte blocks only. Register layout and commands from the NVMe
// 1.4 specification.
//
// Everything the controller reports (capabilities, sizes, completion
// entries) is checked before use; a controller that does not answer within
// its own stated timeout is given up on.
#include <drivers/driver.h>
#include <drivers/nvme.h>
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

constexpr u32 REG_CAP = 0x00, REG_CC = 0x14, REG_CSTS = 0x1C, REG_AQA = 0x24, REG_ASQ = 0x28, REG_ACQ = 0x30;
constexpr u32 CC_EN = 1, CSTS_RDY = 1, CSTS_CFS = 2;
constexpr u8 ADMIN_CREATE_SQ = 0x01, ADMIN_CREATE_CQ = 0x05, ADMIN_IDENTIFY = 0x06;
constexpr u8 IO_FLUSH = 0x00, IO_WRITE = 0x01, IO_READ = 0x02;
constexpr u32 QUEUE_ENTRIES = 16;
constexpr u32 BOUNCE_PAGES = 16;                // 64 KiB per command
constexpr u64 COMMAND_TIMEOUT_US = 5000000;

struct Command {                                // 64 bytes
    u32 cdw0;                                   // opcode | command id << 16
    u32 nsid;
    u64 reserved;
    u64 mptr;
    u64 prp1, prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
static_assert(sizeof(Command) == 64, "NVMe command size");

struct Completion {                             // 16 bytes
    u32 result;
    u32 reserved;
    u16 sq_head, sq_id;
    u16 command_id;
    volatile u16 status;                        // bit 0: phase; bits 1-15: status
};
static_assert(sizeof(Completion) == 16, "NVMe completion size");

struct Queue {
    Command* sq;
    Completion* cq;
    paddr_t sq_phys, cq_phys;
    u16 tail, head;
    u16 phase;
    u16 id;
    u16 next_cid;
};

struct Controller {
    volatile u8* regs;
    u32 doorbell_stride;                        // bytes between doorbells
    Queue admin, io;
    u8* bounce;
    paddr_t bounce_phys;
    u64* prp_list;
    paddr_t prp_list_phys;
    Mutex lock;
    BlockDevice dev;
};

inline u32 rd32(Controller* c, u32 off) { return *(volatile u32*)(c->regs + off); }
inline u64 rd64(Controller* c, u32 off) { return *(volatile u64*)(c->regs + off); }
inline void wr32(Controller* c, u32 off, u32 v) { *(volatile u32*)(c->regs + off) = v; }
inline void wr64(Controller* c, u32 off, u64 v) { *(volatile u64*)(c->regs + off) = v; }

void doorbell(Controller* c, u16 qid, bool completion, u16 value) {
    wr32(c, 0x1000 + (2 * qid + (completion ? 1 : 0)) * c->doorbell_stride, value);
}

bool wait_ready(Controller* c, bool ready, u64 timeout_us) {
    u64 start = refclock_now_us();
    for (;;) {
        u32 csts = rd32(c, REG_CSTS);
        if (csts & CSTS_CFS) return false;
        if (((csts & CSTS_RDY) != 0) == ready) return true;
        if (refclock_now_us() - start > timeout_us) return false;
        asm volatile("pause");
    }
}

bool queue_alloc(Queue* q, u16 id) {
    q->sq_phys = pmm_alloc_zeroed(1);
    q->cq_phys = pmm_alloc_zeroed(1);
    if (q->sq_phys == PMM_NO_MEMORY || q->cq_phys == PMM_NO_MEMORY) return false;
    q->sq = (Command*)hhdm_virt(q->sq_phys);
    q->cq = (Completion*)hhdm_virt(q->cq_phys);
    q->id = id;
    q->phase = 1;
    return true;
}

// Submits one command and waits for its completion.
Result<u32> submit(Controller* c, Queue* q, Command cmd) {
    u16 cid = q->next_cid++;
    cmd.cdw0 = (cmd.cdw0 & 0xFFFF) | (u32)cid << 16;
    q->sq[q->tail] = cmd;
    q->tail = (u16)((q->tail + 1) % QUEUE_ENTRIES);
    asm volatile("mfence" ::: "memory");
    doorbell(c, q->id, false, q->tail);
    u64 start = refclock_now_us();
    for (;;) {
        Completion* e = &q->cq[q->head];
        u16 status = e->status;
        if ((status & 1) == q->phase) {
            u16 got = e->command_id;
            u32 result = e->result;
            q->head = (u16)((q->head + 1) % QUEUE_ENTRIES);
            if (q->head == 0) q->phase ^= 1;
            doorbell(c, q->id, true, q->head);
            if (got != cid) return Error::IO;            // not the command we sent
            if (status >> 1) return Error::IO;
            return result;
        }
        if (refclock_now_us() - start > COMMAND_TIMEOUT_US) return Error::IO;
        asm volatile("pause");
    }
}

Result<void> rw(Controller* c, u8 opcode, u64 lba, u32 count) {
    Command cmd{};
    cmd.cdw0 = opcode;
    cmd.nsid = 1;
    u32 bytes = count * 512;
    cmd.prp1 = c->bounce_phys;
    if (bytes > 2 * PAGE_SIZE) cmd.prp2 = c->prp_list_phys;       // a list of the pages after the first
    else if (bytes > PAGE_SIZE) cmd.prp2 = c->bounce_phys + PAGE_SIZE;
    cmd.cdw10 = (u32)lba;
    cmd.cdw11 = (u32)(lba >> 32);
    cmd.cdw12 = count - 1;
    Result<u32> r = submit(c, &c->io, cmd);
    return r.ok() ? Result<void>() : Result<void>(r.error());
}

Result<void> nvme_read(BlockDevice* bd, u64 lba, u32 count, void* buf) {
    Controller* c = (Controller*)bd->drv;
    MutexGuard g(c->lock);
    Result<void> r = rw(c, IO_READ, lba, count);
    if (r.ok()) memcpy(buf, c->bounce, (usize)count * 512);
    return r;
}

Result<void> nvme_write(BlockDevice* bd, u64 lba, u32 count, const void* buf) {
    Controller* c = (Controller*)bd->drv;
    MutexGuard g(c->lock);
    memcpy(c->bounce, buf, (usize)count * 512);
    return rw(c, IO_WRITE, lba, count);
}

Result<void> nvme_flush(BlockDevice* bd) {
    Controller* c = (Controller*)bd->drv;
    MutexGuard g(c->lock);
    Command cmd{};
    cmd.cdw0 = IO_FLUSH;
    cmd.nsid = 1;
    Result<u32> r = submit(c, &c->io, cmd);
    return r.ok() ? Result<void>() : Result<void>(r.error());
}

Result<void> attach(const PciDevice* d) {
    u64 bar = pci_bar_address(*d, 0);
    if (!bar) return Error::NoDevice;
    pci_enable_dma(*d);
    Controller* c = (Controller*)kzalloc(sizeof(Controller));
    if (!c) return Error::NoMemory;
    c->regs = (volatile u8*)early_map(bar, 0x2000, MapCache::Uncached);
    u64 cap = rd64(c, REG_CAP);
    u32 max_entries = (u32)(cap & 0xFFFF) + 1;
    u32 dstrd = (u32)(cap >> 32) & 0xF;
    u32 mpsmin = (u32)(cap >> 48) & 0xF;
    u64 timeout_us = ((cap >> 24) & 0xFF) * 500000ull;
    if (timeout_us < 1000000) timeout_us = 1000000;
    // The doorbells for queues 0 and 1 must lie inside the 8 KiB we mapped.
    if (max_entries < QUEUE_ENTRIES || mpsmin != 0 || dstrd > 7) {
        kprintf("nvme: controller not supported (queue %u entries, page shift %u, doorbell stride %u)\n", max_entries,
                12 + mpsmin, dstrd);
        kfree(c);
        return Error::NotSupported;
    }
    c->doorbell_stride = 4u << dstrd;

    auto fail = [&](const char* why) -> Result<void> {
        kprintf("nvme: %s\n", why);
        wr32(c, REG_CC, 0);
        kfree(c);
        return Error::IO;
    };
    wr32(c, REG_CC, rd32(c, REG_CC) & ~CC_EN);
    if (!wait_ready(c, false, timeout_us)) return fail("controller did not stop");
    if (!queue_alloc(&c->admin, 0) || !queue_alloc(&c->io, 1)) return fail("no memory for the queues");
    paddr_t bounce = pmm_alloc(BOUNCE_PAGES);
    c->prp_list_phys = pmm_alloc_zeroed(1);
    if (bounce == PMM_NO_MEMORY || c->prp_list_phys == PMM_NO_MEMORY) return fail("no memory for the buffers");
    c->bounce = (u8*)hhdm_virt(bounce);
    c->bounce_phys = bounce;
    c->prp_list = (u64*)hhdm_virt(c->prp_list_phys);
    for (u32 i = 1; i < BOUNCE_PAGES; i++) c->prp_list[i - 1] = bounce + (u64)i * PAGE_SIZE;

    wr32(c, REG_AQA, (QUEUE_ENTRIES - 1) << 16 | (QUEUE_ENTRIES - 1));
    wr64(c, REG_ASQ, c->admin.sq_phys);
    wr64(c, REG_ACQ, c->admin.cq_phys);
    // 4 KiB pages, NVM command set, 64-byte submission and 16-byte completion entries.
    wr32(c, REG_CC, CC_EN | 6u << 16 | 4u << 20);
    if (!wait_ready(c, true, timeout_us)) return fail("controller did not start");

    // Identify the controller (model, largest transfer), then namespace 1.
    Command id{};
    id.cdw0 = ADMIN_IDENTIFY;
    id.prp1 = c->bounce_phys;
    id.cdw10 = 1;
    if (!submit(c, &c->admin, id).ok()) return fail("IDENTIFY (controller) failed");
    for (int i = 0; i < 40; i++) {
        char ch = (char)c->bounce[24 + i];
        c->dev.model[i] = (ch >= 0x20 && ch < 0x7F) ? ch : ' ';
    }
    for (int i = 39; i >= 0 && c->dev.model[i] == ' '; i--) c->dev.model[i] = 0;
    u8 mdts = c->bounce[77];
    u32 max_sectors = BOUNCE_PAGES * PAGE_SIZE / 512;
    if (mdts && mdts < 5) max_sectors = (1u << mdts) * PAGE_SIZE / 512;

    id.nsid = 1;
    id.cdw10 = 0;
    if (!submit(c, &c->admin, id).ok()) return fail("IDENTIFY (namespace 1) failed");
    u64 blocks;
    memcpy(&blocks, c->bounce, 8);
    u8 format = c->bounce[26] & 0xF;
    u32 lbaf;
    memcpy(&lbaf, c->bounce + 128 + 4 * format, 4);
    u32 block_shift = (lbaf >> 16) & 0xFF;
    if (blocks == 0 || blocks > (1ull << 48)) return fail("namespace 1 is empty or absurdly large");
    if (block_shift != 9) return fail("only 512-byte blocks are supported");

    Command cq{};
    cq.cdw0 = ADMIN_CREATE_CQ;
    cq.prp1 = c->io.cq_phys;
    cq.cdw10 = (QUEUE_ENTRIES - 1) << 16 | 1;
    cq.cdw11 = 1;                                   // physically contiguous, no interrupts
    if (!submit(c, &c->admin, cq).ok()) return fail("could not create the I/O completion queue");
    Command sq{};
    sq.cdw0 = ADMIN_CREATE_SQ;
    sq.prp1 = c->io.sq_phys;
    sq.cdw10 = (QUEUE_ENTRIES - 1) << 16 | 1;
    sq.cdw11 = 1u << 16 | 1;                        // completion queue 1, contiguous
    if (!submit(c, &c->admin, sq).ok()) return fail("could not create the I/O submission queue");

    c->dev.sector_size = 512;
    c->dev.sectors = blocks;
    c->dev.read = nvme_read;
    c->dev.write = nvme_write;
    c->dev.flush = nvme_flush;
    c->dev.drv = c;
    c->dev.max_sectors = max_sectors;
    Result<u32> minor = block_register(&c->dev);
    if (!minor.ok()) return fail("no free disk slot");
    kprintf("nvme: %s: %lu MiB, \"%s\" (polled)\n", c->dev.name, (unsigned long)(blocks / 2048), c->dev.model);
    return {};
}

Driver g_driver = {"nvme", "NVMe disks", DriverBus::Pci, 0x01, 0x08, 0x02, nullptr, attach, nullptr, 0, nullptr};

} // namespace

void nvme_register() { driver_register(&g_driver); }
