// AHCI (SATA) disks with DMA: the controller of QEMU's q35 machine, of
// VirtualBox's SATA option and of real PCs (SPEC phase 10, brought forward
// to carry the file system on q35). Polled, no interrupts yet. ATAPI
// devices (CD drives) are skipped.
//
// Everything read from the controller and the disk (port counts, sector
// counts, status) is checked before use; a command that does not finish
// within 5 s is an I/O error.
#pragma once

// Finds AHCI controllers on the PCI bus and registers each SATA disk as a
// block device. Requires pci_init and the heap.
void ahci_init();
