// Builds the file tree at boot (SPEC phase 9):
//   /       the initramfs: the boot archive unpacked into a tmpfs, read-only
//   /dev    devfs: device nodes (null, zero, random, urandom, console, tty0,
//           fb0, input/kbd0, input/mouse0, disk/sd*), noexec,nosuid
//   /tmp    tmpfs, nosuid,nodev
// and starts the page cache's write-back thread. Thread context, after the
// disk drivers have registered their devices.
#pragma once

void fs_init();
