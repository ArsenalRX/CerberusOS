// Shared between the library's own source files; not for programs.
#pragma once

#include <cerberus.h>

// The thread control block. The thread pointer (the FS base) points at it,
// and the thread's copy of the program's thread-local variables sits
// directly below it, as the x86-64 ABI lays them out.
struct __tcb {
    struct __tcb* self;         // %fs:0, which the ABI requires to hold the thread pointer
    void* map;                  // the mapping holding this thread's stack and variables (null: the first thread)
    size_t map_size;
    int tid;
    int reserved;
    void* (*fn)(void*);
    void* arg;
    void* result;
};

extern "C" {
// Finds the program's thread-local template (its PT_TLS segment). `base` is
// where the image was loaded.
void __tls_locate(uint64_t base);
// Bytes needed for one thread's variables and control block.
size_t __tls_area_size(void);
// Sets up a thread's variables and control block in `area` (page-aligned,
// zero-filled, __tls_area_size() bytes) and returns the thread pointer.
struct __tcb* __tls_init(void* area);
}
