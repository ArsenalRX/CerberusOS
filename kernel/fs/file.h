// Open files, as seen through a process's file descriptors. A File is an
// open vnode with an access mode and a position; it is shared by reference
// count, so fork gives the child the same File objects (one shared offset).
//
// All functions are for thread context. The buffers are kernel buffers; the
// system-call layer does the copying to and from user memory.
#pragma once

#include <fs/vfs.h>
#include <lib/result.h>
#include <lib/types.h>
#include <sched/sync.h>
#include <syscall/abi.h>

struct File {
    u32 refs;
    u32 flags;                  // abi::O_ACCMODE | O_APPEND | O_NONBLOCK
    Vnode* vnode;
    u64 offset;                 // byte offset; for directories, the readdir cookie
    Mutex pos_lock;             // serialises reads and writes that move the offset
};

// The boot archive (the initramfs) found among the bootloader's modules.
bool boot_archive(const u8** data, usize* size);

// Opens `path` relative to `start` (null = root) with abi::O_* flags.
// Errors: those of path resolution, plus Access, IsDir (writing a
// directory), NotDir (O_DIRECTORY), Loop (O_NOFOLLOW on a symlink),
// Exists (O_CREAT|O_EXCL), NoDevice, ReadOnly.
Result<File*> file_open(Vnode* start, const char* path, u32 flags, u32 mode, const Credentials& cred);
// A new File on /dev/console (for a process's standard descriptors).
Result<File*> file_open_console();

File* file_ref(File* f);
// Drops a reference; frees the File (and its vnode reference) when last.
void file_unref(File* f);

bool file_readable(const File* f);
bool file_writable(const File* f);

// Reads up to n bytes at the offset and advances it; 0 at end of file.
Result<usize> file_read(File* f, void* buf, usize n);
// Writes n bytes at the offset (at the end with O_APPEND) and advances it.
Result<usize> file_write(File* f, const void* buf, usize n);
// Moves the offset. Returns the new offset. Invalid (negative result,
// unknown whence), SeekPipe-like devices return Invalid as well.
Result<u64> file_seek(File* f, i64 off, u32 whence);
// Next directory entry (false at the end).
Result<bool> file_readdir(File* f, abi::Dirent* out);
void file_stat(const Vnode* v, abi::Stat* out);
