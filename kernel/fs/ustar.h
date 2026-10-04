// Read-only lookup in a USTAR (tar) archive held in memory: the boot
// archive that carries the first user programs until the VFS exists
// (phase 9, where it becomes the initramfs).
//
// The archive is untrusted input: every header field is bounds-checked
// against the archive size, sizes are parsed with overflow checks, and a
// malformed archive yields "not found", never an out-of-range pointer. This
// file depends only on lib/types.h so the same code is built for the host
// and fuzzed (`make fuzz`).
#pragma once

#include <lib/types.h>

struct UstarEntry {
    const u8* data;     // points into the archive
    usize size;
    u32 mode;           // permission bits from the header
    bool is_dir;
};

// Finds a regular file or directory by path. Leading "/" and "./" on either
// side are ignored. Returns false if absent or if the archive is damaged
// before the entry is reached.
bool ustar_find(const u8* archive, usize archive_size, const char* path, UstarEntry* out);

// Calls fn(name, entry, ctx) for each entry until fn returns false or the
// archive ends. Returns the number of entries visited.
usize ustar_each(const u8* archive, usize archive_size, bool (*fn)(const char* name, const UstarEntry& e, void* ctx),
                 void* ctx);
