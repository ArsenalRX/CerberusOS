# Design decisions

Dated entries. Each records the choice, the reasoning, and the alternatives
rejected. Newest at the bottom.

---

## 2026-09-14 — The language is called Spec, not Glint

The spec document (`docs/SPEC.md`) names the language "Glint". The project
owner named it **Spec**. Everywhere the spec says Glint, read Spec:

| Spec document      | This repo          |
|--------------------|--------------------|
| Glint              | Spec               |
| `glintc`           | `specc`            |
| `glintfmt`         | `specfmt`          |
| `.gl` source files | `.spec`            |
| `glint/`           | `spec/`            |
| `libglint`         | `libspec`          |
| GIR (the SSA IR)   | SIR                |
| `tests/glint/`     | `tests/spec/`      |

Rejected: editing the spec document itself. It is the owner's document and is
kept verbatim; this table is the single translation point.

## 2026-09-14 — Host is Windows 11; build happens in WSL2 Ubuntu 24.04

The spec assumes a Unix host (`make`, `xorriso`, GNU cross toolchain, QEMU with
a GTK display). The development machine is Windows 11 with only MinGW GCC,
Chocolatey, and VirtualBox installed. Building a freestanding ELF kernel with
MinGW is possible but xorriso, Limine's host tool, and the GDB workflow are not,
so the whole build runs inside WSL2 (Ubuntu 24.04) with the repo living on
`D:\Programs\OS` (`/mnt/d/Programs/OS` from WSL).

Consequences:
- `toolchain/build-cross.sh` builds in `$HOME/.cache/lumen-toolchain` (Linux
  native filesystem) rather than under the repo, because compiling on
  `/mnt/<drive>` is several times slower. The install prefix is still
  `toolchain/out/` as the spec requires.
- `make run` uses QEMU inside WSL; the window appears through WSLg.
- A `make vbox` target creates/boots a VirtualBox VM on the Windows side from
  the same ISO. VirtualBox is the owner's chosen way to test; QEMU remains the
  spec's primary target and is what the test harness uses.

Rejected: native Windows build with clang/lld (no xorriso, no Limine host tool,
GDB-QEMU workflow poor); moving the repo into the WSL filesystem (owner wants
it on D:, and VirtualBox needs a Windows-visible ISO path).

## 2026-09-14 — Limine v11.x binary release, hybrid BIOS+UEFI ISO

The spec says "UEFI via Limine". We vendor the `v11.x-binary` branch (the
current stable line) plus its `limine.h`. The ISO is built as a hybrid
BIOS/UEFI image (both `limine-bios-cd.bin` and `limine-uefi-cd.bin`) so it
boots whether a VirtualBox VM has the EFI checkbox on or not, and so QEMU's
default SeaBIOS works without OVMF. UEFI is still the primary path; the
`make vbox` target creates the VM with `--firmware efi`.

Rejected: GRUB/multiboot2 (spec forbids writing to anything but Limine);
UEFI-only ISO (needlessly brittle for a VirtualBox user).
