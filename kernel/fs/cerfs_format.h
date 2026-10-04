// cerfs on-disk format, version 1 (SPEC phase 9). Shared by the kernel
// (fs/cerfs.cpp), the host tool tools/mkfs-cerfs.cpp and the fuzzer; it
// depends only on lib/types.h and lib/crc32c.h.
//
// Little-endian, 4 KiB blocks, block numbers are u32 (16 TiB per disk).
//
//   block 0                     superblock
//   bitmap_start ...            block bitmap: one bit per block of the disk
//   inode_table_start ...       inodes, 128 bytes each, 32 per block
//   journal_start               journal header
//   journal_start + 1 ...       journal area (one transaction at a time)
//   data_start ...              file data, directory blocks, indirect blocks
//
// Every metadata block carries a CRC32C: the superblock and each inode in a
// field of their own, and bitmap, directory, indirect and journal blocks in
// a 16-byte header that also names the block (its number, or its owner) so
// a block written to the wrong place is caught. File data has no checksum.
//
// Everything read from a disk is hostile: the check_* functions below
// validate every field before the kernel uses it.
#pragma once

#include <lib/crc32c.h>
#include <lib/types.h>

namespace cerfs {

constexpr u32 BLOCK = 4096;
constexpr u32 VERSION = 1;
constexpr char MAGIC[8] = {'C', 'E', 'R', 'F', 'S', 0, 0, 0};
constexpr u32 INODE_SIZE = 128;
constexpr u32 INODES_PER_BLOCK = BLOCK / INODE_SIZE;
constexpr u32 ROOT_INODE = 1;
constexpr u32 NAME_MAX = 255;
constexpr u32 DIRECT = 12;
constexpr u32 HEADER = 16;                          // bytes of a block header
constexpr u32 PTRS_PER_BLOCK = (BLOCK - HEADER) / 4;    // 1020 in an indirect block
constexpr u32 BITS_PER_BITMAP_BLOCK = (BLOCK - HEADER) * 8;
constexpr u64 MAX_FILE_BLOCKS = DIRECT + PTRS_PER_BLOCK + (u64)PTRS_PER_BLOCK * PTRS_PER_BLOCK;
constexpr u64 MAX_FILE_SIZE = MAX_FILE_BLOCKS * BLOCK;

// Block header magics.
constexpr u32 MAGIC_BITMAP = 0x504D424C;   // "LBMP"
constexpr u32 MAGIC_DIR = 0x5249444C;      // "LDIR"
constexpr u32 MAGIC_INDIRECT = 0x444E494C; // "LIND"
constexpr u32 MAGIC_JHEAD = 0x4C4E4A4C;    // "LJNL"
constexpr u32 MAGIC_JDESC = 0x53444A4C;    // "LJDS"
constexpr u32 MAGIC_JCOMMIT = 0x4D434A4C;  // "LJCM"

// Superblock states.
constexpr u32 STATE_CLEAN = 1, STATE_MOUNTED = 2;

// Inode mode: POSIX type bits in the top nibble, permissions below.
constexpr u16 T_MASK = 0xF000, T_FILE = 0x8000, T_DIR = 0x4000, T_LINK = 0xA000;

// Directory record types.
constexpr u8 REC_FILE = 1, REC_DIR = 2, REC_LINK = 3;

struct SuperBlock {
    char magic[8];
    u32 version;
    u32 block_size;
    u64 total_blocks;
    u64 free_blocks;
    u32 inode_count;
    u32 free_inodes;
    u32 root_inode;
    u32 bitmap_blocks;
    u64 bitmap_start;
    u64 inode_table_start;
    u32 inode_table_blocks;
    u32 journal_blocks;
    u64 journal_start;
    u64 data_start;
    u8 uuid[16];
    char label[32];
    u32 mount_count;
    u32 state;
    u64 last_mount_time;
    u8 reserved[68];
    u32 crc;                    // CRC32C of this struct with crc = 0
};
static_assert(sizeof(SuperBlock) == 224, "superblock layout");

struct Inode {
    u16 mode;                   // 0 = free
    u16 links;
    u32 uid;
    u32 gid;
    u32 flags;
    u64 size;
    u64 atime, mtime, ctime;
    u32 direct[DIRECT];
    u32 indirect;
    u32 dindirect;
    u32 generation;
    u8 reserved[12];
    u32 crc;                    // CRC32C of the inode with crc = 0
};
static_assert(sizeof(Inode) == INODE_SIZE, "inode layout");

struct BlockHeader {
    u32 magic;
    u32 crc;                    // CRC32C of the whole block with crc = 0
    u32 owner;                  // bitmap: index; dir/indirect: inode; journal: low 32 bits of the sequence
    u32 extra;                  // indirect: level (1 or 2); dir: 0; journal: count
};
static_assert(sizeof(BlockHeader) == HEADER, "header layout");

// One name in a directory block (after the header). rec_len covers the
// record and the free space after it, so the records chain through the
// block; inode 0 marks an unused record.
struct DirRec {
    u32 inode;
    u16 rec_len;
    u8 name_len;
    u8 type;
    char name[];
};
constexpr u32 DIRREC_MIN = 8;
inline u32 dirrec_size(u32 name_len) { return (DIRREC_MIN + name_len + 3) & ~3u; }

// Journal header (journal_start) and transaction blocks (journal_start + 1 ...).
struct JournalHeader {
    BlockHeader h;              // magic MAGIC_JHEAD
    u64 sequence;               // the next transaction's number
};
struct JournalDesc {
    BlockHeader h;              // magic MAGIC_JDESC, owner = sequence, extra = count
    u64 sequence;
    u64 targets[(BLOCK - HEADER - 8) / 8];   // home block of each image that follows
};
constexpr u32 JOURNAL_MAX_TX = (BLOCK - HEADER - 8) / 8;   // 509 blocks per transaction
struct JournalCommit {
    BlockHeader h;              // magic MAGIC_JCOMMIT, owner = sequence, extra = count
    u64 sequence;
    u32 images_crc;             // CRC32C over the images, in order
};

// ---------------------------------------------------------------- checks --
inline u32 super_crc(const SuperBlock& s) {
    SuperBlock c = s;
    c.crc = 0;
    return crc32c(0, &c, sizeof c);
}

inline u32 inode_crc(const Inode& i) {
    Inode c = i;
    c.crc = 0;
    return crc32c(0, &c, sizeof c);
}

// CRC32C of a block with its header's crc field (bytes 4-7) taken as zero.
inline u32 block_crc(const u8* block) {
    u32 zero = 0;
    u32 c = crc32c(0, block, 4);
    c = crc32c(c, &zero, 4);
    return crc32c(c, block + 8, BLOCK - 8);
}

inline void seal_block(u8* block) {
    u32 c = block_crc(block);
    __builtin_memcpy(block + 4, &c, 4);
}

inline bool block_ok(const u8* block, u32 magic, u32 owner) {
    const BlockHeader* h = (const BlockHeader*)block;
    return h->magic == magic && h->owner == owner && h->crc == block_crc(block);
}

// The superblock describes a consistent layout that fits in `device_blocks`.
inline bool check_super(const SuperBlock& s, u64 device_blocks) {
    for (int i = 0; i < 8; i++)
        if (s.magic[i] != MAGIC[i]) return false;
    if (s.version != VERSION || s.block_size != BLOCK || s.crc != super_crc(s)) return false;
    if (s.total_blocks < 16 || s.total_blocks > device_blocks || s.total_blocks > 0xFFFFFFFFull) return false;
    if (s.bitmap_start != 1) return false;
    if ((u64)s.bitmap_blocks * BITS_PER_BITMAP_BLOCK < s.total_blocks) return false;
    if (s.inode_table_start != s.bitmap_start + s.bitmap_blocks) return false;
    if (s.inode_count == 0 || s.inode_count != (u64)s.inode_table_blocks * INODES_PER_BLOCK) return false;
    if (s.journal_start != s.inode_table_start + s.inode_table_blocks) return false;
    if (s.journal_blocks < 4) return false;
    if (s.data_start != s.journal_start + s.journal_blocks || s.data_start >= s.total_blocks) return false;
    if (s.root_inode != ROOT_INODE || s.free_blocks > s.total_blocks || s.free_inodes > s.inode_count) return false;
    return true;
}

// A block number found in an inode or indirect block: 0 (a hole) or a data block.
inline bool data_block_ok(const SuperBlock& s, u64 b) { return b == 0 || (b >= s.data_start && b < s.total_blocks); }

// A free inode (mode 0) is accepted as it is: its contents are never used,
// and allocation overwrites all of it.
inline bool check_inode(const SuperBlock& s, const Inode& i) {
    if (i.mode == 0) return true;
    if (i.crc != inode_crc(i)) return false;
    u16 t = i.mode & T_MASK;
    if (t != T_FILE && t != T_DIR && t != T_LINK) return false;
    if (i.size > MAX_FILE_SIZE) return false;
    if (t == T_DIR && i.size % BLOCK) return false;
    if (t == T_LINK && i.size >= BLOCK) return false;
    for (u32 k = 0; k < DIRECT; k++)
        if (!data_block_ok(s, i.direct[k])) return false;
    return data_block_ok(s, i.indirect) && data_block_ok(s, i.dindirect);
}

// Walks the records of a directory block. Returns false at the first
// malformed record; `fn(rec, offset)` returns false to stop early.
template <typename Fn> bool each_dirrec(const u8* block, u32 inode_count, Fn fn) {
    u32 off = HEADER;
    while (off < BLOCK) {
        if (BLOCK - off < DIRREC_MIN) return false;
        const DirRec* r = (const DirRec*)(block + off);
        if (r->rec_len < DIRREC_MIN || r->rec_len % 4 || r->rec_len > BLOCK - off) return false;
        if (r->inode) {
            if (r->inode > inode_count || r->name_len == 0 || dirrec_size(r->name_len) > r->rec_len) return false;
            if (r->type != REC_FILE && r->type != REC_DIR && r->type != REC_LINK) return false;
            for (u32 k = 0; k < r->name_len; k++)
                if (r->name[k] == '/' || r->name[k] == 0) return false;
        }
        if (!fn(r, off)) return true;
        off += r->rec_len;
    }
    return off == BLOCK;
}

inline u32 inode_block(const SuperBlock& s, u32 ino) { return (u32)(s.inode_table_start + (ino - 1) / INODES_PER_BLOCK); }
inline u32 inode_offset(u32 ino) { return (ino - 1) % INODES_PER_BLOCK * INODE_SIZE; }
inline u64 bitmap_block_of(const SuperBlock& s, u64 b) { return s.bitmap_start + b / BITS_PER_BITMAP_BLOCK; }

} // namespace cerfs
