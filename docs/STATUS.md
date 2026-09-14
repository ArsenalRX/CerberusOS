# Status

Updated at the end of every session. Read this first (docs/SPEC.md §18).

## Current phase

**Phase 0 — Scaffolding** (in progress)

## What works

- Repository layout per docs/SPEC.md §2 (with the Glint→Spec rename, see
  docs/DECISIONS.md).
- `Makefile` with all spec targets (`all kernel iso run debug gdb test clean`)
  plus `run-headless`, `vbox`, `check-tools`.
- `toolchain/build-cross.sh` (binutils 2.42 + GCC 13.3.0, x86_64-elf).
- Limine v11.x vendored in `toolchain/limine/` with `limine.h`.
- `kernel/linker.ld` higher-half layout, `limine.conf`, and a phase-0
  `kernel/main.cpp` that halts.

## Half-done

- Host environment: WSL2 Ubuntu 24.04 is being installed; the cross
  toolchain has not been built yet, so `make` has not yet been proven.

## Next

1. Finish WSL2 setup: `sudo apt install build-essential nasm xorriso
   qemu-system-x86 gdb python3 bison flex texinfo libgmp-dev libmpfr-dev
   libmpc-dev`, then `./toolchain/build-cross.sh`.
2. Prove phase 0: `make` produces `build/kernel/lumen.elf`; `make run` boots
   Limine in QEMU.
3. Start phase 1 (Limine requests, serial, framebuffer console, kprintf,
   boot banner).

## Known bugs

None yet.
