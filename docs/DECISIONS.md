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

## 2026-09-14 — Kernel built with -fno-omit-frame-pointer -fno-optimize-sibling-calls

The exception and panic backtraces walk the RBP chain (SPEC phase 2). GCC's
tail-call optimisation removes the caller's frame before the callee runs, so
a fault inside a tail-called function lost the two frames above it in the
first test. Both flags are now global for the kernel: every function has a
frame and every call leaves a return address. Cost is a few percent of
kernel-side CPU time, acceptable before phase 14 ("correctness first").

Rejected: stack-scanning heuristics for unreliable frames (Linux-style "?"
entries) — more code for less certainty; may be added later for userland.

## 2026-09-14 — Early MMIO/ACPI mapping extends the bootloader page tables

Limine base revision 3 maps only usable/reclaimable/kernel/framebuffer
regions in the HHDM, so ACPI tables (reserved memory) and APIC registers are
unreachable at boot. kernel/mm/early_map.cpp walks the live PML4 through the
HHDM and adds 4 KiB mappings in the vmalloc region (0xFFFFC000_00000000, SPEC
§6.1) from a 32-page static pool. Phase 4's VMM inherits these tables rather
than rebuilding from scratch.

Rejected: requesting base revision 0 for the unconditional 4 GiB map (older
semantics, and the spec wants the current protocol); x2APIC via MSRs (still
leaves the I/O APIC and ACPI unmapped).

## 2026-09-14 — Early PS/2 keyboard driver for the kernel shell

The owner asked for a bootable ISO to try in VirtualBox before the desktop
exists. A keyboard driver is a phase 10 deliverable, but a minimal
scancode-set-1 decoder (kernel/drivers/ps2kbd.cpp) feeding the kernel shell
makes the phase 2 ISO interactive on screen, and exercises the I/O APIC
routing with a real device. Phase 10 replaces it with the full driver
(set 2, key events, repeat, /dev/input); the interface is deliberately tiny
(ps2kbd_getc) so nothing else grows a dependency on it.

## 2026-09-14 — Owner requirements beyond the spec: networking, smoothness

The owner wants internet access ("ability to use internet etc") and a
smooth, responsive system in VirtualBox on a fast PC. Networking is SPEC
phase 15 (stretch); it is now a required deliverable, scheduled after the
desktop (phase 13) and before/alongside phase 14: e1000 (VirtualBox and
QEMU both emulate it) plus a minimal TCP/IP stack in a userland server per
the hybrid-kernel rule (§6.2). Smoothness: keep the compositor tear-free
(already required) and target 60 Hz frame pacing; VirtualBox VM gets VMSVGA
with enough VRAM and 4 CPUs.

## 2026-09-14 — Framebuffer console keeps a text-cell shadow buffer

The first console scrolled by memmove on the framebuffer itself, which reads
the whole screen back from video memory on every scroll. Framebuffer memory
is uncached/write-combined; reading it in VirtualBox took long enough that
typing at a full screen felt laggy (owner: "sooo laggy"). The console now
keeps a character-cell buffer (512x256 bytes) and redraws rows from it;
nothing ever reads the framebuffer. memcpy/memset/memmove also switched to
rep movsb/stosb.

Rejected: a full pixel back-buffer (needs the allocator, and the compositor
in phase 12 will own that concern in userland anyway).

## 2026-09-14 — Every VirtualBox run leaves a serial log in logs/

The owner tests by hand in VirtualBox and wants a record of each run that
can be read afterwards. The VM's COM1 is bound to logs/vbox-serial.log (set
on the VM itself, so GUI-started runs log too); `make vbox` archives the
previous log with a timestamp; `make vbox-log` prints the latest. The kernel
prints a one-line "boot: OK ..." summary once every subsystem is up, so the
log can be judged at a glance.

## 2026-09-14 — APIC timer calibrated against the HPET, PIT counter as fallback

The first calibration used PIT channel 2 with the speaker gate and polled
the "timer 2 output" bit of port 0x61. Under VirtualBox the three 10 ms
samples disagreed by 4x and the 100 Hz tick ran 5x slow. Now:
1. The HPET (ACPI "HPET" table, main counter with a known period) is the
   reference when present. Both QEMU q35 and VirtualBox (with the VM's HPET
   setting on, which `make vbox` enables) provide one.
2. Without an HPET, the PIT is used differently: channel 0 runs as a
   free-running rate generator and elapsed time is the difference between
   latched count readings. No dependence on port 0x61.
3. Three samples, median taken.
4. After the periodic timer starts, the kernel measures the actual tick rate
   against the reference for 50 ticks and prints it in the "boot: OK" line,
   so a wrong calibration is visible in every VM log.
Verified: QEMU with and without HPET (`tools/qemu-probe.py --no-hpet`) both
measure 100.0-100.9 Hz.

Rejected: TSC-based calibration (no reliable frequency source on AMD/QEMU
CPUID; the TSC may not be invariant in VMs).

## 2026-09-14 — VirtualBox on this host runs on Hyper-V (NEM); QEMU+KVM in WSL2 is the fast path

Both VirtualBox VMs logged "HM: HMR3Init: Attempting fall back to NEM:
AMD-V is not available": Windows Hyper-V is active (required by WSL2 and
Docker Desktop, and the host is Windows 11 Home so Hyper-V Manager is not an
option either), so VirtualBox uses the Windows Hypervisor Platform backend.
Measured consequences in the guest: typing lag, and virtual time advancing at
roughly a third of wall time while idle (HPET and APIC timer agree with each
other, both lag the host). Nothing in the kernel can fix that.

Nested virtualisation is enabled in this WSL2, so /dev/kvm exists there.
QEMU with -accel kvm -cpu host runs the kernel at native speed with accurate
timers, and WSLg puts the QEMU window on the Windows desktop. Therefore:
- Makefile and tools/qemu-probe.py auto-select KVM when /dev/kvm exists
  (QEMU_ACCEL=tcg / LUMEN_QEMU_ACCEL=tcg force emulation).
- run-lumen.cmd at the repo root boots the ISO from Windows with one click.
- VirtualBox stays supported (make vbox, logs/vbox-serial.log) for the
  owner's preference, with the caveat above recorded in STATUS.md.

The `idle` and `timermode` shell commands and `test idle` stay as
diagnostics; they were what exposed the problem.

Rejected: disabling Hyper-V (bcdedit hypervisorlaunchtype off) to give
VirtualBox AMD-V — it would break WSL2 and therefore the build.

## 2026-09-14 — Owner decision: a graphical desktop now, hosted in the kernel until phase 12

The owner asked for a GUI today ("keep going i want a gui today"). The spec
schedules the window server for phase 12, after the scheduler, userland,
SMP, filesystems, drivers and IPC. Rather than wait, the graphics stack is
brought forward in a form that does not have to be rewritten:
- Phase 3 (physical memory) is completed first, properly, because pixel
  buffers need it.
- libgfx (SPEC §8.2) is written as a freestanding C++ library under
  kernel/gfx/ with no kernel dependencies beyond an allocation callback, so
  it moves to userland/libgfx unchanged.
- A compositor ("Pane preview") under kernel/gui/ implements the SPEC §9
  compositor internals (stacking, damage, decorations, shadows, cursor,
  panel) against in-kernel window objects. At phase 12 the same code becomes
  the Pane server process with the port protocol in front of it.
- An early PS/2 mouse driver joins the early keyboard driver (phase 10
  replaces both).
- Until the scheduler exists (phase 6) the compositor is pumped
  cooperatively from the kernel shell's idle loop; long-running shell
  commands freeze the desktop for their duration. That is a known,
  temporary limitation.
Phases 4-11 continue afterwards in order; the GUI is not a reason to skip
them.
