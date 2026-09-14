# Status

Updated at the end of every session. Read this first (docs/SPEC.md §18).

## Current phase

**Phase 1 — Boot and output: COMPLETE (2026-09-14).**
Next up: **Phase 2 — CPU structures and interrupts.**

## What works

- Host environment: WSL2 Ubuntu 24.04 (`wsl -d Ubuntu-24.04`) with
  build-essential, nasm, xorriso, qemu-system-x86 8.2, gdb, python3, OVMF.
- Cross toolchain: binutils 2.42 + GCC 13.3.0 for x86_64-elf in
  `toolchain/out/`, built by `toolchain/build-cross.sh` (resumable).
- `make` builds `build/kernel/lumen.elf` (higher-half, entry
  0xFFFFFFFF80000000) and `build/kernel/kernel.sym`.
- `make iso` builds a hybrid BIOS/UEFI ISO with Limine 11.4.1.
- `make run` / `make run-uefi` / `make run-headless` / `make debug` /
  `make gdb` / `make vbox` / `make clean` / `make check-tools`.
- `make test` runs every `tests/integration/*.expect` through
  `tools/qemu-probe.py` (headless QEMU + QMP: RIP must be in the kernel,
  expected serial lines must appear in order). Currently: `boot-banner` PASS.
- Phase 1 kernel:
  - Limine base revision 3 requests: bootloader info, memory map, HHDM,
    framebuffer, executable address, RSDP, modules, MP. Copied into
    `g_boot_info` (`kernel/boot/bootinfo.h`).
  - COM1 serial (polled), `kprintf` family (`kernel/lib/kprintf.cpp`),
    console fan-out to serial + framebuffer.
  - PSF2 framebuffer console with the embedded Terminus 8x16 font
    (scroll, colour, cursor). Refuses non-32bpp framebuffers.
  - CPUID vendor/brand, boot banner with memory map summary.
- Verified on QEMU (BIOS and OVMF) and VirtualBox 7.2 (EFI): banner on
  serial and on screen (`build/phase1-screen.png` via QMP screendump);
  QEMU's `info mtree -f` matches the printed memory map.

## Half-done

Nothing.

## Next (phase 2)

1. GDT (kernel/user code+data, TSS), IDT with 256 entries and named
   exception handlers, IST stacks for #DF/#NMI/#MC.
2. Exception dump: name, decoded error code (#PF: CR2 + flags), full
   registers, RIP with symbol lookup, RBP backtrace.
3. Embedded symbol table generated from `kernel.sym` at build time.
4. PIC remap+mask, Local APIC, I/O APIC, APIC timer calibrated by the PIT
   at 100 Hz with a visible tick counter.
5. A minimal kernel shell over serial (`test exceptions`, `ticks`).

## Known bugs

- `make` on the Windows-mounted tree occasionally prints
  "Clock skew detected" (drvfs timestamp rounding). Harmless so far.
- VirtualBox's serial log gets the Limine menu with no newline before the
  first kernel line; cosmetic.
