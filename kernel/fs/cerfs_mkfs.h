// Formats a cerfs file system (version 1). Header-only and dependent only on
// the format header, so the host tool (tools/mkfs-cerfs.cpp), the
// mkfs.cerfs program inside Cerberus and the kernel's own tests share it. The
// caller supplies a function that writes one 4 KiB block.
//
// Layout choices: one inode per 16 KiB of disk (at least 64), a journal of
// 1/64 of the disk between 64 and 4096 blocks, an empty root directory.
#pragma once

#include <fs/cerfs_format.h>

namespace cerfs {

struct MkfsResult {
    bool ok;
    const char* error;          // when !ok
    SuperBlock sb;
};

// write_block(ctx, block_number, data) returns false on an I/O error.
// `uuid` is 16 bytes (random from the caller), `label` up to 31 bytes,
// `now` seconds since 1970.
template <typename WriteFn>
MkfsResult mkfs(u64 total_blocks, const char* label, const u8* uuid, u64 now, WriteFn write_block) {
    MkfsResult r{};
    if (total_blocks < 256) {
        r.error = "disk too small (at least 1 MiB)";
        return r;
    }
    if (total_blocks > 0xFFFFFFFFull) total_blocks = 0xFFFFFFFFull;
    SuperBlock& s = r.sb;
    __builtin_memset(&s, 0, sizeof s);
    __builtin_memcpy(s.magic, MAGIC, 8);
    s.version = VERSION;
    s.block_size = BLOCK;
    s.total_blocks = total_blocks;
    s.bitmap_start = 1;
    s.bitmap_blocks = (u32)((total_blocks + BITS_PER_BITMAP_BLOCK - 1) / BITS_PER_BITMAP_BLOCK);
    u64 inodes = total_blocks / 4;
    if (inodes < 64) inodes = 64;
    if (inodes > 0x00FFFFFF) inodes = 0x00FFFFFF;
    s.inode_table_blocks = (u32)((inodes + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK);
    s.inode_count = s.inode_table_blocks * INODES_PER_BLOCK;
    s.inode_table_start = s.bitmap_start + s.bitmap_blocks;
    u64 journal = total_blocks / 64;
    if (journal < 64) journal = 64;
    if (journal > 4096) journal = 4096;
    s.journal_blocks = (u32)journal;
    s.journal_start = s.inode_table_start + s.inode_table_blocks;
    s.data_start = s.journal_start + s.journal_blocks;
    if (s.data_start + 16 > total_blocks) {
        r.error = "disk too small for the metadata";
        return r;
    }
    s.root_inode = ROOT_INODE;
    s.state = STATE_CLEAN;
    for (int i = 0; i < 16; i++) s.uuid[i] = uuid[i];
    for (int i = 0; i < 31 && label && label[i]; i++) s.label[i] = label[i];
    u64 root_block = s.data_start;              // the root directory's only block
    s.free_blocks = total_blocks - (root_block + 1);
    s.free_inodes = s.inode_count - 1;

    static u8 block[BLOCK];
    // Bitmap: everything up to and including the root directory block is used.
    for (u32 bi = 0; bi < s.bitmap_blocks; bi++) {
        __builtin_memset(block, 0, BLOCK);
        BlockHeader* h = (BlockHeader*)block;
        h->magic = MAGIC_BITMAP;
        h->owner = bi;
        u64 first = (u64)bi * BITS_PER_BITMAP_BLOCK;
        for (u64 b = first; b < first + BITS_PER_BITMAP_BLOCK; b++) {
            bool used = b <= root_block || b >= total_blocks;      // bits past the end stay set
            if (used) block[HEADER + (b - first) / 8] |= (u8)(1u << ((b - first) % 8));
        }
        seal_block(block);
        if (!write_block(s.bitmap_start + bi, block)) {
            r.error = "write failed (bitmap)";
            return r;
        }
    }
    // Inode table: all free except the root.
    for (u32 ti = 0; ti < s.inode_table_blocks; ti++) {
        __builtin_memset(block, 0, BLOCK);
        if (ti == 0) {
            Inode* root = (Inode*)block;
            root->mode = T_DIR | 0755;
            root->links = 2;
            root->size = BLOCK;
            root->atime = root->mtime = root->ctime = now;
            root->direct[0] = (u32)root_block;
            root->generation = 1;
            root->crc = inode_crc(*root);
        }
        if (!write_block(s.inode_table_start + ti, block)) {
            r.error = "write failed (inode table)";
            return r;
        }
    }
    // Journal header: empty, first sequence 1.
    __builtin_memset(block, 0, BLOCK);
    JournalHeader* jh = (JournalHeader*)block;
    jh->h.magic = MAGIC_JHEAD;
    jh->sequence = 1;
    seal_block(block);
    if (!write_block(s.journal_start, block)) {
        r.error = "write failed (journal)";
        return r;
    }
    // A stale transaction left on a reused disk must not be replayed.
    __builtin_memset(block, 0, BLOCK);
    if (!write_block(s.journal_start + 1, block)) {
        r.error = "write failed (journal)";
        return r;
    }
    // Root directory: "." and "..", both the root.
    __builtin_memset(block, 0, BLOCK);
    BlockHeader* dh = (BlockHeader*)block;
    dh->magic = MAGIC_DIR;
    dh->owner = ROOT_INODE;
    DirRec* dot = (DirRec*)(block + HEADER);
    dot->inode = ROOT_INODE;
    dot->rec_len = (u16)dirrec_size(1);
    dot->name_len = 1;
    dot->type = REC_DIR;
    dot->name[0] = '.';
    DirRec* dotdot = (DirRec*)(block + HEADER + dot->rec_len);
    dotdot->inode = ROOT_INODE;
    dotdot->rec_len = (u16)(BLOCK - HEADER - dot->rec_len);
    dotdot->name_len = 2;
    dotdot->type = REC_DIR;
    dotdot->name[0] = dotdot->name[1] = '.';
    seal_block(block);
    if (!write_block(root_block, block)) {
        r.error = "write failed (root directory)";
        return r;
    }
    // Superblock last: until it is written the disk is not a cerfs.
    __builtin_memset(block, 0, BLOCK);
    s.crc = super_crc(s);
    __builtin_memcpy(block, &s, sizeof s);
    if (!write_block(0, block)) {
        r.error = "write failed (superblock)";
        return r;
    }
    r.ok = true;
    return r;
}

} // namespace cerfs
