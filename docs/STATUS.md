# Status

Updated at the end of every session. Read this first (docs/SPEC.md §18).

## Current phase

**Phase 0 — Scaffolding: COMPLETE.** Next up: **Phase 1 — Boot and output.**

## What works

- Repository layout per docs/SPEC.md §2 (with the Glint→Spec rename, see
  docs/DECISIONS.md).
- Host environment: WSL2 Ubuntu 24.04 (`wsl -d Ubuntu-24.04`) with
  build-essential, nasm, xorriso, qemu-system-x86 8.2, gdb, python3, OVMF.
- Cross toolchain: binutils 2.42 + GCC 13.3.0 for x86_64-elf in
  `toolchain/out/`, built by `toolchain/build-cross.sh` (resumable).
- `make` builds `build/kernel/lumen.elf` (higher-half, entry
  0xFFFFFFFF80000000) and `build/kernel/kernel.sym`.
- `make iso` builds a hybrid BIOS/UEFI ISO with Limine 11.4.1.
- `make run` / `make run-uefi` / `make run-headless` / `make debug` /
  `make gdb` / `make vbox` / `make clean` / `make check-tools`.
- Verified: `python3 tools/qemu-probe.py build/lumen.iso` (BIOS) and
  `--uefi /usr/share/OVMF/OVMF_CODE_4M.fd` both report
  `RIP=ffffffff80000002 … HLT=1` — Limine hands off to the kernel, which
  halts as intended for phase 0.
- `make vbox` creates/boots the VirtualBox VM "Lumen" (EFI firmware, 4 CPUs,
  512 MB, COM1 logged to `build/serial.log`).

## Half-done

Nothing.

## Next (phase 1)

1. Limine requests (memory map, framebuffer, HHDM, kernel address, RSDP,
   modules, SMP) copied into a `BootInfo`.
2. COM1 serial driver (polling), `kprintf`, PSF2 font renderer + framebuffer
   text console (convert Terminus 8x16 from PSF1 with a `tools/` script).
3. Boot banner: version, build date, memory map summary, framebuffer
   resolution, CPUID vendor/brand.
4. Integration test: boot-to-banner `.expect` file using `tools/qemu-probe.py`.

## Known bugs

- `make` on the Windows-mounted tree occasionally prints
  "Clock skew detected" (drvfs timestamp rounding). Harmless so far.
