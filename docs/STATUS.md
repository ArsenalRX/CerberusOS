# Status

Updated at the end of every session. Read this first (docs/SPEC.md §18).

## Current phase

**Phase 2 — CPU structures and interrupts: COMPLETE (2026-09-14).**
Next up: **Phase 3 — Physical memory.**

## How to run it

- `run-lumen.cmd` (double-click on Windows): boots the ISO in QEMU with KVM
  inside WSL2, window on the desktop via WSLg, serial log in
  `logs/qemu-serial.log`. `run-lumen.cmd uefi` boots through OVMF;
  `run-lumen.cmd build` rebuilds first. This is the fast, accurate path.
- `dist/lumen-0.0.1.iso`: snapshot ISO for any VM (`make dist` refreshes it).
- VirtualBox: the owner's VM is "Lumen 0.0.1" (COM1 → `logs/vbox-serial.log`,
  HPET on). **Caveat:** on this host VirtualBox runs on the Hyper-V backend
  ("AMD-V is not available" in VBox.log, because WSL2/Docker keep Hyper-V
  on), which makes the guest laggy and its clock run ~3x slow while idle.
  See docs/DECISIONS.md. "Lumen-dev" is a headless VM used for automated
  VirtualBox checks (log `logs/vbox-dev-serial.log`).

## What works

- Host environment: WSL2 Ubuntu 24.04 (`wsl -d Ubuntu-24.04`) with
  build-essential, nasm, xorriso, qemu-system-x86 8.2, gdb, python3, OVMF;
  /dev/kvm available (nested virtualisation).
- Cross toolchain: binutils 2.42 + GCC 13.3.0 for x86_64-elf in
  `toolchain/out/`, built by `toolchain/build-cross.sh` (resumable).
- Build: `make` (two-pass link embedding the symbol table), `make iso`
  (hybrid BIOS/UEFI, Limine 11.4.1), `make dist`.
- Run: `make run` / `run-uefi` / `run-headless` / `debug` / `gdb` (KVM when
  `/dev/kvm` exists, else the spec's TCG line; `QEMU_ACCEL=tcg` forces TCG);
  `make vbox` / `make vbox-log`.
- Test: `make test` runs every `tests/integration/*.expect` through
  `tools/qemu-probe.py` (headless QEMU + QMP; `!send` types over serial,
  `!key` through the emulated PS/2 keyboard; `--no-hpet` exercises the PIT
  path; `--hmp` runs monitor commands). All 6 pass under KVM and TCG:
  boot-banner, shell-tests, keyboard, exception-de, exception-pf, exception-ud.
- Phase 1: Limine base revision 3 requests → `g_boot_info`; COM1 serial;
  `kprintf`; PSF2 framebuffer console (Terminus 8x16) with a text-cell shadow
  buffer and batched repaints; boot banner.
- Phase 2:
  - GDT with kernel/user code+data and a TSS per CPU (BSP loaded), IST
    stacks for #DF/#NMI/#MC; selector layout matches syscall/sysret.
  - IDT with 256 gates (NASM stubs), `interrupt_register`, exception dump:
    name, decoded error code (#PF: CR2 + flags; selector errors decoded),
    all registers, CR0-4, RIP with symbol, RBP backtrace with symbols.
  - Embedded symbol table (`tools/gensyms.py`, `.ksymtab` section, two-pass
    link with an address-stability check).
  - PIC remapped and masked; ACPI RSDP/RSDT/XSDT walk, MADT parse; HPET
    driver; Local APIC (xAPIC MMIO via `early_map`); I/O APIC with ISA routes
    honouring interrupt source overrides; APIC timer: estimate from the count
    register against the reference clock (HPET, or PIT channel 0 counter),
    then closed-loop correction of the delivered rate; 100 Hz tick counter;
    measured rate printed in the "boot: OK" summary line.
  - `kernel/mm/early_map.cpp`: maps MMIO/ACPI pages into the vmalloc region
    using the bootloader's page tables.
  - Kernel shell over serial and the early PS/2 keyboard driver:
    `help ticks mem sym test idle timermode panic halt reboot`; self-tests
    `kprintf`, `timer`, `idle`, `exceptions <de|ud|pf|pfw|gp|bp>`
    (`test all` skips the halting one). PANIC prints registers + backtrace.
- Verified: QEMU/KVM and QEMU/TCG via `make test` (timer 99.7-100.9 Hz);
  VirtualBox (Hyper-V backend) boots to the shell, keyboard works, timer
  tunes to 100 Hz but guest time lags the host (hypervisor limitation).

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
