// AHCI (SATA) disks with DMA: the controller of QEMU's q35 machine, of
// VirtualBox's SATA option and of real PCs (SPEC phase 10, brought forward
// to carry the file system on q35). Commands complete by interrupt (MSI)
// where the controller offers it, else by polling. ATAPI
// devices (CD drives) are skipped.
//
// Everything read from the controller and the disk (port counts, sector
// counts, status) is checked before use; a command that does not finish
// within 5 s is an I/O error.
#pragma once

// Registers the driver; the driver model attaches it to each AHCI controller,
// and every SATA disk found becomes a block device.
void ahci_register();
