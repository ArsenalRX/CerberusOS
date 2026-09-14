# Status

Updated at the end of every session. Read this first (docs/SPEC.md §18).

## Current phase

**Phase 2 — CPU structures and interrupts: COMPLETE (2026-09-14).**
Next up: **Phase 3 — Physical memory.**

## What works

- Host environment: WSL2 Ubuntu 24.04 (`wsl -d Ubuntu-24.04`) with
  build-essential, nasm, xorriso, qemu-system-x86 8.2, gdb, python3, OVMF.
- Cross toolchain: binutils 2.42 + GCC 13.3.0 for x86_64-elf in
  `toolchain/out/`, built by `toolchain/build-cross.sh` (resumable).
- Build: `make` (two-pass link embedding the symbol table), `make iso`
  (hybrid BIOS/UEFI, Limine 11.4.1), `make dist` (snapshot ISO in `dist/`).
- Run: `make run` / `run-uefi` / `run-headless` / `debug` / `gdb`;
  `make vbox` boots the VirtualBox VM "Lumen" (EFI, 4 CPUs, 1 GiB, VMSVGA)
  and archives the previous serial log; `make vbox-log` prints the last
  VirtualBox run's serial log (`logs/vbox-serial.log`, written on every VM
  run, including runs started from the VirtualBox GUI).
- Test: `make test` runs every `tests/integration/*.expect` through
  `tools/qemu-probe.py` (headless QEMU + QMP; `!send` types over serial,
  `!key` through the emulated PS/2 keyboard). All 6 pass:
  boot-banner, shell-tests, keyboard, exception-de, exception-pf, exception-ud.
- Phase 1: Limine base revision 3 requests → `g_boot_info`; COM1 serial;
  `kprintf`; PSF2 framebuffer console (Terminus 8x16) with a text-cell shadow
  buffer (never reads the framebuffer back); boot banner.
- Phase 2:
  - GDT with kernel/user code+data and a TSS per CPU (BSP loaded), IST
    stacks for #DF/#NMI/#MC; selector layout matches syscall/sysret.
  - IDT with 256 gates (NASM stubs), `interrupt_register`, exception dump:
    name, decoded error code (#PF: CR2 + flags; selector errors decoded),
    all registers, CR0-4, RIP with symbol, RBP backtrace with symbols.
  - Embedded symbol table (`tools/gensyms.py`, `.ksymtab` section, two-pass
    link with an address-stability check).
  - PIC remapped and masked; ACPI RSDP/RSDT/XSDT walk, MADT parse;
    Local APIC enabled (xAPIC MMIO via `early_map`); I/O APIC with ISA routes
    honouring interrupt source overrides; APIC timer calibrated against the
    PIT (channel 2), periodic at 100 Hz with a tick counter.
  - `kernel/mm/early_map.cpp`: maps MMIO/ACPI pages into the vmalloc region
    using the bootloader's page tables.
  - Kernel shell over serial and the early PS/2 keyboard driver:
    `help ticks mem sym test panic halt reboot`; self-tests `kprintf`,
    `timer`, `exceptions <de|ud|pf|pfw|gp|bp>` (`test all` skips the
    halting one). PANIC prints registers + backtrace.
- Verified: QEMU (BIOS) via `make test`; VirtualBox 7.2 EFI boots to the
  shell, keyboard input works, APIC timer calibrates (~515 MHz bus).

## Half-done

Nothing.

## Next (phase 3)

1. Bitmap physical frame allocator over the Limine memory map:
   `pmm_alloc(count)`, `pmm_free`, `pmm_stats`, first-fit contiguous runs.
2. Reserve kernel image, bootloader structures (until reclaimed),
   framebuffer, the bitmap itself.
3. `test pmm`: 10,000 random-order alloc/free with bitmap and stats
   verification; allocate-everything-then-free.
4. Consider reclaiming bootloader memory after copying what `early_map`
   needs (page tables live there).

## Known bugs

- `make` on the Windows-mounted tree occasionally prints
  "Clock skew detected" (drvfs timestamp rounding). Harmless so far.
- Backtraces cannot name a function that faults inside its own prologue
  (inherent to RBP walking); tests avoid it by making a call before faulting.
- VirtualBox's serial log gets the Limine menu with no newline before the
  first kernel line; cosmetic.
