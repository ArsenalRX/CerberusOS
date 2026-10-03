# Changelog

What changed in each version of Lumen, newest first.

**How this file is maintained** (docs/SPEC.md §23): every commit that changes
behaviour adds one line under "Unreleased", in the same commit, under one of
the headings Security, Fixed, Performance, Added, Changed, Removed (in that
order; omit empty ones). Lines are written for the person using the OS. At a
release, "Unreleased" is renamed to the version and date and a fresh
"Unreleased" block is added above it. A version with a Security entry is
marked "(security release)". Released blocks are never edited afterwards.

Version numbers: `0.PHASE.PATCH` before 1.0, where PHASE is the last
completed spec phase.

---

## Unreleased

## 0.4.0 — 2026-10-03 (security release)

Spec phase 4, virtual memory, is complete.

### Security
- No memory can be both writable and executable; every request for such a
  mapping is refused.
- All memory outside the kernel's code is now non-executable. Before this
  release the bootloader's mapping left all of RAM executable.
- Kernel code and read-only data can no longer be written; kernel data can
  no longer be executed.
- Memory handed to an address space is always zeroed first, so nothing left
  behind by a previous owner can be read.
- The first 64 KiB of an address space can never be mapped, so a null
  pointer always faults.
- A stack overflow on a guarded kernel stack is reported and stops the
  system instead of overwriting other memory.
- Stack variables are zero-initialised by the compiler; debug builds stop
  with the source line on undefined behaviour such as signed overflow.

### Added
- Virtual memory manager: private address spaces, memory regions, memory
  allocated on first use, copy-on-write copies of an address space, 2 MiB
  pages for large kernel mappings.
- `test vmm` self-test; `test exceptions so` (stack overflow) and
  `test exceptions ub` (undefined behaviour).

### Changed
- The other CPUs now wait inside the kernel instead of inside the
  bootloader's code.

## 0.3.0 — 2026-10-03

First release under the `0.PHASE.PATCH` scheme: spec phases 0–3 are complete.

### Fixed
- Keyboard and mouse can no longer swallow each other's input: both
  interrupts now read the PS/2 controller through one shared routine that
  sends each byte to the right device.
- The desktop no longer freezes on VirtualBox when it runs on the Hyper-V
  backend: while the desktop is up the kernel keeps polling instead of
  halting the CPU, and copies pixels to the screen with plain stores. (This
  uses a full CPU while idle; the scheduler in phase 6 replaces it. Verified
  in QEMU; not yet re-verified on VirtualBox.)

### Added
- Shell commands `gui` (compositor statistics) and `irqs` (interrupt counts).
- The boot log reports how long the first desktop frame took.

### Changed
- Versions are now labelled `0.PHASE.PATCH`; development builds show
  `-dev+<commit>` after the number. The number lives in the `VERSION` file.
- Specification version 2: security, privacy, networking and performance are
  requirements (see `docs/DECISIONS.md`, 2026-10-03).

## 0.0.1 — 2026-09-14

First snapshot. Covers spec phases 0–3 and the desktop preview.

### Added
- Boots through Limine on BIOS and UEFI; boot banner on screen and serial.
- CPU setup: GDT/TSS, interrupts with symbolised crash dumps, ACPI, APIC,
  HPET, a 100 Hz timer, real-time clock.
- Physical memory allocator with a self-test.
- Kernel shell with PS/2 keyboard input.
- Desktop preview: windows with shadows that move, resize, stack, minimise
  and maximise; panel with launcher, task buttons, memory use and clock;
  Terminal, About, System Monitor and Memory Map windows; PS/2 mouse;
  Alt+Tab, Alt+F4 and Super hotkeys.
