// Builds the file tree at boot (SPEC phase 9):
//   /       the initramfs: the boot archive unpacked into a tmpfs, read-only
//   /dev    devfs: device nodes (null, zero, random, urandom, console, tty0,
//           fb0, input/kbd0, input/mouse0, disk/sd*), noexec,nosuid
//   /tmp    tmpfs, nosuid,nodev
// and starts the page cache's write-back thread. Thread context, after the
// disk drivers have registered their devices.
#pragma once

#include <lib/types.h>
#include <lib/result.h>

void fs_init();

// /data: the first disk holding a cerfs volume labelled "data" is mounted
// there at boot, for settings, reminders and the user's own files (B-011).
// Thread context.
bool fs_data_mounted();
// The disk /data is on ("sda"), or "" if none.
const char* fs_data_disk();
// Erases disk `name` (e.g. "sda"), formats it as cerfs with the label
// "data" and mounts it at /data. Refuses a disk that is in use.
Result<void> fs_data_format(const char* name);
// Reports every disk: name, size in MiB, and whether it is the /data disk,
// holds some cerfs volume, or is blank/unknown.
struct DiskInfo {
    char name[8];
    u64 mib;
    enum { Blank, Cerfs, Data, Busy } state;
};
u32 fs_disks(DiskInfo* out, u32 max);
