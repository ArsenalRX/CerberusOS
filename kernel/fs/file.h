// Open files, as seen through a process's file descriptors. Until the VFS
// exists (phase 9) there are two kinds: the console, and read-only files in
// the boot archive. A File is shared by reference count: fork gives the
// child the same File objects, so parent and child share one offset.
//
// All functions are for thread context. The buffers are kernel buffers; the
// system-call layer does the copying to and from user memory.
#pragma once

#include <lib/result.h>
#include <lib/types.h>

enum class FileKind : u8 { Console, Archive };

struct File {
    FileKind kind;
    u32 refs;
    bool readable, writable;
    const u8* data;         // Archive: the file's bytes inside the boot archive
    usize size;
    usize offset;
};

// Finds the boot archive among the bootloader's modules. Call once at boot;
// without an archive every open fails with NotFound.
void files_init();
// True if the boot archive is present.
bool files_archive_present();

// A new reference to the console (readable and writable).
Result<File*> file_open_console();
// Opens a file in the boot archive for reading. Errors: NotFound, IsDir,
// NoMemory.
Result<File*> file_open_archive(const char* path);
// Raw access for the program loader: the bytes of a file in the archive.
// Errors: NotFound, IsDir.
Result<void> file_archive_lookup(const char* path, const u8** data, usize* size, u32* mode);

File* file_ref(File* f);
// Drops a reference; frees the File when it was the last.
void file_unref(File* f);

// Reads up to n bytes at the file's offset and advances it. Returns the
// number read; 0 means end of file. The console has no input yet and always
// returns 0. Error: BadFd if the file is not readable.
Result<usize> file_read(File* f, void* buf, usize n);
// Writes n bytes. Error: BadFd if the file is not writable.
Result<usize> file_write(File* f, const void* buf, usize n);
