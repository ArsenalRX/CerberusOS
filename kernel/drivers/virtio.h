// The virtio transport over PCI ("modern", virtio 1.0+): finding a device's
// configuration structures, negotiating features, and split virtqueues.
// Written once for every virtio device (virtio-blk now, virtio-net in
// phase 14). Polled: the caller asks whether a request has completed.
//
// The device is not trusted: queue sizes, offsets and the ids it returns in
// the used ring are checked before use.
#pragma once

#include <drivers/pci.h>
#include <lib/result.h>
#include <lib/types.h>

namespace virtio {

constexpr u16 VENDOR = 0x1AF4;
constexpr u64 F_VERSION_1 = 1ull << 32;

constexpr u16 DESC_NEXT = 1, DESC_WRITE = 2;    // WRITE: the device writes into this buffer

struct Desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};

struct Queue {
    u16 index;
    u16 size;                   // entries (a power of two, at most 64 here)
    Desc* desc;
    volatile u16* avail;        // flags, idx, ring[size]
    volatile u16* used;         // flags, idx, then {u32 id, u32 len}[size]
    u16 last_used;
    volatile u16* notify;       // write the queue index here to kick the device
};

struct Device {
    const PciDevice* pci;
    volatile u8* common;        // common configuration
    volatile u8* device_cfg;    // device-specific configuration
    u32 device_cfg_len;
    volatile u8* notify_base;
    u32 notify_multiplier;
    u64 features;               // negotiated
};

// Maps the device's structures, resets it and negotiates `wanted` features
// (VERSION_1 is required and added). Errors: NoDevice (no modern
// capabilities), NotSupported (the device refuses the features).
Result<void> init(Device* d, const PciDevice* pci, u64 wanted);
// Sets up queue `index` with up to 64 entries. Errors: NoDevice, NoMemory.
Result<void> queue_setup(Device* d, Queue* q, u16 index);
// Tells the device initialisation is complete.
void driver_ok(Device* d);
// Marks the device failed (after an unrecoverable error).
void fail(Device* d);

// Offers the descriptor chain starting at `head` (already filled in
// q->desc) to the device and notifies it.
void submit(Queue* q, u16 head);
// True once the device has returned a chain; *head receives its first
// descriptor (checked to be below the queue size).
bool poll_used(Queue* q, u16* head);

} // namespace virtio
