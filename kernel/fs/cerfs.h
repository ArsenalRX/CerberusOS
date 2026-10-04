// cerfs: Cerberus's on-disk file system (SPEC phase 9). Format in
// fs/cerfs_format.h; this is the kernel driver behind `mount -t cerfs`.
//
// Consistency: every metadata change (superblock, bitmap, inodes,
// directories, indirect blocks) joins the running transaction and is held in
// the page cache. A commit (sync, fsync, unmount, the 5 s write-back, or a
// full transaction) first writes the files' data, then the changed metadata
// blocks to the journal with a commit record, and only then to their home
// locations; finally the journal is marked empty. After a crash, mount
// replays a complete transaction found in the journal and ignores an
// incomplete one, so the file system is always as it was at some commit.
//
// The disk is untrusted: every field is checked before use (cerfs_format.h);
// damage is reported as Error::IO and logged, never a panic.
#pragma once

// Registers the "cerfs" type with the VFS.
void cerfs_register();
