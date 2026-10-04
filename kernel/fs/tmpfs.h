// tmpfs: a file system that lives entirely in memory (SPEC phase 9). Used for
// /tmp, for /dev (devfs is a tmpfs holding device nodes) and for the root:
// at boot the initramfs (the boot archive, a USTAR file) is unpacked into a
// tmpfs that is then made read-only.
//
// File contents are kept in whole pages allocated on first write, so sparse
// files cost nothing for their holes. Each instance has a page budget (half
// of RAM by default); a write beyond it fails with NoSpace.
#pragma once

#include <fs/vfs.h>

// Creates a new tmpfs instance (type name recorded as `type`). Registered
// as "tmpfs" for `mount -t tmpfs`.
Result<Mount*> tmpfs_create(const char* type, u32 flags);
// Registers the "tmpfs" type with the VFS.
void tmpfs_register();
// Copies every file and directory of a USTAR archive into the tmpfs `m`,
// creating missing parent directories. Entries that are not regular files
// or directories are skipped. Returns the number of entries added.
Result<usize> tmpfs_populate_tar(Mount* m, const u8* archive, usize size);
// Creates directory `name` in the root of `m` if it is missing (for mount
// points in a read-only root).
Result<void> tmpfs_ensure_dir(Mount* m, const char* name);
