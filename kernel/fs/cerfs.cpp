// See cerfs.h. All functions run with the VFS lock held.
#include <fs/block.h>
#include <fs/dev.h>
#include <fs/cerfs.h>
#include <fs/cerfs_format.h>
#include <fs/pagecache.h>
#include <fs/vfs.h>
#include <lib/csprng.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <sched/sched.h>

using namespace cerfs;

namespace {

constexpr u32 SECTORS = BLOCK / 512;
// Commit before an operation when fewer than this many transaction slots
// are left; no single step of an operation dirties more blocks.
constexpr u32 TX_RESERVE = 24;
// Closed files kept cached (with their pages) per mounted file system.
constexpr u32 MAX_INACTIVE = 256;

enum class Kind : u8 { Super, Bitmap, Dir, Indirect, Inodes };

struct TxEntry {
    Page* page;
    u64 block;
    Kind kind;
};

struct CerNode;

struct CerFs {
    Mount mount;
    Vnode* dev;                 // the block device node (a reference)
    u32 minor;
    SuperBlock sb;
    bool sb_dirty;
    TxEntry tx[JOURNAL_MAX_TX];
    u32 tx_count;
    u32 tx_cap;                 // blocks per transaction: what the journal holds, at most JOURNAL_MAX_TX
    u64 seq;                    // next journal sequence number
    CerNode* nodes;             // vnodes currently in use
    u64 alloc_hint;
    u32 inode_hint;
    bool broken;                // an I/O error: refuse further writes
    u32 inactive;               // cached vnodes with no references
};

struct CerNode {
    CerFs* fs;
    Vnode* v;
    u32 ino;
    Inode di;                   // the on-disk inode as last read or written
    CerNode* next;
};

extern const VnodeOps g_ops;
extern const PageIo g_file_io;

CerNode* node_of(Vnode* v) { return (CerNode*)v->fs_data; }
CerFs* fs_of(Vnode* v) { return node_of(v)->fs; }

Error corrupt(CerFs* fs, const char* what, u64 block) {
    kprintf("cerfs: %s: damaged %s (block %lu)\n", fs->mount.source, what, (unsigned long)block);
    return Error::IO;
}

// ------------------------------------------------------- metadata blocks --
Result<Page*> meta(CerFs* fs, u64 block) {
    if (block >= fs->sb.total_blocks) return corrupt(fs, "block reference", block);
    return page_get(fs->dev, block, block_page_io(), true);
}

// A metadata block about to be written from scratch (no read).
Result<Page*> meta_new(CerFs* fs, u64 block) {
    Result<Page*> p = page_get(fs->dev, block, block_page_io(), false);
    if (p.ok()) memset(p.value()->data, 0, BLOCK);
    return p;
}

Result<void> commit(CerFs* fs);

// Adds a changed metadata page to the running transaction. The transaction
// keeps its own page reference until the commit.
void meta_dirty(CerFs* fs, Page* p, u64 block, Kind kind) {
    page_mark_dirty(p);
    // Seal at once: the block is read (and checked) again before the commit.
    if (kind == Kind::Bitmap || kind == Kind::Dir || kind == Kind::Indirect) seal_block(p->data);
    for (u32 i = 0; i < fs->tx_count; i++)
        if (fs->tx[i].page == p) return;
    // The superblock always has its slot (tx_cap leaves one), so commit can
    // add it without recursing.
    if (fs->tx_count >= fs->tx_cap && kind != Kind::Super) {
        // Operations reserve room first (tx_reserve), so this means one step
        // dirtied more than expected: commit what there is rather than
        // overflow. The step's own changes go into the next transaction.
        kprintf("cerfs: %s: transaction full, committing early\n", fs->mount.source);
        (void)commit(fs);
    }
    page_hold(p, true);
    p->refs++;
    fs->tx[fs->tx_count++] = {p, block, kind};
}

Result<void> raw_write(CerFs* fs, u64 block, const void* data) {
    return block_dev_write(fs->minor, block * SECTORS, SECTORS, data);
}

Result<void> raw_read(CerFs* fs, u64 block, void* data) {
    return block_dev_read(fs->minor, block * SECTORS, SECTORS, data);
}

Result<void> write_journal_header(CerFs* fs) {
    u8* b = (u8*)kzalloc(BLOCK);
    if (!b) return Error::NoMemory;
    JournalHeader* h = (JournalHeader*)b;
    h->h.magic = MAGIC_JHEAD;
    h->sequence = fs->seq;
    seal_block(b);
    Result<void> r = raw_write(fs, fs->sb.journal_start, b);
    kfree(b);
    return r;
}

// ------------------------------------------------------------- commit ---
Result<void> commit(CerFs* fs) {
    if (fs->broken) return Error::IO;
    // Ordered data: file contents reach the disk before the metadata that
    // points at them.
    for (CerNode* n = fs->nodes; n; n = n->next) {
        Result<void> r = page_sync_owner(n->v);
        if (!r.ok()) {
            fs->broken = true;
            return r;
        }
    }
    if (fs->sb_dirty) {
        Result<Page*> p = meta(fs, 0);
        if (!p.ok()) return p.error();
        fs->sb.crc = super_crc(fs->sb);
        memset(p.value()->data, 0, BLOCK);
        memcpy(p.value()->data, &fs->sb, sizeof fs->sb);
        meta_dirty(fs, p.value(), 0, Kind::Super);
        page_put(p.value());
        fs->sb_dirty = false;
    }
    if (fs->tx_count == 0) return block_dev_flush(fs->minor);

    for (u32 i = 0; i < fs->tx_count; i++) {
        Kind k = fs->tx[i].kind;
        if (k == Kind::Bitmap || k == Kind::Dir || k == Kind::Indirect) seal_block(fs->tx[i].page->data);
    }
    // 1. Descriptor and images into the journal.
    JournalDesc* d = (JournalDesc*)kzalloc(BLOCK);
    if (!d) return Error::NoMemory;
    d->h.magic = MAGIC_JDESC;
    d->h.owner = (u32)fs->seq;
    d->h.extra = fs->tx_count;
    d->sequence = fs->seq;
    u32 images_crc = 0;
    Result<void> r;
    for (u32 i = 0; i < fs->tx_count; i++) {
        d->targets[i] = fs->tx[i].block;
        images_crc = crc32c(images_crc, fs->tx[i].page->data, BLOCK);
    }
    seal_block((u8*)d);
    u64 js = fs->sb.journal_start;
    r = raw_write(fs, js + 1, d);
    for (u32 i = 0; i < fs->tx_count && r.ok(); i++) r = raw_write(fs, js + 2 + i, fs->tx[i].page->data);
    if (r.ok()) r = block_dev_flush(fs->minor);
    // 2. The commit record: from here on the transaction counts.
    if (r.ok()) {
        memset(d, 0, BLOCK);
        JournalCommit* c = (JournalCommit*)d;
        c->h.magic = MAGIC_JCOMMIT;
        c->h.owner = (u32)fs->seq;
        c->h.extra = fs->tx_count;
        c->sequence = fs->seq;
        c->images_crc = images_crc;
        seal_block((u8*)c);
        r = raw_write(fs, js + 2 + fs->tx_count, c);
        if (r.ok()) r = block_dev_flush(fs->minor);
    }
    kfree(d);
    // 3. Checkpoint: the blocks to their homes.
    for (u32 i = 0; i < fs->tx_count; i++) {
        Page* p = fs->tx[i].page;
        page_hold(p, false);
        if (r.ok()) r = page_write(p);
        page_put(p);
    }
    fs->tx_count = 0;
    if (r.ok()) r = block_dev_flush(fs->minor);
    // 4. The journal is empty again.
    if (r.ok()) {
        fs->seq++;
        r = write_journal_header(fs);
        if (r.ok()) r = block_dev_flush(fs->minor);
    }
    if (!r.ok()) {
        fs->broken = true;
        kprintf("cerfs: %s: write failed during commit; the file system is now read-only\n", fs->mount.source);
    }
    return r;
}

Result<void> tx_reserve(CerFs* fs) {
    if (fs->broken) return Error::IO;
    if (fs->tx_count + TX_RESERVE <= fs->tx_cap) return {};
    return commit(fs);
}

// As tx_reserve, in the middle of an operation on `n`: the inode is written
// first, so the commit never records blocks the inode does not show.
Result<void> node_write(CerNode* n);
Result<void> tx_reserve_node(CerNode* n) {
    CerFs* fs = n->fs;
    if (fs->broken) return Error::IO;
    if (fs->tx_count + TX_RESERVE <= fs->tx_cap) return {};
    Result<void> r = node_write(n);
    return r.ok() ? commit(fs) : r;
}

// Replays a complete transaction left in the journal (mount, before any
// metadata is read through the cache).
Result<void> replay(CerFs* fs) {
    const SuperBlock& s = fs->sb;
    u8* b = (u8*)kmalloc(BLOCK);
    if (!b) return Error::NoMemory;
    Result<void> r = raw_read(fs, s.journal_start, b);
    if (!r.ok()) {
        kfree(b);
        return r;
    }
    JournalHeader* jh = (JournalHeader*)b;
    if (!block_ok(b, MAGIC_JHEAD, 0)) {
        kfree(b);
        return corrupt(fs, "journal header", s.journal_start);
    }
    fs->seq = jh->sequence;
    r = raw_read(fs, s.journal_start + 1, b);
    JournalDesc* d = (JournalDesc*)b;
    u32 count = d->h.extra;
    bool candidate = r.ok() && block_ok(b, MAGIC_JDESC, (u32)fs->seq) && d->sequence == fs->seq && count > 0 &&
                     count <= JOURNAL_MAX_TX && 2 + count < s.journal_blocks;
    if (!candidate) {
        kfree(b);
        return r;
    }
    // Copy the targets out and check each one before writing anything.
    u64* targets = (u64*)kmalloc(count * sizeof(u64));
    u8* images = (u8*)kmalloc((usize)count * BLOCK);
    u8* c = (u8*)kmalloc(BLOCK);
    bool valid = targets && images && c;
    for (u32 i = 0; valid && i < count; i++) {
        targets[i] = d->targets[i];
        if (targets[i] >= s.total_blocks || (targets[i] >= s.journal_start && targets[i] < s.data_start)) valid = false;
    }
    u32 crc = 0;
    for (u32 i = 0; valid && i < count; i++) {
        if (!raw_read(fs, s.journal_start + 2 + i, images + (usize)i * BLOCK).ok()) valid = false;
        else crc = crc32c(crc, images + (usize)i * BLOCK, BLOCK);
    }
    if (valid) {
        JournalCommit* cm = (JournalCommit*)c;
        valid = raw_read(fs, s.journal_start + 2 + count, c).ok() && block_ok(c, MAGIC_JCOMMIT, (u32)fs->seq) &&
                cm->sequence == fs->seq && cm->h.extra == count && cm->images_crc == crc;
    }
    if (valid) {
        for (u32 i = 0; i < count && r.ok(); i++) r = raw_write(fs, targets[i], images + (usize)i * BLOCK);
        if (r.ok()) r = block_dev_flush(fs->minor);
        if (r.ok()) {
            kprintf("cerfs: %s: replayed journal transaction %lu (%u blocks)\n", fs->mount.source,
                    (unsigned long)fs->seq, count);
            fs->seq++;
            r = write_journal_header(fs);
            if (r.ok()) r = block_dev_flush(fs->minor);
        }
    }
    kfree(targets);
    kfree(images);
    kfree(c);
    kfree(b);
    return r;
}

// --------------------------------------------------------------- inodes --
Result<void> inode_read(CerFs* fs, u32 ino, Inode* out) {
    if (ino == 0 || ino > fs->sb.inode_count) return corrupt(fs, "inode number", ino);
    u64 blk = inode_block(fs->sb, ino);
    Result<Page*> p = meta(fs, blk);
    if (!p.ok()) return p.error();
    memcpy(out, p.value()->data + inode_offset(ino), sizeof *out);
    page_put(p.value());
    if (!check_inode(fs->sb, *out)) return corrupt(fs, "inode", blk);
    return {};
}

Result<void> inode_write(CerFs* fs, u32 ino, Inode* in) {
    u64 blk = inode_block(fs->sb, ino);
    Result<Page*> p = meta(fs, blk);
    if (!p.ok()) return p.error();
    in->crc = inode_crc(*in);
    memcpy(p.value()->data + inode_offset(ino), in, sizeof *in);
    meta_dirty(fs, p.value(), blk, Kind::Inodes);
    page_put(p.value());
    return {};
}

// The vnode's attributes into the inode, and the inode to its block.
Result<void> node_write(CerNode* n) {
    Vnode* v = n->v;
    n->di.mode = (u16)((n->di.mode & T_MASK) | (v->mode & 07777));
    n->di.links = (u16)v->nlink;
    n->di.uid = v->uid;
    n->di.gid = v->gid;
    n->di.size = v->size;
    n->di.atime = v->atime;
    n->di.mtime = v->mtime;
    n->di.ctime = v->ctime;
    return inode_write(n->fs, n->ino, &n->di);
}

VType type_of(u16 mode) {
    switch (mode & T_MASK) {
    case T_DIR: return VType::Dir;
    case T_LINK: return VType::Symlink;
    default: return VType::File;
    }
}

u8 dirtype_of(VType t) { return t == VType::Dir ? REC_DIR : t == VType::Symlink ? REC_LINK : REC_FILE; }

// The vnode for inode `ino`, with a new reference.
Result<Vnode*> node_get(CerFs* fs, u32 ino) {
    for (CerNode* n = fs->nodes; n; n = n->next)
        if (n->ino == ino) {
            if (n->v->refs == 0) fs->inactive--;
            return vnode_ref(n->v);
        }
    Inode di;
    Result<void> r = inode_read(fs, ino, &di);
    if (!r.ok()) return r.error();
    if (di.mode == 0) return corrupt(fs, "reference to a free inode", ino);
    CerNode* n = (CerNode*)kzalloc(sizeof(CerNode));
    if (!n) return Error::NoMemory;
    Result<Vnode*> v = vnode_alloc(&g_ops, &fs->mount, type_of(di.mode));
    if (!v.ok()) {
        kfree(n);
        return v.error();
    }
    n->fs = fs;
    n->v = v.value();
    n->ino = ino;
    n->di = di;
    Vnode* vn = n->v;
    vn->fs_data = n;
    vn->ino = ino;
    vn->mode = di.mode & 07777;
    vn->uid = di.uid;
    vn->gid = di.gid;
    vn->nlink = di.links;
    vn->size = di.size;
    vn->atime = di.atime;
    vn->mtime = di.mtime;
    vn->ctime = di.ctime;
    n->next = fs->nodes;
    fs->nodes = n;
    return vn;
}

// -------------------------------------------------------------- bitmap ---
Result<u64> block_alloc(CerFs* fs) {
    SuperBlock& s = fs->sb;
    if (s.free_blocks == 0) return Error::NoSpace;
    u64 start = fs->alloc_hint >= s.data_start && fs->alloc_hint < s.total_blocks ? fs->alloc_hint : s.data_start;
    for (u64 scanned = 0; scanned < s.total_blocks;) {
        u64 b = (start + scanned) % s.total_blocks;
        if (b < s.data_start) {
            scanned += s.data_start - b;
            continue;
        }
        u32 bi = (u32)(b / BITS_PER_BITMAP_BLOCK);
        u64 bblock = s.bitmap_start + bi;
        Result<Page*> p = meta(fs, bblock);
        if (!p.ok()) return p.error();
        u8* data = p.value()->data;
        if (!block_ok(data, MAGIC_BITMAP, bi)) {
            page_put(p.value());
            return corrupt(fs, "bitmap", bblock);
        }
        u64 first = (u64)bi * BITS_PER_BITMAP_BLOCK;
        u64 end = first + BITS_PER_BITMAP_BLOCK < s.total_blocks ? first + BITS_PER_BITMAP_BLOCK : s.total_blocks;
        for (u64 x = b; x < end; x++) {
            u32 bit = (u32)(x - first);
            if (data[HEADER + bit / 8] & (1u << (bit % 8))) continue;
            data[HEADER + bit / 8] |= (u8)(1u << (bit % 8));
            meta_dirty(fs, p.value(), bblock, Kind::Bitmap);
            page_put(p.value());
            s.free_blocks--;
            fs->sb_dirty = true;
            fs->alloc_hint = x + 1;
            return x;
        }
        page_put(p.value());
        scanned += end - b;
    }
    return corrupt(fs, "free block count", 0);
}

Result<void> block_free(CerFs* fs, u64 b) {
    SuperBlock& s = fs->sb;
    if (b < s.data_start || b >= s.total_blocks) return corrupt(fs, "block number to free", b);
    u32 bi = (u32)(b / BITS_PER_BITMAP_BLOCK);
    u64 bblock = s.bitmap_start + bi;
    Result<Page*> p = meta(fs, bblock);
    if (!p.ok()) return p.error();
    u8* data = p.value()->data;
    if (!block_ok(data, MAGIC_BITMAP, bi)) {
        page_put(p.value());
        return corrupt(fs, "bitmap", bblock);
    }
    u32 bit = (u32)(b - (u64)bi * BITS_PER_BITMAP_BLOCK);
    if (data[HEADER + bit / 8] & (1u << (bit % 8))) {
        data[HEADER + bit / 8] &= (u8) ~(1u << (bit % 8));
        meta_dirty(fs, p.value(), bblock, Kind::Bitmap);
        s.free_blocks++;
        fs->sb_dirty = true;
        if (b < fs->alloc_hint) fs->alloc_hint = b;
    }
    page_put(p.value());
    return {};
}

Result<u32> inode_alloc(CerFs* fs) {
    SuperBlock& s = fs->sb;
    if (s.free_inodes == 0) return Error::NoSpace;
    u32 start = fs->inode_hint >= 2 && fs->inode_hint <= s.inode_count ? fs->inode_hint : 2;
    for (u32 k = 0; k < s.inode_count; k++) {
        u32 ino = (start - 1 + k) % s.inode_count + 1;
        u64 blk = inode_block(s, ino);
        Result<Page*> p = meta(fs, blk);
        if (!p.ok()) return p.error();
        const Inode* di = (const Inode*)(p.value()->data + inode_offset(ino));
        bool free = di->mode == 0;
        page_put(p.value());
        if (!free) continue;
        // A free inode that a cached vnode still uses (unlinked, still open)
        // is not reusable yet.
        bool busy = false;
        for (CerNode* n = fs->nodes; n; n = n->next) busy |= n->ino == ino;
        if (busy) continue;
        s.free_inodes--;
        fs->sb_dirty = true;
        fs->inode_hint = ino + 1;
        return ino;
    }
    return corrupt(fs, "free inode count", 0);
}

// ------------------------------------------------------- block mapping ---
// Reads entry `idx` of an indirect block, checking the block first.
Result<u64> ind_get(CerFs* fs, u64 blk, u32 owner, u32 level, u32 idx) {
    Result<Page*> p = meta(fs, blk);
    if (!p.ok()) return p.error();
    u8* d = p.value()->data;
    if (!block_ok(d, MAGIC_INDIRECT, owner) || ((BlockHeader*)d)->extra != level) {
        page_put(p.value());
        return corrupt(fs, "indirect block", blk);
    }
    u32 v;
    memcpy(&v, d + HEADER + idx * 4, 4);
    page_put(p.value());
    if (!data_block_ok(fs->sb, v)) return corrupt(fs, "block pointer", blk);
    return (u64)v;
}

Result<void> ind_set(CerFs* fs, u64 blk, u32 idx, u64 value) {
    Result<Page*> p = meta(fs, blk);
    if (!p.ok()) return p.error();
    u32 v = (u32)value;
    memcpy(p.value()->data + HEADER + idx * 4, &v, 4);
    meta_dirty(fs, p.value(), blk, Kind::Indirect);
    page_put(p.value());
    return {};
}

Result<u64> ind_new(CerFs* fs, u32 owner, u32 level) {
    Result<u64> b = block_alloc(fs);
    if (!b.ok()) return b;
    Result<Page*> p = meta_new(fs, b.value());
    if (!p.ok()) return p.error();
    BlockHeader* h = (BlockHeader*)p.value()->data;
    h->magic = MAGIC_INDIRECT;
    h->owner = owner;
    h->extra = level;
    meta_dirty(fs, p.value(), b.value(), Kind::Indirect);
    page_put(p.value());
    return b;
}

// Physical block of file block `idx`; 0 for a hole. With alloc, a hole is
// filled with a new block and *fresh is set (its old contents are garbage).
Result<u64> bmap(CerNode* n, u64 idx, bool alloc, bool* fresh = nullptr) {
    CerFs* fs = n->fs;
    if (fresh) *fresh = false;
    if (idx >= MAX_FILE_BLOCKS) return Error::TooBig;
    u32* slot = nullptr;
    u64 holder = 0;             // indirect block holding the entry, 0 = the inode
    u32 holder_idx = 0;
    if (idx < DIRECT) {
        slot = &n->di.direct[idx];
    } else if (idx < DIRECT + PTRS_PER_BLOCK) {
        if (!n->di.indirect) {
            if (!alloc) return (u64)0;
            Result<u64> b = ind_new(fs, n->ino, 1);
            if (!b.ok()) return b;
            n->di.indirect = (u32)b.value();
        }
        holder = n->di.indirect;
        holder_idx = (u32)(idx - DIRECT);
    } else {
        u64 rel = idx - DIRECT - PTRS_PER_BLOCK;
        if (!n->di.dindirect) {
            if (!alloc) return (u64)0;
            Result<u64> b = ind_new(fs, n->ino, 2);
            if (!b.ok()) return b;
            n->di.dindirect = (u32)b.value();
        }
        u32 l1 = (u32)(rel / PTRS_PER_BLOCK);
        Result<u64> mid = ind_get(fs, n->di.dindirect, n->ino, 2, l1);
        if (!mid.ok()) return mid;
        u64 m = mid.value();
        if (!m) {
            if (!alloc) return (u64)0;
            Result<u64> b = ind_new(fs, n->ino, 1);
            if (!b.ok()) return b;
            m = b.value();
            Result<void> s = ind_set(fs, n->di.dindirect, l1, m);
            if (!s.ok()) return s.error();
        }
        holder = m;
        holder_idx = (u32)(rel % PTRS_PER_BLOCK);
    }
    u64 cur;
    if (slot) cur = *slot;
    else {
        Result<u64> e = ind_get(fs, holder, n->ino, 1, holder_idx);
        if (!e.ok()) return e;
        cur = e.value();
    }
    if (cur || !alloc) return cur;
    Result<u64> b = block_alloc(fs);
    if (!b.ok()) return b;
    if (slot) *slot = (u32)b.value();
    else {
        Result<void> s = ind_set(fs, holder, holder_idx, b.value());
        if (!s.ok()) return s.error();
    }
    if (fresh) *fresh = true;
    return b;
}

// Frees file blocks from index `from` on, and indirect blocks left empty.
Result<void> free_from(CerNode* n, u64 from) {
    CerFs* fs = n->fs;
    for (u64 k = from; k < DIRECT; k++)
        if (n->di.direct[k]) {
            Result<void> r = block_free(fs, n->di.direct[k]);
            if (!r.ok()) return r;
            n->di.direct[k] = 0;
        }
    auto free_level1 = [&](u64 blk, u32 start) -> Result<void> {
        for (u32 e = start; e < PTRS_PER_BLOCK; e++) {
            Result<void> rr = tx_reserve_node(n);
            if (!rr.ok()) return rr;
            Result<u64> v = ind_get(fs, blk, n->ino, 1, e);
            if (!v.ok()) return v.error();
            if (!v.value()) continue;
            Result<void> r = block_free(fs, v.value());
            if (r.ok()) r = ind_set(fs, blk, e, 0);
            if (!r.ok()) return r;
        }
        return {};
    };
    if (n->di.indirect) {
        u32 start = from > DIRECT ? (u32)(from - DIRECT) : 0;
        if (start < PTRS_PER_BLOCK) {
            Result<void> r = free_level1(n->di.indirect, start);
            if (!r.ok()) return r;
            if (start == 0) {
                r = block_free(fs, n->di.indirect);
                if (!r.ok()) return r;
                n->di.indirect = 0;
            }
        }
    }
    if (n->di.dindirect) {
        u64 base = DIRECT + PTRS_PER_BLOCK;
        u64 start = from > base ? from - base : 0;
        for (u32 l1 = (u32)(start / PTRS_PER_BLOCK); l1 < PTRS_PER_BLOCK; l1++) {
            Result<u64> m = ind_get(fs, n->di.dindirect, n->ino, 2, l1);
            if (!m.ok()) return m.error();
            if (!m.value()) continue;
            u32 s2 = l1 == start / PTRS_PER_BLOCK ? (u32)(start % PTRS_PER_BLOCK) : 0;
            Result<void> r = free_level1(m.value(), s2);
            if (!r.ok()) return r;
            if (s2 == 0) {
                r = block_free(fs, m.value());
                if (r.ok()) r = ind_set(fs, n->di.dindirect, l1, 0);
                if (!r.ok()) return r;
            }
        }
        if (start == 0) {
            Result<void> r = block_free(fs, n->di.dindirect);
            if (!r.ok()) return r;
            n->di.dindirect = 0;
        }
    }
    return {};
}

// ----------------------------------------------------------- file data ---
Result<void> file_fill(Vnode* owner, u64 index, u32 count, u8* const* pages) {
    CerNode* n = node_of(owner);
    for (u32 i = 0; i < count; i++) {
        Result<u64> b = bmap(n, index + i, false);
        if (!b.ok()) return b.error();
        if (!b.value()) {
            memset(pages[i], 0, BLOCK);
            continue;
        }
        // Merge a run of consecutive blocks into one request.
        u32 run = 1;
        while (i + run < count) {
            Result<u64> nb = bmap(n, index + i + run, false);
            if (!nb.ok() || nb.value() != b.value() + run) break;
            run++;
        }
        if (run == 1) {
            Result<void> r = block_dev_read(n->fs->minor, b.value() * SECTORS, SECTORS, pages[i]);
            if (!r.ok()) return r;
        } else {
            u8* bounce = (u8*)kmalloc((usize)run * BLOCK);
            if (!bounce) return Error::NoMemory;
            Result<void> r = block_dev_read(n->fs->minor, b.value() * SECTORS, run * SECTORS, bounce);
            if (r.ok())
                for (u32 k = 0; k < run; k++) memcpy(pages[i + k], bounce + (usize)k * BLOCK, BLOCK);
            kfree(bounce);
            if (!r.ok()) return r;
            i += run - 1;
        }
    }
    return {};
}

Result<void> file_flush(Vnode* owner, u64 index, const u8* page) {
    CerNode* n = node_of(owner);
    if (index * BLOCK >= owner->size) return {};        // past the end (truncated)
    Result<u64> b = bmap(n, index, false);
    if (!b.ok()) return b.error();
    if (!b.value()) return {};                          // a hole that was never written
    return block_dev_write(n->fs->minor, b.value() * SECTORS, SECTORS, page);
}

const PageIo g_file_io = {file_fill, file_flush};

Result<usize> data_read(CerNode* n, u64 off, void* buf, usize len) {
    Vnode* v = n->v;
    if (off >= v->size) return (usize)0;
    if (len > v->size - off) len = (usize)(v->size - off);
    usize done = 0;
    while (done < len) {
        u64 at = off + done;
        Result<Page*> p = page_get(v, at / BLOCK, &g_file_io, true);
        if (!p.ok()) return done ? Result<usize>(done) : Result<usize>(p.error());
        usize in = (usize)(at % BLOCK);
        usize take = BLOCK - in < len - done ? BLOCK - in : len - done;
        memcpy((u8*)buf + done, p.value()->data + in, take);
        page_put(p.value());
        done += take;
    }
    return done;
}

Result<usize> data_write(CerNode* n, u64 off, const void* buf, usize len) {
    Vnode* v = n->v;
    if (off >= MAX_FILE_SIZE) return Error::TooBig;
    if (len > MAX_FILE_SIZE - off) len = (usize)(MAX_FILE_SIZE - off);
    usize done = 0;
    Error err = Error::None;
    while (done < len) {
        Result<void> rr = tx_reserve_node(n);
        if (!rr.ok()) {
            err = rr.error();
            break;
        }
        u64 at = off + done;
        u64 idx = at / BLOCK;
        usize in = (usize)(at % BLOCK);
        usize take = BLOCK - in < len - done ? BLOCK - in : len - done;
        bool fresh;
        Result<u64> b = bmap(n, idx, true, &fresh);
        if (!b.ok()) {
            err = b.error();
            break;
        }
        bool whole = take == BLOCK;
        // A new block's old contents belong to some deleted file: never read
        // them, zero instead.
        Result<Page*> p = page_get(v, idx, &g_file_io, !whole && !fresh);
        if (!p.ok()) {
            err = p.error();
            break;
        }
        if (fresh && !whole) memset(p.value()->data, 0, BLOCK);
        memcpy(p.value()->data + in, (const u8*)buf + done, take);
        page_mark_dirty(p.value());
        page_put(p.value());
        done += take;
        if (at + take > v->size) v->size = at + take;
    }
    if (done) {
        v->mtime = v->ctime = vfs_now();
        Result<void> r = node_write(n);
        if (!r.ok() && err == Error::None) err = r.error();
    }
    if (!done) return err == Error::None ? Error::IO : err;
    return done;
}

// ----------------------------------------------------------- directories --
struct DirSlot {
    u64 block;                  // physical
    u32 offset;                 // of the record
    u32 prev;                   // offset of the previous record in the block, 0 if first
    u32 ino;
    u8 type;
};

// Iterates the directory's blocks; fn(page, phys, data) returns true to stop.
template <typename Fn> Result<bool> dir_blocks(CerNode* d, Fn fn) {
    CerFs* fs = d->fs;
    u64 blocks = d->v->size / BLOCK;
    for (u64 i = 0; i < blocks; i++) {
        Result<u64> b = bmap(d, i, false);
        if (!b.ok()) return b.error();
        if (!b.value()) return corrupt(fs, "directory with a hole", d->ino);
        Result<Page*> p = meta(fs, b.value());
        if (!p.ok()) return p.error();
        u8* data = p.value()->data;
        if (!block_ok(data, MAGIC_DIR, d->ino)) {
            page_put(p.value());
            return corrupt(fs, "directory block", b.value());
        }
        bool stop = fn(p.value(), b.value(), data);
        page_put(p.value());
        if (stop) return true;
    }
    return false;
}

Result<bool> dir_find(CerNode* d, const char* name, usize len, DirSlot* out) {
    bool bad = false;
    Result<bool> r = dir_blocks(d, [&](Page*, u64 phys, u8* data) {
        u32 prev = 0;
        bool found = false;
        bool ok = each_dirrec(data, d->fs->sb.inode_count, [&](const DirRec* rec, u32 off) {
            if (rec->inode && rec->name_len == len && memcmp(rec->name, name, len) == 0) {
                *out = {phys, off, prev, rec->inode, rec->type};
                found = true;
                return false;
            }
            prev = off;
            return true;
        });
        if (!ok) bad = true;
        return found || bad;
    });
    if (!r.ok()) return r;
    if (bad) return corrupt(d->fs, "directory records", d->ino);
    return r.value();
}

void write_rec(u8* data, u32 off, u32 ino, u16 rec_len, const char* name, usize len, u8 type) {
    DirRec* r = (DirRec*)(data + off);
    r->inode = ino;
    r->rec_len = rec_len;
    r->name_len = (u8)len;
    r->type = type;
    memcpy(r->name, name, len);
}

Result<void> dir_add(CerNode* d, const char* name, usize len, u32 ino, u8 type) {
    CerFs* fs = d->fs;
    u32 need = dirrec_size((u32)len);
    bool placed = false, bad = false;
    Result<bool> r = dir_blocks(d, [&](Page* p, u64 phys, u8* data) {
        bool ok = each_dirrec(data, fs->sb.inode_count, [&](const DirRec* rec, u32 off) {
            u32 used = rec->inode ? dirrec_size(rec->name_len) : 0;
            if (rec->rec_len - used < need) return true;
            if (rec->inode) {
                u16 rest = (u16)(rec->rec_len - used);
                ((DirRec*)(data + off))->rec_len = (u16)used;
                write_rec(data, off + used, ino, rest, name, len, type);
            } else {
                write_rec(data, off, ino, rec->rec_len, name, len, type);
            }
            meta_dirty(fs, p, phys, Kind::Dir);
            placed = true;
            return false;
        });
        if (!ok) bad = true;
        return placed || bad;
    });
    if (!r.ok()) return r.error();
    if (bad) return corrupt(fs, "directory records", d->ino);
    if (placed) return {};
    // No room: a new block at the end of the directory.
    bool fresh;
    Result<u64> b = bmap(d, d->v->size / BLOCK, true, &fresh);
    if (!b.ok()) return b.error();
    Result<Page*> p = meta_new(fs, b.value());
    if (!p.ok()) return p.error();
    BlockHeader* h = (BlockHeader*)p.value()->data;
    h->magic = MAGIC_DIR;
    h->owner = d->ino;
    write_rec(p.value()->data, HEADER, ino, (u16)(BLOCK - HEADER), name, len, type);
    meta_dirty(fs, p.value(), b.value(), Kind::Dir);
    page_put(p.value());
    d->v->size += BLOCK;
    return node_write(d);
}

Result<void> dir_remove(CerNode* d, const DirSlot& s) {
    Result<Page*> p = meta(d->fs, s.block);
    if (!p.ok()) return p.error();
    u8* data = p.value()->data;
    DirRec* rec = (DirRec*)(data + s.offset);
    if (s.prev) {
        DirRec* prev = (DirRec*)(data + s.prev);
        prev->rec_len = (u16)(prev->rec_len + rec->rec_len);
    } else {
        rec->inode = 0;
    }
    meta_dirty(d->fs, p.value(), s.block, Kind::Dir);
    page_put(p.value());
    return {};
}

Result<void> dir_set_ino(CerNode* d, const DirSlot& s, u32 ino) {
    Result<Page*> p = meta(d->fs, s.block);
    if (!p.ok()) return p.error();
    ((DirRec*)(p.value()->data + s.offset))->inode = ino;
    meta_dirty(d->fs, p.value(), s.block, Kind::Dir);
    page_put(p.value());
    return {};
}

Result<bool> dir_empty(CerNode* d) {
    bool nonempty = false, bad = false;
    Result<bool> r = dir_blocks(d, [&](Page*, u64, u8* data) {
        bool ok = each_dirrec(data, d->fs->sb.inode_count, [&](const DirRec* rec, u32) {
            if (!rec->inode) return true;
            bool dot = rec->name_len == 1 && rec->name[0] == '.';
            bool dotdot = rec->name_len == 2 && rec->name[0] == '.' && rec->name[1] == '.';
            if (!dot && !dotdot) nonempty = true;
            return !nonempty;
        });
        if (!ok) bad = true;
        return nonempty || bad;
    });
    if (!r.ok()) return r.error();
    if (bad) return corrupt(d->fs, "directory records", d->ino);
    return !nonempty;
}

// Frees everything of an inode nobody refers to any more.
Result<void> inode_destroy(CerNode* n) {
    Result<void> r = free_from(n, 0);
    if (!r.ok()) return r;
    page_drop_owner(n->v, 0);
    memset(&n->di, 0, sizeof n->di);
    r = inode_write(n->fs, n->ino, &n->di);
    if (!r.ok()) return r;
    n->fs->sb.free_inodes++;
    n->fs->sb_dirty = true;
    return {};
}

// ------------------------------------------------------ vnode operations --
Result<Vnode*> cer_lookup(Vnode* dir, const char* name, usize len) {
    CerNode* d = node_of(dir);
    DirSlot s;
    Result<bool> f = dir_find(d, name, len, &s);
    if (!f.ok()) return f.error();
    if (!f.value()) return Error::NotFound;
    return node_get(d->fs, s.ino);
}

Result<Vnode*> cer_create(Vnode* dir, const char* name, usize len, VType type, u32 mode, u32 uid, u32 gid,
                          const char* target, u32) {
    CerNode* d = node_of(dir);
    CerFs* fs = d->fs;
    if (type == VType::CharDev || type == VType::BlockDev) return Error::NotSupported;
    if (len > NAME_MAX) return Error::NameTooLong;
    usize tlen = target ? strlen(target) : 0;
    if (type == VType::Symlink && (tlen == 0 || tlen >= BLOCK)) return Error::NameTooLong;
    Result<void> rr = tx_reserve(fs);
    if (!rr.ok()) return rr.error();
    DirSlot s;
    Result<bool> f = dir_find(d, name, len, &s);
    if (!f.ok()) return f.error();
    if (f.value()) return Error::Exists;
    Result<u32> ino = inode_alloc(fs);
    if (!ino.ok()) return ino.error();
    u64 now = vfs_now();
    Inode di;
    memset(&di, 0, sizeof di);
    u16 tbits = type == VType::Dir ? T_DIR : type == VType::Symlink ? T_LINK : T_FILE;
    di.mode = (u16)(tbits | (type == VType::Symlink ? 0777 : (mode & 07777)));
    di.links = type == VType::Dir ? 2 : 1;
    di.uid = uid;
    di.gid = gid;
    di.atime = di.mtime = di.ctime = now;
    u32 gen;
    csprng_bytes(&gen, sizeof gen);
    di.generation = gen;
    Result<void> r = inode_write(fs, ino.value(), &di);
    if (!r.ok()) return r.error();
    Result<Vnode*> v = node_get(fs, ino.value());
    if (!v.ok()) return v.error();
    CerNode* n = node_of(v.value());
    if (type == VType::Dir) {
        // "." and ".." in the first block; the parent gains a link.
        bool fresh;
        Result<u64> b = bmap(n, 0, true, &fresh);
        Result<Page*> p = b.ok() ? meta_new(fs, b.value()) : Result<Page*>(b.error());
        if (!p.ok()) {
            vnode_unref(v.value());
            return p.error();
        }
        u8* data = p.value()->data;
        BlockHeader* h = (BlockHeader*)data;
        h->magic = MAGIC_DIR;
        h->owner = ino.value();
        u16 first = (u16)dirrec_size(1);
        write_rec(data, HEADER, ino.value(), first, ".", 1, REC_DIR);
        write_rec(data, HEADER + first, d->ino, (u16)(BLOCK - HEADER - first), "..", 2, REC_DIR);
        meta_dirty(fs, p.value(), b.value(), Kind::Dir);
        page_put(p.value());
        n->v->size = BLOCK;
        r = node_write(n);
        if (r.ok()) {
            dir->nlink++;
            r = node_write(d);
        }
    } else if (type == VType::Symlink) {
        Result<usize> w = data_write(n, 0, target, tlen);
        r = w.ok() ? Result<void>() : Result<void>(w.error());
    }
    if (r.ok()) r = dir_add(d, name, len, ino.value(), dirtype_of(type));
    if (r.ok()) {
        dir->mtime = dir->ctime = now;
        r = node_write(d);
    }
    if (!r.ok()) {
        n->v->nlink = 0;        // released (and freed) by the unref below
        vnode_unref(v.value());
        return r.error();
    }
    return v;
}

Result<void> cer_unlink(Vnode* dir, const char* name, usize len, bool dir_wanted) {
    CerNode* d = node_of(dir);
    CerFs* fs = d->fs;
    Result<void> rr = tx_reserve(fs);
    if (!rr.ok()) return rr;
    DirSlot s;
    Result<bool> f = dir_find(d, name, len, &s);
    if (!f.ok()) return f.error();
    if (!f.value()) return Error::NotFound;
    Result<Vnode*> tv = node_get(fs, s.ino);
    if (!tv.ok()) return tv.error();
    Vnode* t = tv.value();
    CerNode* tn = node_of(t);
    Result<void> r;
    if (dir_wanted && t->type != VType::Dir) r = Error::NotDir;
    else if (!dir_wanted && t->type == VType::Dir) r = Error::IsDir;
    else if (t->type == VType::Dir) {
        Result<bool> e = dir_empty(tn);
        r = !e.ok() ? Result<void>(e.error()) : e.value() ? Result<void>() : Result<void>(Error::NotEmpty);
    }
    if (r.ok()) r = dir_remove(d, s);
    if (r.ok()) {
        u64 now = vfs_now();
        if (t->type == VType::Dir) {
            t->nlink = 0;
            if (dir->nlink > 2) dir->nlink--;
        } else if (t->nlink) {
            t->nlink--;
        }
        t->ctime = now;
        dir->mtime = dir->ctime = now;
        r = node_write(tn);
        if (r.ok()) r = node_write(d);
    }
    vnode_unref(t);             // the last reference frees it (cer_release)
    return r;
}

Result<void> cer_link(Vnode* dir, const char* name, usize len, Vnode* existing) {
    CerNode* d = node_of(dir);
    CerNode* e = node_of(existing);
    CerFs* fs = d->fs;
    Result<void> rr = tx_reserve(fs);
    if (!rr.ok()) return rr;
    DirSlot s;
    Result<bool> f = dir_find(d, name, len, &s);
    if (!f.ok()) return f.error();
    if (f.value()) return Error::Exists;
    if (existing->nlink >= 0xFFFE) return Error::TooBig;
    Result<void> r = dir_add(d, name, len, e->ino, dirtype_of(existing->type));
    if (!r.ok()) return r;
    u64 now = vfs_now();
    existing->nlink++;
    existing->ctime = now;
    dir->mtime = dir->ctime = now;
    r = node_write(e);
    if (r.ok()) r = node_write(d);
    return r;
}

Result<void> cer_rename(Vnode* from_dir, const char* from, usize from_len, Vnode* to_dir, const char* to,
                        usize to_len) {
    CerNode* fd = node_of(from_dir);
    CerNode* td = node_of(to_dir);
    CerFs* fs = fd->fs;
    Result<void> rr = tx_reserve(fs);
    if (!rr.ok()) return rr;
    DirSlot src;
    Result<bool> f = dir_find(fd, from, from_len, &src);
    if (!f.ok()) return f.error();
    if (!f.value()) return Error::NotFound;
    DirSlot dst;
    Result<bool> g = dir_find(td, to, to_len, &dst);
    if (!g.ok()) return g.error();
    if (g.value() && dst.ino == src.ino) return {};
    Result<Vnode*> sv = node_get(fs, src.ino);
    if (!sv.ok()) return sv.error();
    Vnode* s = sv.value();
    bool sdir = s->type == VType::Dir;
    Result<void> r;
    u64 now = vfs_now();
    if (g.value()) {
        Result<Vnode*> dv = node_get(fs, dst.ino);
        if (!dv.ok()) {
            vnode_unref(s);
            return dv.error();
        }
        Vnode* t = dv.value();
        bool ddir = t->type == VType::Dir;
        if (sdir && !ddir) r = Error::NotDir;
        else if (!sdir && ddir) r = Error::IsDir;
        else if (ddir) {
            Result<bool> e = dir_empty(node_of(t));
            r = !e.ok() ? Result<void>(e.error()) : e.value() ? Result<void>() : Result<void>(Error::NotEmpty);
        }
        if (r.ok()) {
            // The target's name now leads to the source.
            r = dir_set_ino(td, dst, src.ino);
            if (r.ok()) {
                if (ddir) {
                    t->nlink = 0;
                    if (to_dir->nlink > 2) to_dir->nlink--;
                } else if (t->nlink) {
                    t->nlink--;
                }
                t->ctime = now;
                r = node_write(node_of(t));
            }
        }
        vnode_unref(t);
    } else {
        r = dir_add(td, to, to_len, src.ino, src.type);
    }
    if (r.ok()) {
        // The old name goes; find it again, the directory may have changed.
        DirSlot again;
        Result<bool> h = dir_find(fd, from, from_len, &again);
        if (!h.ok()) r = h.error();
        else if (h.value() && again.ino == src.ino) r = dir_remove(fd, again);
    }
    if (r.ok() && sdir && fd != td) {
        DirSlot dd;
        Result<bool> h = dir_find(node_of(s), "..", 2, &dd);
        if (!h.ok()) r = h.error();
        else if (!h.value()) r = corrupt(fs, "directory without ..", src.ino);
        else r = dir_set_ino(node_of(s), dd, td->ino);
        if (r.ok()) {
            if (from_dir->nlink > 2) from_dir->nlink--;
            to_dir->nlink++;
        }
    }
    if (r.ok()) {
        s->ctime = now;
        from_dir->mtime = from_dir->ctime = to_dir->mtime = to_dir->ctime = now;
        r = node_write(node_of(s));
        if (r.ok()) r = node_write(fd);
        if (r.ok() && td != fd) r = node_write(td);
    }
    vnode_unref(s);
    return r;
}

Result<usize> cer_read(Vnode* v, u64 off, void* buf, usize n) { return data_read(node_of(v), off, buf, n); }

Result<usize> cer_write(Vnode* v, u64 off, const void* buf, usize n) {
    CerFs* fs = fs_of(v);
    Result<void> rr = tx_reserve(fs);
    if (!rr.ok()) return rr.error();
    return data_write(node_of(v), off, buf, n);
}

Result<void> cer_truncate(Vnode* v, u64 size) {
    CerNode* n = node_of(v);
    CerFs* fs = n->fs;
    if (size > MAX_FILE_SIZE) return Error::TooBig;
    Result<void> r = tx_reserve(fs);
    if (!r.ok()) return r;
    u64 old = v->size;
    if (size < old) {
        // The new size first, so every intermediate state on disk is valid.
        v->size = size;
        v->mtime = v->ctime = vfs_now();
        r = node_write(n);
        u64 keep = (size + BLOCK - 1) / BLOCK;
        if (r.ok()) r = free_from(n, keep);
        page_drop_owner(v, keep);
        // The rest of the last page reads as zeros if the file grows again.
        if (r.ok() && size % BLOCK) {
            Result<Page*> p = page_get(v, size / BLOCK, &g_file_io, true);
            if (!p.ok()) r = p.error();
            else {
                memset(p.value()->data + size % BLOCK, 0, BLOCK - size % BLOCK);
                page_mark_dirty(p.value());
                page_put(p.value());
            }
        }
        if (r.ok()) r = node_write(n);
        return r;
    }
    v->size = size;             // growing leaves a hole
    v->mtime = v->ctime = vfs_now();
    return node_write(n);
}

Result<bool> cer_readdir(Vnode* dir, u64* cookie, DirEntry* out) {
    CerNode* d = node_of(dir);
    CerFs* fs = d->fs;
    u64 blocks = dir->size / BLOCK;
    for (u64 i = *cookie / BLOCK; i < blocks; i++) {
        Result<u64> b = bmap(d, i, false);
        if (!b.ok()) return b.error();
        if (!b.value()) return corrupt(fs, "directory with a hole", d->ino);
        Result<Page*> p = meta(fs, b.value());
        if (!p.ok()) return p.error();
        u8* data = p.value()->data;
        if (!block_ok(data, MAGIC_DIR, d->ino)) {
            page_put(p.value());
            return corrupt(fs, "directory block", b.value());
        }
        u32 skip_to = i == *cookie / BLOCK ? (u32)(*cookie % BLOCK) : 0;
        bool found = false;
        bool ok = each_dirrec(data, fs->sb.inode_count, [&](const DirRec* rec, u32 off) {
            if (off < skip_to || !rec->inode) return true;
            bool dot = rec->name_len == 1 && rec->name[0] == '.';
            bool dotdot = rec->name_len == 2 && rec->name[0] == '.' && rec->name[1] == '.';
            if (dot || dotdot) return true;
            out->ino = rec->inode;
            out->type = rec->type == REC_DIR ? VType::Dir : rec->type == REC_LINK ? VType::Symlink : VType::File;
            memcpy(out->name, rec->name, rec->name_len);
            out->name[rec->name_len] = 0;
            *cookie = i * BLOCK + off + rec->rec_len;
            found = true;
            return false;
        });
        page_put(p.value());
        if (!ok) return corrupt(fs, "directory records", d->ino);
        if (found) return true;
    }
    *cookie = blocks * BLOCK;
    return false;
}

Result<usize> cer_readlink(Vnode* v, char* buf, usize n) { return data_read(node_of(v), 0, buf, n); }

Result<void> cer_setattr(Vnode* v) {
    Result<void> r = tx_reserve(fs_of(v));
    return r.ok() ? node_write(node_of(v)) : r;
}

Result<void> cer_fsync(Vnode* v) { return commit(fs_of(v)); }

// Forgets a vnode nobody holds: its data is written first (the pages are
// keyed by the vnode, which is about to go).
void node_free(CerNode* n) {
    CerFs* fs = n->fs;
    Vnode* v = n->v;
    Result<void> r = page_sync_owner(v);
    if (!r.ok()) fs->broken = true;
    page_drop_owner(v, 0);
    for (CerNode** link = &fs->nodes; *link; link = &(*link)->next)
        if (*link == n) {
            *link = n->next;
            break;
        }
    if (n->v->refs == 0 && fs->inactive) fs->inactive--;
    kfree(n);
    kfree(v);
}

// The last reference went. A file that still has a name stays cached
// (inactive) with its pages, so opening it again finds its data in memory;
// past MAX_INACTIVE the oldest inactive vnode is let go. A file without a
// name is freed on disk now.
void cer_release(Vnode* v) {
    CerNode* n = node_of(v);
    CerFs* fs = n->fs;
    if (v->nlink == 0 && n->di.mode != 0 && !fs->broken && !(fs->mount.flags & vfs::MNT_RDONLY)) {
        Result<void> r = inode_destroy(n);
        if (!r.ok()) kprintf("cerfs: could not free inode %u: %s\n", n->ino, error_name(r.error()));
        node_free(n);
        return;
    }
    fs->inactive++;
    while (fs->inactive > MAX_INACTIVE) {
        // New vnodes go to the front of the list, so the oldest inactive one is the last.
        CerNode* oldest = nullptr;
        for (CerNode* o = fs->nodes; o; o = o->next)
            if (o->v->refs == 0 && o != n) oldest = o;
        if (!oldest) break;
        node_free(oldest);
    }
}

const VnodeOps g_ops = {
    cer_lookup,   cer_create,   cer_unlink,  cer_rename, cer_read,  cer_write,   cer_truncate,
    cer_readdir,  cer_readlink, cer_setattr, cer_fsync,  nullptr,   cer_release, cer_link,
};

// ---------------------------------------------------------------- mount --
Result<void> cer_sync(Mount* m) { return commit((CerFs*)m->fs_data); }

Result<void> cer_unmount(Mount* m) {
    CerFs* fs = (CerFs*)m->fs_data;
    Result<void> r = commit(fs);
    if (r.ok() && !(m->flags & vfs::MNT_RDONLY)) {
        fs->sb.state = STATE_CLEAN;
        fs->sb_dirty = true;
        r = commit(fs);
    }
    // Every vnode left is inactive (unmount checked that nothing is in use).
    while (fs->nodes) node_free(fs->nodes);
    // After an I/O error a transaction may be left uncommitted: its pages are
    // let go (the changes are lost, which is what a failed write means).
    for (u32 i = 0; i < fs->tx_count; i++) {
        page_hold(fs->tx[i].page, false);
        page_put(fs->tx[i].page);
    }
    fs->tx_count = 0;
    Result<void> s = page_sync_owner(fs->dev);
    if (s.ok()) page_drop_owner(fs->dev, 0);
    block_unclaim(fs->minor);
    vnode_unref(fs->dev);
    kfree(fs);
    return r;
}

Result<Mount*> cer_mount(const char* source, u32 flags) {
    Credentials rootcred{0, 0};
    Result<Vnode*> dv = vfs_resolve(nullptr, source, rootcred, LookupFlags{});
    if (!dv.ok()) return dv.error();
    Vnode* dev_node = dv.value();
    if (dev_node->type != VType::BlockDev || !block_present(dev::minor_of(dev_node->rdev))) {
        vnode_unref(dev_node);
        return Error::NoDevice;
    }
    if (!block_claim(dev::minor_of(dev_node->rdev))) {
        vnode_unref(dev_node);
        return Error::Busy;             // already mounted
    }
    CerFs* fs = (CerFs*)kzalloc(sizeof(CerFs));
    if (!fs) {
        vnode_unref(dev_node);
        return Error::NoMemory;
    }
    fs->dev = dev_node;
    fs->minor = dev::minor_of(dev_node->rdev);
    Mount* m = &fs->mount;
    m->type = "cerfs";
    strlcpy(m->source, source, sizeof m->source);
    m->flags = flags;
    m->fs_data = fs;
    m->unmount = cer_unmount;
    m->sync = cer_sync;

    auto fail = [&](Error e) -> Result<Mount*> {
        block_unclaim(fs->minor);
        vnode_unref(dev_node);
        kfree(fs);
        return e;
    };
    // The superblock, read raw (nothing of this disk may be cached before
    // the journal has been replayed).
    u8* b = (u8*)kmalloc(BLOCK);
    if (!b) return fail(Error::NoMemory);
    Result<void> r = raw_read(fs, 0, b);
    memcpy(&fs->sb, b, sizeof fs->sb);
    kfree(b);
    if (!r.ok()) return fail(r.error());
    BlockDevice* bd = block_get(fs->minor);
    if (!check_super(fs->sb, bd->sectors / SECTORS)) {
        kprintf("cerfs: %s: not a valid cerfs (or damaged superblock)\n", source);
        return fail(Error::Invalid);
    }
    (void)page_sync_owner(dev_node);
    page_drop_owner(dev_node, 0);
    // Journal: header, descriptor, images, commit; one image slot is kept
    // for the superblock.
    fs->tx_cap = fs->sb.journal_blocks - 4 < JOURNAL_MAX_TX - 1 ? fs->sb.journal_blocks - 4 : JOURNAL_MAX_TX - 1;
    if (!(flags & vfs::MNT_RDONLY)) {
        r = replay(fs);
        if (!r.ok()) return fail(r.error());
        // Replay may have rewritten the superblock: read it again.
        u8* b2 = (u8*)kmalloc(BLOCK);
        if (!b2) return fail(Error::NoMemory);
        r = raw_read(fs, 0, b2);
        memcpy(&fs->sb, b2, sizeof fs->sb);
        kfree(b2);
        if (!r.ok() || !check_super(fs->sb, bd->sectors / SECTORS)) return fail(r.ok() ? Error::IO : r.error());
    } else {
        // Read-only: the journal is left alone; a pending transaction is
        // replayed by the next read-write mount.
        fs->seq = 0;
    }
    Result<Vnode*> root = node_get(fs, ROOT_INODE);
    if (!root.ok()) return fail(root.error());
    if (root.value()->type != VType::Dir) {
        vnode_unref(root.value());
        return fail(corrupt(fs, "root directory", ROOT_INODE));
    }
    m->root = root.value();
    if (!(flags & vfs::MNT_RDONLY)) {
        fs->sb.state = STATE_MOUNTED;
        fs->sb.mount_count++;
        fs->sb.last_mount_time = vfs_now();
        fs->sb_dirty = true;
        r = commit(fs);
        if (!r.ok()) {
            vnode_unref(m->root);
            return fail(r.error());
        }
    }
    kprintf("cerfs: mounted %s (%lu MiB, %lu MiB free, %u of %u inodes free%s)\n", source,
            (unsigned long)(fs->sb.total_blocks / 256), (unsigned long)(fs->sb.free_blocks / 256), fs->sb.free_inodes,
            fs->sb.inode_count, flags & vfs::MNT_RDONLY ? ", read-only" : "");
    return m;
}

} // namespace

void cerfs_register() { vfs_register_type("cerfs", cer_mount); }
