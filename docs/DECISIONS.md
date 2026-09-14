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

## 2026-09-14 — libgcc is built with `-mcmodel=large -mno-red-zone`, SSE left on

The spec's kernel flags (`-mcmodel=kernel -mno-sse`) cannot be applied to
libgcc itself: libgcc is compiled `-fpic`, which the kernel code model
rejects, and its float helpers return values in XMM registers, which
`-mno-sse` makes uncompilable. libgcc is therefore built with the large code
model (PIC-compatible, links into a kernel-model image) and no red zone, with
SSE enabled. The kernel is compiled `-mno-sse`, so it can never emit a call to
a float helper; the integer helpers (`__divti3`, `__popcountdi2`, …) are what
we actually link. Note for `build-cross.sh`: GCC bakes `CFLAGS_FOR_TARGET`
into `gcc/libgcc.mvars` during `all-gcc`, so the flags are passed to every
step, not only the libgcc step.

Rejected: not linking libgcc at all (128-bit division and a few builtins
would then need hand-written replacements).

## 2026-09-14 — Boot verification uses a QMP probe, not silence

`tools/qemu-probe.py` boots the ISO headless, waits, and reads `RIP` through
QEMU's QMP socket. A boot counts only if `RIP` is inside the higher-half
kernel image. This is the primitive the integration harness (phase 1+) will
build on, because "no crash on serial" is not evidence that the kernel ran.

## 2026-09-14 — Console font: Terminus 8x16 converted from PSF1 to PSF2

The spec asks for a PSF v2 renderer. Ubuntu ships Terminus 8x16 only as
PSF1 (Lat15-Terminus16.psf.gz); the 32x16 variant is already PSF2.
tools/psf1to2.py converts the PSF1 file once and the result is committed as
kernel/boot/font/terminus-8x16.psf, embedded with .incbin from
kernel/boot/font.S. Terminus is SIL OFL 1.1 (kernel/boot/font/LICENSE.txt).

Rejected: supporting PSF1 in the kernel renderer (one format is enough);
fetching the upstream Terminus tarball (adds a network step to the build).

## 2026-09-14 — Integration tests are expect-files over the serial log

tests/integration/<name>.expect holds ordered substrings that must appear in
the serial output of a headless boot. make test runs each through
tools/qemu-probe.py, which also insists RIP ends inside the kernel image.
Scripted input (for shell-driven tests) is added to the probe when phase 2
introduces the kernel shell.

## 2026-09-14 — Owner direction for the desktop (phases 12-13)

The owner wants the GUI to feel "super nice and sleek, like Linux and
Windows combined" and will supply reference screenshots. Treat Pane/Facet/
shell visuals as a first-class requirement, not a checkbox: consistent
theme, shadows, animations, and polish per SPEC §10 "Accessibility and
polish". Revisit this entry when the screenshots arrive.
