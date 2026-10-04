# Installer, USB media and dual boot — research notes

**What this file is for.** Research and a proposed design for the owner's
request of 2026-10-04: a simple installer like Windows Setup that asks which
drive to install on, either erases the old system or keeps it, and a boot
menu that lets the user pick Cerberus, Windows, and so on. It feeds phase 18
(docs/SPEC.md §5, docs/TO_FINISH.md). Nothing here is decided until the
owner approves phase 18's contents.

**When to update it.** When phase 18 is planned or started, and whenever a
finding below turns out to be wrong.

---

## What phase 18 already plans

Live-ISO installer (GPT, cerfs + a FAT32 EFI partition, copy the system,
install Limine, create a user), optional full-disk encryption, signed
updates with a fallback kernel, a recovery boot entry, `fsck.cerfs`. Its
acceptance test installs to a **blank** disk. Not planned yet: keeping an
existing system (dual boot), a boot menu entry for Windows, UEFI boot
entries, and making a bootable USB stick.

## How other installers ask the question

- **Windows Setup** ("Custom: install Windows only"): lists every disk's
  partitions and unallocated space with Delete / New / Format / Extend.
  Choosing unallocated space makes Setup create the partitions it needs;
  deleting everything is the "wipe" path.
- **Ubuntu**: "Install alongside Windows / Erase disk / Manual". It refuses
  "alongside" while BitLocker is on.
- **Calamares** (many Linux distributions): Erase disk / Replace a partition
  / Install alongside / Manual.
- **Fedora Anaconda**: use all space / use free space / shrink or remove.

**Proposed for Cerberus** (simpler than all of these):

1. **Pick a disk.** Each disk shows model, size, bus, its partitions and
   the operating systems found on it (Windows: `\EFI\Microsoft\Boot\bootmgfw.efi`
   on the EFI partition, or an NTFS volume). The USB stick Cerberus booted from
   is not offered.
2. **Pick what to do:**
   - **Erase this disk and install Cerberus** — a fresh partition table.
   - **Install Cerberus alongside the existing system** — uses unallocated
     space (at least about 8 GiB) and the existing EFI partition.
3. **Review the plan**: a list of every change ("create partition 5, FAT32,
   512 MiB", "add \EFI\Cerberus\ to the existing EFI partition", "add boot
   entry 'Cerberus'"). Erasing asks the user to type the disk's name.
4. User account, progress bar, reboot.

Cerberus will **not shrink Windows' partition itself**. Resizing NTFS is large
and risky work. If there is too little free space, the installer explains how
to shrink the volume in Windows Disk Management and stops. No manual
partition editor at first.

## Bootable USB stick from Windows

The ISO is already a hybrid image: it has an MBR with Limine's BIOS loader
and an EFI partition. Writing it to a stick byte for byte should boot on
both BIOS and UEFI machines. **Not yet tried on real hardware.**

- **Rufus**: use **"DD image" mode**. Its default "ISO image" mode copies
  files and installs its own loader. It does not know Limine, so BIOS boot
  would fail.
- **balenaEtcher**: always writes byte for byte, so it is the simplest
  instruction.
- Proposed: document both, with screenshots. A Cerberus-made Windows tool is a
  later nice-to-have, not part of phase 18.
- **Consequence:** Cerberus has no USB driver (phase 19), so the live installer
  must load everything it installs into memory at boot (as Limine modules,
  as `initrd.tar` is loaded today) and never read the stick afterwards.

## Boot menu and dual boot (UEFI)

- **Limine is the menu.** It cannot read cerfs, only FAT and ISO9660. The
  kernel therefore lives on a small FAT32 **Cerberus boot partition** (about
  512 MiB); cerfs holds the system and home. Windows' EFI partition (often
  only 100 MB) gets only the loader.
- **The existing EFI partition is shared, never formatted.** Cerberus adds
  `\EFI\Cerberus\BOOTX64.EFI` and `\EFI\Cerberus\limine.conf` and does not
  overwrite `\EFI\BOOT\BOOTX64.EFI` when Windows is present.
- **Menu entries** (Limine config syntax):

  ```
  timeout: 5
  remember_last_entry: yes
  /Cerberus
      protocol: limine
      path: guid(<cerberus-boot-partition-uuid>):/cerberus.elf
      module_path: guid(<cerberus-boot-partition-uuid>):/initrd.tar
  /Cerberus (previous kernel)
  /Cerberus recovery
  /Windows
      protocol: efi
      path: guid(<efi-partition-uuid>):/EFI/Microsoft/Boot/bootmgfw.efi
  ```

- **Firmware boot entry.** For the PC to start Limine, the installer writes
  a UEFI variable `Boot####` ("Cerberus" → `\EFI\Cerberus\BOOTX64.EFI`) and puts
  it first in `BootOrder` (checkbox "start Cerberus's menu by default"; the
  old order is backed up). This needs UEFI runtime services in the kernel,
  which is new work.
- **Pitfalls the installer must handle:**
  - **BitLocker**: changing the boot order can make Windows ask for its
    48-digit recovery key. Detect BitLocker and refuse "alongside" until the
    user has suspended it and has the key.
  - **Secure Boot**: Cerberus's loader is unsigned. The user must turn Secure
    Boot off; signing comes later.
  - **Fast Startup / hibernation** leaves Windows' disk "in use". Tell the
    user to turn Fast Startup off before shrinking.
  - **Windows updates** sometimes move Windows Boot Manager back to first
    place. The firmware's boot menu (F12 or similar) still lists Cerberus; the
    recovery entry gets "repair boot entry".
  - **Matching boot mode**: Windows on a GPT disk means UEFI, so no BIOS
    dual boot on such disks.

Legacy BIOS/MBR machines: erase installs work (Limine's BIOS installer);
dual boot beside an MBR Windows is possible (`protocol: bios`) but
second-class. The original MBR is backed up first.

## Safety rules

- Nothing is written to any disk before the final confirmation; the live
  system mounts nothing read-write.
- Pre-checks abort on any doubt: BitLocker, hibernated Windows, too little
  space, damaged partition table, wrong boot mode.
- Write order: new partitions → partition table → EFI files → firmware
  boot entry last. Every action is logged to the Cerberus boot partition.
- Test: a QEMU disk with a fake Windows layout. After installing, every byte
  outside the plan must be unchanged.

## What has to exist first

- Phase 9: cerfs and `mkfs.cerfs`.
- Phase 10: disk drivers (AHCI, virtio-blk). **NVMe must be added** before
  installing on a real PC, because most current PCs use NVMe disks. It is
  not in phase 10 yet.
- In phase 18: partition-table reader/writer (GPT and MBR), FAT32 writer,
  OS detection, the dry-run plan, Limine installation, UEFI variables, the
  installer screens.
- Real-hardware testing is phase 19.

## Sources

- Limine configuration and usage: https://codeberg.org/Limine/Limine/raw/branch/v11.x/CONFIG.md,
  https://codeberg.org/Limine/Limine/raw/branch/v11.x/USAGE.md
- UEFI boot manager variables: https://uefi.org/specs/UEFI/2.10/03_Boot_Manager.html
- Ubuntu and BitLocker: https://documentation.ubuntu.com/desktop/en/latest/reference/bitlocker-during-ubuntu-installation
- Windows Setup partitions: https://www.ctrl.blog/entry/how-to-esp-windows-setup
- Fedora disk reclaim: https://www.fedoraproject.org/wiki/Changes/Anaconda_Reclaim_Disk_Space
- BitLocker recovery triggers: https://learn.microsoft.com/zh-cn/archive/blogs/askcore/issues-resulting-in-bitlocker-recovery-mode-and-their-resolution
