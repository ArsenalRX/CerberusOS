// ATA (IDE) disks by programmed I/O: the legacy controller that QEMU and
// VirtualBox both emulate. Simple and slow (the CPU moves every word); it
// carries the file system until phase 10 adds AHCI and virtio-blk with DMA.
// ATAPI devices (the CD drive with the boot ISO) are skipped.
//
// Every value read from the device (sector counts, model string) is checked
// before use; a device that does not answer within 5 s is reported as an
// I/O error, never waited on forever.
#pragma once

// Registers the driver; when the driver model attaches it, both legacy
// channels are probed and each disk found becomes a block device.
void ata_register();
