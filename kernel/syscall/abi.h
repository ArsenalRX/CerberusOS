// Structures and constants shared with user programs through system calls.
// userland/libc/include/cerberus.h repeats them; the two must match (the
// values follow Linux where Linux has one, so ported code needs no changes).
#pragma once

#include <lib/types.h>

namespace abi {

// open() flags.
constexpr u32 O_RDONLY = 0, O_WRONLY = 1, O_RDWR = 2, O_ACCMODE = 3;
constexpr u32 O_CREAT = 0x40, O_EXCL = 0x80, O_TRUNC = 0x200, O_APPEND = 0x400, O_NONBLOCK = 0x800;
constexpr u32 O_DIRECTORY = 0x10000, O_NOFOLLOW = 0x20000, O_CLOEXEC = 0x80000;
constexpr u32 O_KNOWN = O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_NONBLOCK | O_DIRECTORY | O_NOFOLLOW |
                        O_CLOEXEC;

// The *at calls: a directory descriptor, or the working directory.
constexpr i32 AT_FDCWD = -100;
constexpr u32 AT_SYMLINK_NOFOLLOW = 0x100;

// seek() whence.
constexpr u32 SEEK_SET = 0, SEEK_CUR = 1, SEEK_END = 2;

// File type in Stat::mode.
constexpr u32 S_IFMT = 0170000, S_IFDIR = 0040000, S_IFCHR = 0020000, S_IFBLK = 0060000, S_IFREG = 0100000,
              S_IFLNK = 0120000;

struct Stat {
    u64 ino;
    u64 size;
    u64 blocks;                 // 512-byte units allocated
    u64 atime, mtime, ctime;    // seconds since 1970-01-01 UTC
    u32 mode;                   // S_IF* | permission bits
    u32 nlink;
    u32 uid, gid;
    u32 rdev;                   // device nodes: major << 16 | minor
    u32 blksize;                // preferred I/O size
};

// Directory entry types.
constexpr u8 DT_UNKNOWN = 0, DT_CHR = 2, DT_DIR = 4, DT_BLK = 6, DT_REG = 8, DT_LNK = 10;

struct Dirent {
    u64 ino;
    u8 type;                    // DT_*
    u8 reserved[7];
    char name[256];             // NUL-terminated
};

// mount() flags (vfs::MNT_* values).
constexpr u32 MS_RDONLY = 1, MS_NOEXEC = 2, MS_NOSUID = 4, MS_NODEV = 8;

} // namespace abi
