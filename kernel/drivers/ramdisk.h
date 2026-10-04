// A disk in memory, registered as a block device like any other. Used by
// the file-system fuzzer and tests, which change its contents directly.
#pragma once

#include <lib/result.h>
#include <lib/types.h>

struct RamDisk {
    u8* data;
    u64 bytes;
    u32 minor;
};

// Creates and registers a RAM disk of `bytes` (a multiple of 4096).
// Errors: NoMemory, NoSpace (no block-device slot left).
Result<RamDisk*> ramdisk_create(u64 bytes);
