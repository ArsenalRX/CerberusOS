// Device nodes. A device node (a CharDev or BlockDev vnode, normally under
// /dev) names a driver by its device number: major << 16 | minor. Drivers
// register the operations for their major number here; opening, reading and
// writing a device node go through this switch.
//
// Character devices are called without the VFS lock (a console read may
// wait). Block devices are byte-addressed through the block layer
// (fs/block.h) and always go through the page cache.
#pragma once

#include <lib/result.h>
#include <lib/types.h>

struct Vnode;
struct VnodeOps;

namespace dev {
constexpr u32 make(u32 major, u32 minor) { return major << 16 | minor; }
constexpr u32 major_of(u32 rdev) { return rdev >> 16; }
constexpr u32 minor_of(u32 rdev) { return rdev & 0xFFFF; }

// Major numbers (Linux's where one exists, so tools look familiar).
constexpr u32 MEM = 1;          // minor: NULL_ 3, ZERO 5, RANDOM 8, URANDOM 9
constexpr u32 TTY = 5;          // minor: CONSOLE 1, TTY0 0
constexpr u32 DISK = 8;         // minor: disk index
constexpr u32 INPUT = 13;       // minor: KBD 0, MOUSE 1
constexpr u32 FB = 29;          // minor: 0

constexpr u32 NULL_ = 3, ZERO = 5, RANDOM = 8, URANDOM = 9;
constexpr u32 CONSOLE = 1, TTY0 = 0;
constexpr u32 KBD = 0, MOUSE = 1;
} // namespace dev

struct CharDeviceOps {
    // May the device be opened at all (beyond the node's permission bits)?
    // Null = yes.
    Result<void> (*open)(u32 minor);
    Result<usize> (*read)(u32 minor, u64 offset, void* buf, usize n);
    Result<usize> (*write)(u32 minor, u64 offset, const void* buf, usize n);
    Result<i64> (*ioctl)(u32 minor, u32 request, u64 arg);
};

void dev_register_char(u32 major, const CharDeviceOps* ops);
// The operations behind device vnodes (tmpfs and devfs use them for their
// CharDev and BlockDev nodes).
const VnodeOps* dev_vnode_ops();
// Called by the file layer when a device node is opened. NoDevice if no
// driver claims it.
Result<void> dev_open(Vnode* v);

// Registers the memory devices (null, zero, random, urandom) and the console.
void dev_init();
