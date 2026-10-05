# Design decisions

> **Name:** the operating system was called **Lumen** until 2026-10-04,
> when the owner renamed it **Cerberus** (its file system lumfs became
> **cerfs**). Entries written before that date keep the old names.

Dated entries. Each records the choice, the reasoning, and the alternatives
rejected. Newest at the bottom.

**What this file is for.** It records why the system is the way it is, so a
later session does not re-decide, undo, or contradict a choice without
knowing the reasoning (docs/SPEC.md §0 rule 7 and §22).

**When to add an entry.**
- Any design choice that is not obvious from the spec.
- Any deviation from docs/SPEC.md, and any change to the spec itself.
- Any direction or requirement from the owner.
- Any security bug that crossed a trust boundary: the class of bug and the
  rule or check that now prevents the class.
- Any change to a performance budget (SPEC §20.1), and any benchmark
  regression over 10% that is being accepted.
- Any dependency proposed or approved.
- Any open question that needs the owner's answer (mark it **OPEN**).

**How to write an entry.** Append at the bottom, never in the middle. Title
is `## YYYY-MM-DD — one-line summary`. The body states: what was decided,
why, what it affects (files, phases), and a `Rejected:` line with the
alternatives and why each lost. Keep it short enough to read in a minute.

**Never rewrite an old entry.** If a decision changes, add a new entry that
names the one it supersedes and says why; add a one-line
`Superseded by YYYY-MM-DD — title` note under the old title and leave the
rest intact. The history of why something changed is as useful as the
current answer.

**When an OPEN question is answered**, add a new dated entry with the
answer and mark the question `Answered YYYY-MM-DD` where it was asked.

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

*Partly superseded by 2026-10-03 — Spec v2: the owner asked for the spec to
be updated, so it is no longer verbatim v1. The naming table still applies.*

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

## 2026-10-03 — Spec v2: security, privacy, networking and performance become requirements

**Owner direction (2026-10-03):** a large overhaul so the OS runs real
programs and connects to the internet; "keeping this OS privacy and security
for the users is the most important"; the OS must run "extremely smoothly
with minimal lag" and use memory and hardware efficiently; and every file in
docs/ must say how, when and why it is updated and describe how to implement
things effectively and safely.

**What changed in docs/SPEC.md (v1 → v2):**

| Area | v1 | v2 |
|---|---|---|
| Security | Non-goal beyond user/kernel separation and pointer validation | Requirement. New §19 (principles, threat model, kernel protections, identity, sandbox, window-server privacy, network security, privacy rules, cryptography, supply chain, fuzzing, checklist) |
| Privacy | Not mentioned | Requirement. §19.8: no telemetry, no identifiers, no user content in logs, local-only crash dumps; permanent non-goal to ever add telemetry |
| Users and permissions | Non-goal | Enforced in the VFS from phase 9; users, login, sandbox in phase 15 |
| Performance | "No optimisation before phase 14" | New §20: budgets, rules, per-subsystem design, measurement with `make bench` and docs/BENCH.md from phase 6 |
| Networking | Phase 15 stretch | Phase 14, required, with firewall and per-app permission |
| Running real programs | Minimal libc only | Phase 17: dynamic linking, POSIX subset, ports, signed packages |
| Installer, updates, disk encryption | Absent | Phase 18 |
| Phases 4–13 | Functional deliverables only | Extra security and performance rows per phase (§5A) |
| Buffer cache | Separate from file cache | One unified page cache (phase 9) |
| Syscalls | 0–65 | Added 66 `getrandom`, 70–83 sockets, 90–92 events, 100–111 credentials and `restrict`, 120–125 files; existing numbers unchanged |
| Working agreement | 10 rules | Rules 11–14 added (security first, budgets, procedure, no unapproved dependency) |
| Process | §18 checklist | New §21 implementation procedure, §22 document maintenance |

**Phase renumbering:** old 14 (language backend) → 16; old 15 (stretch) →
19; new 14 networking, 15 security model, 17 software platform, 18
installer/updates/encryption. Phases 0–13 keep their numbers. This matches
the 2026-09-14 entry that scheduled networking after phase 13.

**Why security is built into each phase rather than added at the end:**
research on 2026-10-03 (Linux kernel self-protection documentation and
SerenityOS's mitigation history) shows the baseline — W^X, never executing
or casually touching user memory (SMEP/SMAP), UMIP, stack protectors,
compiler hardening, then pledge/unveil-style sandboxing — is cheap when
built with the mechanism and expensive to retrofit. Lumen has no VMM or
syscall layer yet, so it can be designed in from the start.

**Design choices made here (not in v1, not from the owner verbatim):**
- **No setuid binaries;** elevation through a broker service (`authd`).
  Rejected: classic setuid (historic source of local privilege escalation).
- **Self-restriction sandbox** (`restrict`, like OpenBSD pledge/unveil).
  Rejected: syscall-filter programs (seccomp-BPF style) — far more
  machinery for a small system; mandatory access control frameworks — too
  large for now.
- **Window-server client isolation and user-mediated capture** as protocol
  rules. Rejected: X11-style trust between clients (any client can log
  keys and read the screen).
- **Network stack stays in userland** (`netd`), as decided 2026-09-14.
- **Event multiplexing moved into phase 11**, before the window server
  leaves the kernel. Reason: ToaruOS's first userland compositor performed
  badly mainly because its kernel could not wait on many clients at once.
- **KASLR, KPTI, Secure Boot** deferred to phase 19 or later: each needs
  groundwork (relocatable kernel, signing infrastructure) that would stall
  everything else.
- **Budgets in §20.1** are initial targets chosen from what comparable
  systems achieve under KVM; they are not yet measured on Lumen and will
  be revised by dated entries once `make bench` exists.

**OPEN — needs the owner's answer (SPEC §0 rule 14):**
1. **TLS/cryptography source.** Port Mbed TLS (recommended: has TLS 1.3,
   takes I/O callbacks, insists on a real entropy source), port BearSSL
   (smaller, no heap, but TLS 1.2 only and lightly maintained), or write
   in-tree (not recommended). Needed before phase 15.
2. **libc.** Grow the in-tree libc, or port mlibc (its porting guide needs
   only paging, an ELF loader, syscalls and text output from the kernel;
   other independent OSes run large software on it). Needed by phase 17;
   best decided before phase 7.
3. **TCP/IP stack.** Write in-tree (current plan), or port lwIP into
   `netd`. Other independent OSes report hand-written TCP staying
   incomplete for a long time. Needed before phase 14.
4. **Confirm the v2 phase order and non-goals**, in particular: no own web
   browser (porting one later is possible), and offensive network tools
   remain declined (see docs/TO_FINISH.md). *(Browser: answered 2026-10-04,
   the owner wants our own; see below.)*

**Evidence limits:** the research verified claims about libc porting, TLS
library porting, kernel hardening and compositor IPC. It found no verified
sources on filesystems, drivers, ASLR, permissions models, disk encryption,
update mechanisms or service management; those parts of v2 follow general
operating-system practice and should be treated as proposals.

*Still OPEN after this session: questions 1–4 above.*

Rejected: keeping v1 verbatim and layering changes only in this file (the
owner asked for the docs themselves to be updated; a spec that says
"security is a non-goal" contradicts the owner's top priority); writing a
separate SECURITY.md and PERFORMANCE.md (the spec is meant to be read top
to bottom as one document; §19 and §20 keep it that way).

## 2026-10-03 — Version scheme: 0.PHASE.PATCH, numbers only (owner approved)

The owner asked how updates are labelled and approved this scheme the same
day. Before 1.0 the version is `0.PHASE.PATCH`: PHASE is the last completed
spec phase, PATCH counts every release in between (bug, performance and
security fixes) and resets when PHASE advances. From 1.0 it is
MAJOR.MINOR.PATCH with MAJOR for compatibility breaks. The kind of change is
carried by `docs/CHANGELOG.md` headings (Security first), not by the number.
Full rules: docs/SPEC.md §23.

Mechanics: the number lives in `./VERSION`; `make RELEASE=1` builds a release,
any other build is labelled `<version>-dev+<commit>`; only
`kernel/lib/version.cpp` sees the string, so a version change rebuilds one
object. The first release under the scheme is 0.3.0 (phases 0–3 complete);
the earlier snapshot keeps its 0.0.1 label.

Rejected: letter suffixes (a/b/rc) — ambiguous ordering and nothing the
changelog does not say better; date-based versions — do not show how far the
OS is through the plan; detecting a release from git tag and tree cleanliness
automatically — the repository is edited from Windows and built from WSL,
where file-mode and line-ending differences make "clean" unreliable, so the
release build is an explicit `RELEASE=1`.

## 2026-10-03 — Periodic desktop diagnostic removed; it broke the tests

The uncommitted work from 2026-09-14 included a timer hook that printed a
`diag:` line to serial every 5 seconds. On 2026-10-03 `make test` failed
`keyboard` and `desktop`: the line landed between the shell prompt and the
typed command, so the expected `lumen> <command>` text never appeared
contiguously. The keyboard itself worked. The hook, `gui_diag_line` and the
`g_diag_*` counters were removed; the on-demand `gui` and `irqs` shell
commands remain. Rule going forward: nothing prints to the serial log
unprompted on a timer, because the integration tests match on that log.

Rejected: loosening the tests to tolerate interleaved output (they would
stop proving that typed input is echoed intact).

## 2026-10-03 — Phase 4: the VMM adopts and hardens the bootloader's page tables

As planned on 2026-09-14 ("Early MMIO/ACPI mapping extends the bootloader
page tables"), `vmm_init` keeps Limine's PML4 as the kernel address space
instead of building a new one, then hardens it in place:
- every leaf in the kernel half outside the kernel image gets NX;
- the kernel image is re-permissioned page by page from the linker symbols
  (text r-x, data/bss rw-, everything else r--);
- the lower half is emptied; CR0.WP and EFER.NXE are forced on;
- all 256 kernel PML4 slots are populated, so a user address space is a copy
  of those 256 entries and never goes stale.

**Finding:** Limine maps the whole direct map writable *and executable*
(entries were `P|W` with no NX). Until `vmm_init` runs, every byte of RAM is
executable kernel memory. This is why the hardening walk exists and why it
runs as early as the IDT allows.

Why adopt rather than rebuild: the direct map already uses 2 MiB leaves
(255 of them at 512 MiB) and the framebuffer keeps the bootloader's cache
attributes; rebuilding would have to reproduce both.

Other choices made in phase 4:
- **Kernel memory is never demand-paged.** `mmap` on the kernel space always
  populates. A fault inside an interrupt handler must not depend on the
  allocator.
- **`mmap` with FIXED never replaces an existing mapping** (returns Exists).
  POSIX replaces silently; that is a classic way to clobber a live mapping.
- **Copy-on-write marks every owned page**, including pages in read-only
  regions, so a later `mprotect` to writable cannot make two address spaces
  share a writable frame.
- **Frames carry a reference count only while the VMM owns them** (anonymous
  memory). Raw `map()` and device mappings are tagged not-owned in the PTE
  and are never freed by unmapping.
- **VMA nodes and AddressSpace objects come from small page-backed pools**
  until the heap exists (phase 5); pool pages are kept for reuse.
- **Fault-safe probes** (`kernel/mm/probe.*`): three labelled instructions
  that may fault, with fixups. The self-tests use them now; phase 7's
  `copy_from_user`/`copy_to_user` are built on the same mechanism.

**Deviation from SPEC phase 4:** the spec lists VMA backing as "anonymous,
file, or device". File backing needs the VFS and page cache (phase 9) and is
added there; phase 4 implements anonymous, device, and guard regions.

**Not covered yet:** the bootstrap stack (provided by the bootloader) and the
static IST stacks have no guard page. Guarded stacks exist
(`vmm_alloc_kernel_stack`) and every thread stack gets one in phase 6, which
is also where SPEC §5A lists that row.

Rejected: building fresh kernel tables (see above); a recursive page-table
mapping (the direct map already reaches every table); per-VMA red-black tree
(a sorted list is enough until processes have hundreds of regions; revisit
with `make bench`).

## 2026-10-03 — The other CPUs are parked in kernel text before hardening

Symptom: with NX applied to the direct map, boot slowed to a crawl under
QEMU/KVM (seconds per line of output) but was fine under TCG. Bisecting the
hardening walk showed that only the 4 KiB leaves below 2 MiB mattered.

Cause: Limine leaves the application processors spinning in its own code,
which under BIOS boot lives in bootloader-reclaimable memory below 1 MiB and
is reached through the direct map. Making that memory non-executable made
all three APs fault with no usable IDT, over and over, which starved the
bootstrap CPU. (TCG kept the stale executable translations cached, so it did
not show.)

Fix: `boot_park_aps()` (`kernel/boot/limine_requests.cpp`) writes each AP's
`goto_address` so it jumps into `ap_park`, a `pause` loop in kernel text,
and waits until all have arrived before any permission is changed.

Consequences for phase 8 (SMP): the APs have already consumed Limine's
`goto_address`. Phase 8 does not start them from the bootloader as the spec
text says; it releases them from `ap_park` (the loop gains a per-CPU "go"
word that points at the AP initialisation path). No real-mode trampoline is
needed. They still burn host CPU while parked, exactly as they did inside
Limine; halting them needs a per-CPU IDT/TSS so an NMI can wake them, which
is phase 8 work.

Rule learned (SPEC §21 "when fixing a bug"): before removing a permission
from memory, ask who else is still executing or writing through it — other
CPUs included.

Rejected: leaving bootloader-reclaimable memory executable until phase 8
(keeps several hundred KiB of writable+executable memory, the exact thing
W^X forbids); marking it read-only+executable (the bootloader's wait loop
writes to its own data).

## 2026-10-03 — Compiler hardening: zero-initialised locals, a subset of UBSAN

The kernel is now built with `-ftrivial-auto-var-init=zero` (always) and, in
debug builds, `-fsanitize=undefined` with handlers in `kernel/lib/ubsan.cpp`
that panic with the source location.

SPEC §5A says "the undefined-behaviour sanitizer"; these checks are left out,
for the reasons given:
- `vptr` — needs RTTI, which the kernel does not have.
- `float-cast-overflow`, `float-divide-by-zero` — no FPU use in the kernel.
- `alignment`, `null`, `object-size` — one check per pointer dereference.
  With them kernel text grew from 22 to 73 pages; without them, to 43. x86
  permits unaligned access and ACPI tables are unaligned by design (the
  first boot with `alignment` on stopped in `acpi.cpp`); a null dereference
  already faults cleanly because nothing is mapped in the low half.

What remains on: signed overflow, shifts, array bounds, division, invalid
bool/enum loads, pointer overflow, unreachable code, missing return,
non-null violations, VLA bounds.

`kernel/gfx/` (the pixel loops) is built without the sanitizer as a
precaution. The cost could not be measured: the only timing available, the
first desktop frame, varied between 21 and 29 ms from run to run with and
without it. Revisit when `make bench` exists (phase 6).

Cost accepted: debug kernel text roughly doubles (22 → 43 pages).

## 2026-10-03 — Phase 5: kernel heap design

- **Slabs are single pages in the direct map**, with a 32-byte header at the
  start of the page. `kfree` finds the slab by rounding the pointer down to
  the page. No per-frame metadata table.
  Cost: the 1024-byte class holds 3 objects per page and the 2048-byte class
  holds 1, because the header takes the space of one. Rejected: an
  out-of-band table with one descriptor per physical frame (about 0.6% of
  all RAM, paid whether or not the heap is used); multi-page slabs (the
  frame allocator cannot hand out aligned runs, so the header could not be
  found from a pointer). Revisit if allocations of 1–2 KiB turn out to be
  common.
- **Large allocations (over 2048 bytes) are whole pages**, physically
  contiguous through the direct map when the frame allocator has a run,
  otherwise a VMM mapping. SPEC phase 5 says "direct page allocation"; the
  direct map also keeps the desktop's multi-megabyte pixel buffers on 2 MiB
  TLB entries, exactly as before they moved to the heap. Their bookkeeping
  is a small node from the 64-byte slab on a linked list, looked up
  linearly at free; fine for tens of large allocations, to be indexed when
  there are thousands.
- **`kmalloc` returns a plain pointer, nullptr on failure** (marked
  `[[nodiscard]]`). SPEC §4 says a nullable pointer's type should say so;
  the spec also names these functions with their conventional signatures.
  Every kernel caller checks the result. Rejected: `Result<void*>` at every
  call site.
- **Free-list hardening:** the link in a free object is stored XORed with a
  per-boot secret and the link's own address, and every pointer taken off a
  list is checked to be an object boundary inside the same slab. The secret
  comes from RDRAND when present, mixed with the time-stamp counter; the
  kernel CSPRNG replaces that source in phase 7.
- **One empty slab per size class is kept**, the rest go back to the frame
  allocator, so a cache that hovers around empty does not thrash it. Idle
  cost: at most 8 pages.
- **Debug overhead is 48 bytes per object** (16-byte record of call site,
  requested size and state; 16-byte red zone each side). Slack between the
  requested size and the class size is red zone too, so an overrun of one
  byte is caught even inside the size class.
- **Lock order for phase 8:** the VMM allocates its VMAs from the slab path,
  and the heap's large path calls the VMM. The slab path must therefore
  never call the VMM, and the large path must not hold the heap lock while
  it does.

Measured on 2026-10-03 (QEMU/KVM, one CPU active, `test heap`): a
`kmalloc(64)`+`kfree` pair takes 74–83 ns in the debug build and 13–19 ns
with `DEBUG=0`; the SPEC §20.1 budget is 100 ns. 100,000 random
allocate/free pairs with fill and verify take about 270 ms (debug).

## 2026-10-03 — Parked CPUs halt instead of spinning (fixes the keyboard on VirtualBox)

Supersedes the "they still burn host CPU while parked" part of "The other
CPUs are parked in kernel text before hardening" (same date).

Symptom (0.5.0 on VirtualBox 7.2.6, Hyper-V backend, 3–4 virtual CPUs): the
desktop came up, the first typed command worked, then the keyboard stopped
after about eight keys. The kernel was alive (same idle RIP on every sample,
interrupts enabled, nothing pending in the APIC), and VirtualBox's own
`info ps2k` showed the typed bytes piling up in its keyboard queue (18, then
45 items) with the controller's output buffer empty.

Cause: VirtualBox hands the guest one keyboard byte at a time, driven by an
internal timer. Three virtual CPUs spinning in `pause` at 100% starved that
timer on the Hyper-V backend. With the VM set to one CPU the same ISO typed
and ran `test vmm` normally, which isolated it.

Fix: `ap_park` is now `cli; hlt` in a loop. A halted virtual CPU costs the
host nothing. Verified on VirtualBox with 4 CPUs after the change.

Consequence for phase 8: the parked CPUs cannot be released by setting a
flag (they are halted with interrupts off). Phase 8 restarts them with the
standard INIT/SIPI sequence and a small real-mode trampoline. This replaces
both the spec's "start APs via Limine's goto_address" and the earlier note
that no trampoline would be needed.

Likely related, to re-test in phase 6: the 2026-09-14 finding that "a halted
vCPU stops receiving the timer interrupt" on this backend was observed while
the other CPUs were spinning inside the bootloader. The idle thread's `hlt`
may simply work now.

Rule learned: an idle CPU must halt. A busy-wait that is harmless on real
hardware and on KVM can break a hypervisor that multiplexes device timers
onto the same host threads.

## 2026-10-03 — dist/ holds one ISO; the VirtualBox VM is "Lumen"

Owner direction: a new release replaces the old ISO instead of piling up
next to it. `make dist` now deletes `dist/lumen*.iso` before copying the new
`dist/lumen-<version>.iso`, and re-points the DVD drive of the VirtualBox VM
`Lumen` at it when that VM exists and is powered off. Old versions remain
reproducible from their git tags.

The VM `Lumen` was created on 2026-10-03 (the owner's earlier VMs had been
removed and a hand-made one had been created as 32-bit "Other", which hides
64-bit mode from the guest; Limine then reports that the CPU is not 64-bit).
Settings that matter: OS type Other (64-bit), BIOS firmware, I/O APIC and
HPET on, PS/2 keyboard and mouse, VMSVGA with 64 MB, COM1 to
`logs/vbox-serial.log`. `make vbox` creates the same VM if it is missing.

## 2026-10-03 — Phase 6: scheduler design

- **Four levels, one FIFO queue each, one 10 ms tick per slice.** A thread
  that uses its whole slice drops one level; a thread that blocks returns to
  its base level; every second all threads return to base so nothing
  starves. A thread made ready above the running one's level preempts at
  once, including from an interrupt handler (the switch happens on the way
  out of the interrupt, after the handler has acknowledged it).
- **The boot stack is abandoned.** `sched_start` creates the idle, reaper
  and first threads on guarded stacks and switches away from the
  bootloader's stack for good; the exception (IST) stacks are moved onto
  guarded stacks as soon as the heap exists. From then on every stack in
  use has a guard page (SPEC §5A phase 6).
- **Each thread carries the address space it runs in**, and the switch
  reloads CR3 only when the incoming thread's differs. Simple and correct
  for kernel threads that temporarily enter a user address space (the VMM
  test does); it gives up the "lazy" optimisation of letting kernel threads
  borrow whatever space is loaded. Revisit with PCID in phase 8.
- **Exited threads are freed by whoever joins them, or by a reaper thread
  if detached**, because a thread cannot free the stack it is standing on.
- **Synchronisation primitives use "interrupts off" as their internal
  lock.** Exact on one CPU; phase 8 puts a spinlock inside each.
- **`kprintf` is atomic per call** (interrupts off for the duration), so
  output from different threads cannot interleave within a line.
- **The compositor is an INTERACTIVE thread** that sleeps on a wait queue,
  woken by PS/2 input and by the next tick. The shell is a NORMAL thread
  that polls the serial port once per tick. The idle thread halts.

Deviations from SPEC phase 6, for the owner to see:
- `Process` has pid, name, address space, thread list, parent, exit status
  and credentials, but **no file-descriptor table or working directory
  yet**; those types do not exist until the VFS (phase 9).
- **Frame pacing follows the 10 ms tick**, not a dedicated 60 Hz timer
  (SPEC §9): with nothing happening the compositor wakes 100 times a second
  and presents at most every 16.7 ms, so continuous animation lands on
  20 ms boundaries (50 frames per second) unless input wakes it sooner. A
  one-shot high-resolution timer would fix this; it belongs with tickless
  idle in phase 8.
- Sleep resolution is one tick: `thread_sleep_ms` never returns early and
  may be up to 10 ms late.

Measured (docs/BENCH.md, 2026-10-03): context switch 13 ns, wake-up latency
6 µs average, idle desktop 0% busy. **Minor page fault is 4.1 µs against a
2 µs budget**; recorded as over budget, not yet investigated.

Rejected: a single run queue with priorities as weights (more arithmetic for
no benefit at this scale); a tickless design now (needs per-CPU one-shot
timer management that phase 8 has to redo anyway); switching stacks inside
the timer handler itself (the switch at interrupt exit keeps every handler
ordinary code).

## 2026-10-03 — The test probe waits for output instead of for a fixed time

`tools/qemu-probe.py` used to sleep a fixed number of seconds before typing
and read the serial pipe only at the end. Two failures followed from that on
2026-10-03: with the host busy the guest had not booted when the probe typed
(the keys landed in the bootloader's menu), and under UEFI the unread pipe
filled with firmware output until QEMU stopped accepting serial bytes and the
guest blocked in `serial_putc` (the "`--uefi` hangs" bug).

Now a thread drains the pipe continuously; before the first action the probe
waits until the expect lines that precede it have appeared (up to 90 s);
after each `!wait` it allows up to three times longer if the expected output
has not arrived; an expect file with no actions waits for all of its lines.
A healthy run is as fast as before; a slow host no longer fails tests.

This also explains the one unexplained `test idle` failure recorded for
0.5.1: the host was running VirtualBox at the time and the guest's timer
measurement was disturbed. It has not recurred.

## 2026-10-03 — `make dist` repairs and re-points any VirtualBox VM that boots Lumen

Supersedes the "the VirtualBox VM is 'Lumen'" part of the entry "dist/ holds
one ISO" (same date).

The owner creates and deletes VirtualBox VMs through its wizard rather than
keeping one fixed VM, and twice in one day the new VM could not boot: the
wizard cannot recognise the Lumen ISO, so it picks OS type "Other/Unknown",
which is 32-bit and disables long mode; Limine then says the CPU does not
support 64-bit.

So `make dist` no longer targets a VM by name. `tools/vbox-attach.sh` looks
at every registered VM, and for each powered-off one whose DVD drive already
points at an ISO in `dist/` it attaches the new ISO and, if long mode is off,
switches the VM to "Other (64-bit)" with long mode, PAE, I/O APIC and HPET
on. Running or saved VMs are left alone and reported. Nothing is created or
deleted.

Rejected: keeping a VM that the build owns (the owner removed it); printing
instructions only (the same mistake had already happened twice); shipping a
32-bit stub that explains the problem on screen (Limine prints its own
message first, and it is a lot of code for a VM setting).

## 2026-10-03 — The release ISO has one fixed name: dist/lumen.iso

Owner direction, repeated: "when building the dist overwrite the old one so
there is only one release I can choose from". `make dist` now writes
`dist/lumen.iso` every time (and `dist/VERSION.txt` naming the version)
instead of `dist/lumen-<version>.iso`. A fixed name also means a VirtualBox
VM keeps working across releases without being re-pointed. The version is
visible in the boot banner, the About window and VERSION.txt.

Supersedes the file-naming part of "dist/ holds one ISO" (same date).

## 2026-10-03 — Phase 7: user mode and system-call design

- **One frame layout for every way into the kernel.** The `syscall` entry
  builds the same `InterruptFrame` an interrupt does, on the thread's kernel
  stack (found through per-CPU data behind `swapgs`). `fork` copies the
  frame, `execve` rewrites it, and the first entry to ring 3 is an `iretq`
  from one. Before `sysret` the dispatcher forces user segments, cleans the
  flags and kills the process if the return address is not a user address
  (a non-canonical one would fault in ring 0 on the user's stack).
- **Dispatch is a `switch` generated from `kernel/syscall/table.def`**, not a
  table of function pointers: nothing writable to overwrite, and an unknown
  number falls through to `ENOSYS`. The number is also masked without a
  branch before the switch, so an out-of-range value is not used
  speculatively. `tools/gen-syscalls.py` writes `docs/SYSCALLS.md` and the
  user library's numbers from the same file. Only implemented calls are
  listed.
- **User memory is touched only in `kernel/mm/usercopy*`.** Range check
  (user half, no wrap), then a copy whose faults are fixed up to
  `Error::Fault`. With SMAP these are the only `stac`/`clac` in the kernel,
  and a kernel access to a user page from anywhere else is reported as a
  kernel bug. System calls copy through a kernel buffer in chunks.
- **Programs must be position-independent; the loader refuses anything
  else.** Load base, mmap base and stack top are drawn from the CSPRNG per
  `execve`. Segments that are writable and executable are refused, as is
  `mmap` with both. The ELF parser and the tar reader depend only on
  `lib/types.h`, so `make fuzz` builds the very same files for the host.
- **CSPRNG: ChaCha20 with fast key erasure**, seeded from RDSEED/RDRAND when
  present and always mixed with the time-stamp counter, the reference clock
  and interrupt timing. It is initialised first thing in `kernel_main`, so
  the kernel's stack guard, the heap's free-list secret and ASLR all draw
  from it.
- **Stack protector with one global guard** in the kernel and in each user
  program (`-mstack-protector-guard=global`). A user program's guard is set
  by its start-up code from 16 random bytes the kernel puts on its stack
  (`AT_RANDOM`).
- **FPU/SSE belongs to user programs.** The kernel is still built without
  SSE; the state is saved and loaded only when a different user thread is
  about to run.
- **A faulting user program is killed, not the system.** The exit status
  carries a Unix-style signal number (11, 8, 4) so `waitpid` callers can
  tell; there is no signal delivery until phase 11.
- **The boot archive** (`boot/initrd.tar`, a Limine module) holds the user
  programs until the VFS exists. `open` reads from it, read-only. `init`
  is pid 1; for now it only announces itself and collects orphaned
  children.
- **User programs are linked with the host linker.** The bare-metal cross
  linker silently produces fixed-address executables when asked for PIE.
  The objects are ordinary x86-64 ELF, so the host `ld` links them with our
  linker script and no host libraries.
- **`make fuzz`**: a small mutation driver of our own
  (`tests/fuzz/driver.cpp`, AddressSanitizer + undefined-behaviour checks,
  exact-size inputs, crash input saved, `tests/fuzz/corpus/` replayed as
  regression tests) rather than libFuzzer, which would need clang on the
  build host. System-call arguments are fuzzed inside the running system by
  `/bin/sysfuzz`, whose children make random calls with hostile arguments.

**libc.** Phase 7 wrote the minimal in-tree library that SPEC phase 7 lists
as a deliverable (`userland/libc`: start-up code, system-call wrappers,
`malloc` on `mmap`, strings, `printf`). That does not settle the open
question for phase 17 (keep growing it, or port mlibc); no answer from the
owner is recorded, so it stays open.

Deviations from SPEC phase 7, for the owner to see:
- **No `/dev/random` yet**: there is no device filesystem until phase 9.
  `getrandom` (call 66) is there.
- **The stack guard is one global value, not per-CPU** as §5A words it. A
  per-CPU value adds nothing while there is one CPU; revisit in phase 8.
- **The "direct dereference faults under SMAP" check lives in `test vmm`**,
  not in a test system call: a deliberately unsafe call does not belong in
  the ABI.
- **`read` from the console returns 0** (no terminal input path to user
  programs until the tty work in phases 9–10). `Process` now has a
  file-descriptor table (32 entries) ahead of the VFS; no working directory.
- **Speculation hardening is partial**: the system-call number is masked;
  the user-copy bounds check is a plain branch, and whether this CPU needs
  retpolines has not been evaluated. Left for the phase 8 audit.
- **`fork` is not in the random system-call fuzzer** (children would
  multiply); `forktest` covers it.

Rejected: a writable function-pointer table for dispatch; letting the page
fault handler accept any kernel-mode fault on a user address (hides kernel
bugs; SMAP would be pointless); loading fixed-address executables "for
now" (ASLR would then be optional forever); a separate interrupt-style
`int 0x80` path (two entry paths to keep correct instead of one).

## 2026-10-03 — Phase 8: SMP design

- **The other processors are started through the bootloader, not with
  INIT/SIPI.** Limine has already started every processor and leaves each
  waiting for an address to jump to (SPEC phase 8 names this mechanism). At
  boot they are moved into a wait loop in kernel text, as before; `smp_init`
  then releases them. Each loads its own GDT, TSS, the shared IDT, its
  per-CPU data and control registers, enables its local APIC, prints
  "smp: cpu N online", and sleeps (`sti; hlt`) until the scheduler starts.
  This replaces the plan recorded on 2026-10-03 ("phase 8 restarts them
  with INIT/SIPI"): a real-mode trampoline would duplicate what the
  bootloader has done. The wait loop spins instead of halting, but only for
  the few milliseconds between `vmm_init` and `smp_init`; the VirtualBox
  problem of 0.5.0 came from spinning for the whole session.
- **Per-CPU data behind GS** (`arch/x86_64/percpu.h`): kernel stack pointer
  for system-call entry, the running thread, CPU id, the loaded address
  space, the lock ranks held. It is set up first thing in `kernel_main`,
  because locks and `kprintf` use it.
- **Per-CPU run queues under one scheduler lock.** Each CPU has its own four
  queues. A thread that becomes runnable goes to the CPU it last ran on if
  that CPU is idle, else to any idle CPU, else back to its last CPU; a CPU
  that runs out of work steals from the queue of a busy one. One spinlock
  protects all of it, together with wait queues, the sleep list, the thread
  and process lists and the process tree, and it is held across the switch
  from one thread to the next (released by the thread that resumes). That
  one rule removes the hard races of a multi-CPU scheduler: a thread cannot
  be woken and run elsewhere while it is still on its old stack, and "put
  myself on a wait queue and stop" is a single step. Mutex, semaphore,
  condition variable and reader-writer lock keep their state under the same
  lock.
- **One clock.** Every CPU has its own 100 Hz APIC timer for time slices;
  only the bootstrap CPU's tick advances time, wakes sleepers and restores
  priorities.
- **Inter-processor interrupts:** "look at your run queue" (the switch
  happens on the way out of the interrupt), "flush your TLB", and a
  non-maskable "stop" sent by a panic so one CPU reports while the others
  halt.
- **TLB shootdown** (`arch/x86_64/smp.cpp`): whoever removes or restricts a
  translation tells the CPUs that have that address space loaded (all of
  them for the kernel space) and waits for each to flush, before it releases
  the address-space lock and before any frame is given back to the
  allocator. A CPU that is waiting for a spinlock keeps answering these
  requests, so waiting for a flush while holding a lock cannot deadlock.
- **A lock per address space** plus one for the frame reference counts
  shared between spaces. Copy-on-write copies the page while still holding a
  reference, so a second sharer cannot make the page writable under the
  copy. A fault that finds the page tables already allow the access (another
  CPU resolved it) just flushes and retries.
- **Per-CPU heap slabs** (`mm/kheap.cpp`): each CPU allocates from, and
  frees into, a slab of its own per size class without a lock. An object
  freed on a different CPU goes onto its slab's "remote" list under the heap
  lock; the owner collects that list when its own runs out. Red zones,
  poisoning, encoded free lists and double-free detection work as before.
- **Lock ranks** (`lib/lock_order.h`): address space (user, then kernel) →
  frame counts → heap → frame allocator → TLB → scheduler → CSPRNG →
  console. Debug builds check every acquisition per CPU and panic on a
  violation (`test exceptions lo`).
- **FPU/SSE state is saved whenever a user thread leaves a CPU**, not lazily:
  the thread may resume on another CPU.
- **Everything that relied on "interrupts off" as its lock was changed:**
  frame allocator, heap, VMM, scheduler and sync primitives, process tree,
  CSPRNG, `kprintf`/console (one console lock, also taken by the compositor
  when it paints or resizes the terminal), file reference counts and offsets
  (atomics), keyboard/mouse/terminal input rings (single producer, single
  consumer, published with atomic stores), interrupt counters (atomics).

Also in this phase: the user-copy routines now mask the address without a
branch after the range check (left over from phase 7), so a mispredicted
check cannot be used to read kernel memory speculatively.

Deviations from SPEC phase 8, for the owner to see:
- **The run-queue path takes a shared lock.** §5A asks that per-CPU slab
  caches and run queues let "the common path take no shared lock". The heap
  does that; the scheduler has per-CPU queues but one lock, for the reason
  above. With four CPUs the lock is not contended enough to matter;
  splitting it is future work if measurements ask for it.
- **Waking a thread on another CPU costs an interrupt.** Under nested
  virtualisation that is about 20 µs, so workloads that hand work back and
  forth between two threads got slower when the threads landed on different
  CPUs (the producer/consumer run in `test sched` moves about a tenth of
  the items it did on one CPU). Latency is still far inside its budget. The
  known remedy is to let an idle CPU poll for a short while before it halts,
  or to wake a thread on the waker's CPU when the waker is about to sleep;
  neither is done yet.
- **Not tickless**, no one-shot timer; frame pacing is still tied to the
  10 ms tick (phase 6 deviation, unchanged).
- **Retpolines were not evaluated**, and the stack guard is still one global
  value rather than per-CPU; both remain open from phase 7.
- At most **32 CPUs** are used (thread affinity is a 32-bit mask); any
  beyond that stay halted.
- The `timermode` diagnostic changes only the timer of the CPU the shell
  happens to run on.

Rejected: INIT/SIPI with a real-mode trampoline (see above); a lock per run
queue plus a per-thread "still on its CPU" flag (the usual fully split
design; several subtle races for no measurable gain at this size); deferring
frame frees to a later pass instead of shooting down before the free
(simpler to reason about when the free can never precede the flush);
keeping lazy FPU switching with a cross-CPU "give me that thread's state"
interrupt.

## 2026-10-04 — Version scheme: 0.0.5a, one letter per release (owner)

The owner asked for versions to climb more slowly: the next build is
`0.0.5a`, the one after `0.0.5b`, and so on to `0.0.5j`, after which comes
`0.0.6a`. Every release takes the next letter, whatever it contains; the
phase a release completes is written in its changelog entry, not in its
number. After `0.0.9j` the patch number carries into the minor number
(`0.1.0a`). This replaces the `0.PHASE.PATCH` scheme of 2026-10-03
(docs/SPEC.md §23.1 rewritten). Releases 0.3.0 to 0.7.0 and their tags are
left as they are, so the number goes down once, from 0.7.0 to 0.0.5a; tags
are only labels, and nothing in the system compares version numbers yet
(`pkg upgrade`, phase 18, will have to compare by the new rules and treat
the old numbers as older).

## 2026-10-04 — Reference clock: the time-stamp counter, PIT under a lock

Found while checking phase 8 on VirtualBox with 4 CPUs. VirtualBox offers no
HPET, so `refclock_now_us()` read the PIT: three port accesses with no lock.
With several CPUs reading it at once (the compositor and a test, say), the
reads interleaved, the clock ran several times too fast, and `test timer`,
`test idle`, `test smp` and `test sched` failed there (0.7.0 used one CPU
and never saw it; QEMU has an HPET). First suspected, wrongly, to be
VirtualBox withholding timer interrupts.

- **The clock is now the time-stamp counter** when the CPU says its rate is
  constant, or when running under a hypervisor (which presents a
  constant-rate counter even when it hides the flag). It is calibrated for
  50 ms at boot against the HPET or PIT, read with one instruction, and a
  shared "latest value" keeps it from going backwards when a thread moves
  between CPUs. QEMU/KVM: 3,869 MHz.
- A counter slower than 100 MHz is not believed. VirtualBox's measured
  6.9 MHz here, so it keeps the PIT, which is now read under a lock with
  interrupts off.
- Rejected: keeping the HPET as the first choice (every read is a VM exit,
  about 1 µs under KVM); a per-CPU clock (callers compare timestamps taken
  on different CPUs).

Also changed in the self-tests: the preemption workers are pinned to one CPU
and the work-stealing threads run for a number of timer ticks rather than
microseconds, so they test the scheduler and not the clock; reader overlap
in the reader-writer test is checked in a phase without a writer; latency
checks in self-tests are sanity bounds (20 ms), the budgets are judged from
`make bench` under QEMU/KVM.

## 2026-10-04 — Our own web browser (owner)

The owner asked for "cool features ie internet our own web browser". This
reverses the spec v2 non-goal "writing our own web browser". It becomes
**phase 15B** (docs/SPEC.md), after networking (14) and TLS and the
sandbox (15), which it needs; numbering it 15B avoids renumbering phases
16–19 everywhere. Scope for 15B: HTTP/HTTPS, an HTML parser following the
WHATWG algorithm for common cases, a documented CSS subset, block and
inline layout, images, a tabbed UI; no JavaScript (a script engine, written
or a QuickJS port, is a later decision). Sandboxed, every parser fuzzed.
The owner also said to get the current base running smoothly first, so the
desktop polish (0.0.5b) and phases 9–15 come before it.

## 2026-10-04 — The OS is renamed Cerberus (owner)

The owner asked to "rename the OS Cerberus". Done as one sweep while
phase 9 was in progress:

- Every current name: code identifiers (`cerberus_version()`,
  `CERBERUS_DEBUG`), the boot banner, the shell prompt (`cerberus>`), the
  desktop's wordmark, the C library header (`cerberus.h`), the kernel image
  (`cerberus.elf`), the release image (`dist/cerberus.iso`), the Windows
  launcher (`run-cerberus.cmd`), the spec and the current documents.
- The file system lumfs became **cerfs** (`mkfs.cerfs`, on-disk magic
  `CERFS`): no disk had been formatted outside the tests yet, so changing
  the format's magic cost nothing now and would cost compatibility later.
- History is not rewritten: released changelog entries, earlier decision
  entries and benchmark blocks keep "Lumen", with a note at the top of each
  of those files. Git history and tags (v0.7.0, v0.0.5a, v0.0.5b) are
  unchanged.
- The Spec language keeps its name.

## 2026-10-04 — Phase 9: files and file systems

Design:

- **One VFS lock**, a sleeping and re-entrant mutex, serialises the tree and
  every file system's metadata and data (`fs/vfs.cpp`). Character devices
  are called without it, because a console read may wait. Finer locking
  when a benchmark asks for it.
- **The initramfs is the boot archive unpacked into a tmpfs**, which is
  then marked read-only, rather than a file system reading the tar in
  place. One file system type fewer; the archive is small.
- **devfs is a tmpfs holding device nodes.** Device vnodes dispatch through
  a device switch by major number (`fs/dev.cpp`). Block devices are byte
  addressable through their node, cached in the page cache.
- **Page cache** (`fs/pagecache.cpp`): pages keyed by (vnode, index), one
  quarter of RAM at most, LRU eviction, read-ahead of up to 16 pages when
  misses are sequential, write-back every 5 s by a kernel thread. File
  systems keep metadata in the pages of the device vnode and file contents
  in the pages of the file's vnode, so nothing is cached twice. A page can
  be **held** (part of an uncommitted journal transaction): eviction and
  write-back skip it.
- **Name cache** in the VFS: 512 positive entries for names up to 31 bytes,
  each holding references to both vnodes, invalidated on unlink, rename and
  unmount.
- **cerfs** (the spec's "lumfs", renamed with the OS): 4 KiB blocks, 128-byte
  inodes with 12 direct, one indirect and one double-indirect pointer,
  directories as chains of records in 4 KiB blocks, a block bitmap. CRC32C
  on the superblock and every inode (their own field) and on bitmap,
  directory, indirect and journal blocks (a 16-byte header that also names
  the block's owner, so a misplaced block is caught). Directory blocks are
  metadata: journaled, read through the device's pages.
- **Journal:** physical block journaling of metadata, one transaction in
  the journal at a time. A commit writes the files' dirty data first
  (ordered mode), then a descriptor, the block images and a commit record
  with a CRC over the images, flushing between steps; then the blocks to
  their homes; then advances the journal header's sequence number. Mount
  replays a complete transaction and ignores an incomplete one. Commits
  happen on sync, fsync, unmount, every 5 s, and when a transaction nears
  the journal's size; long operations (writing or truncating big files)
  write the inode before an intermediate commit, so a commit never records
  blocks the inode does not show.
- **Closed files stay cached** (up to 256 inactive vnodes per file system)
  with their pages; a deleted file is freed on disk when its last reference
  goes.
- **Access times are not written** (reads would otherwise dirty metadata).
- **Disk drivers brought forward from phase 10:** the phase 9 acceptance
  test needs a disk. ATA PIO (legacy IDE: QEMU `pc`, VirtualBox's default
  IDE controller) and AHCI with DMA (QEMU q35, VirtualBox SATA, real PCs),
  both polled, plus minimal PCI enumeration. Phase 10 adds interrupts, MSI,
  virtio-blk and the driver model.
- **System calls:** the spec's file block (3–19) is used as numbered, with
  `readdir` returning an array of entries and `truncate` taking a
  descriptor. Calls the spec's table has no number for start a new block at
  120: `openat` 120, `rmdir` 121, `symlink` 122, `readlink` 123, `chmod` 124,
  `fsync` 125, `mount` 126, `umount` 127, `lstat` 128, `chown` 129. `time_ms`
  (60) is implemented as specified.
- **Fuzzing cerfs inside the kernel** (`test cerfsfuzz`, run by `make fuzz`)
  instead of a host harness: the real driver, VFS and page cache run under
  the debug kernel's checks; images are damaged at random, half of the time
  with their checksums recomputed so the damage reaches field validation.

Deviations from SPEC phase 9, for the owner to see:

- **File-backed `mmap` is not done.** The spec's §5A asks for one page cache
  shared by file reads, file-backed mmap and block I/O; the cache is ready
  for it, but the fault path (which would have to wait for disk reads while
  the address space lock is a spinlock) is not. Moved to phase 11 (with
  shared memory).
- **tmpfs keeps its own pages** instead of living in the page cache.
- **No hard links** (`link`): not in the spec's call table; link counts exist
  on disk.
- **A deleted file that is still open when the power goes** leaves an inode
  allocated but nameless; the checker reports it as a leak (a warning).
  Repairing that is `fsck.cerfs` in phase 18.
- The console still returns end-of-file to programs that read it (phase 17,
  terminals).

Rejected: a buffer cache separate from the page cache (two caches for one
disk); logical journaling of operations (harder replay); journaling file
data (halves write speed); a host-side cerfs fuzzer through kernel shims
(tests less of the real code); a lock per vnode (deferred until measured).

## 2026-10-04 — Reference clock: which TSC to trust (correction)

The rule of earlier today ("use the TSC when it is invariant, or under any
hypervisor") was wrong for VirtualBox on this host (Hyper-V backend). Found
by the phase 9 check there: `test timer` saw 58 ticks in "500 ms".
VirtualBox passes the host's invariant-TSC flag through, does not always set
the CPUID hypervisor bit, and its counter measured 7, 1,345 and 1,530 MHz on
different boots of one 4,200 MHz CPU; a clock built on it stalls or jumps
when a thread changes CPU.

- The TSC is now used only on bare metal with the invariant flag, or under
  **KVM** (recognised by its CPUID signature). VirtualBox is recognised by
  its ACPI OEM id ("VBOX") and never trusted.
- Calibration takes three 20 ms measurements that must agree within 1%.
- The PIT fallback is polled from the timer tick so its 16-bit counter
  cannot wrap unseen.
- Verified: QEMU/KVM uses the TSC (4,192 MHz); VirtualBox uses the PIT and
  passes `test all` (10 of 10) twice in a row with 4 CPUs.

## 2026-10-04 — Anti-aliased fonts rendered at build time (owner: "rough edges")

The owner's screenshot of 0.0.5c showed stair-stepped text: the desktop used
the 8x16 console bitmap font, and drew its wordmarks by enlarging the 16x32
one two and three times. A TrueType renderer is phase 13 work (Facet), so
until then the glyphs are rendered on the build machine: `tools/gen-fonts.py`
(FreeType through Pillow) writes `kernel/gfx/fonts.bin`, 8-bit coverage for
DejaVu Sans 14 px (regular and bold), DejaVu Sans Mono 13.3 px (advance
exactly 8 px, 8x17 cells) and DejaVu Sans Bold at 64 and 104 px (letters
only). The blob is committed, so a build needs neither Pillow nor the font
files. libgfx's `Font` now covers both kinds; the loader bounds-checks every
offset in the blob. The text console (boot log, panics) keeps the PSF fonts.

- **New asset, licence:** DejaVu fonts (Bitstream Vera licence plus public
  domain additions; free to embed and redistribute). This is a bundled
  asset the owner did not pick explicitly; swapping the family is a
  one-line change in the generator.
- Also: an anti-aliased line primitive for the close button, and the
  wallpaper computed with 8 extra bits per channel and ordered dithering.
- Rejected: smoothing the enlarged bitmap font (still blurry, still a pixel
  font); writing the TrueType rasteriser now (phase 13).

## 2026-10-04 — Phase 10: drivers

The owner said "finish all phases" on 2026-10-04, so phases follow one
another without waiting for a go-ahead; each still ends with a verified
release.

Design:

- **Driver model** (`drivers/driver.h`): a `Driver` record with name, bus
  (platform or PCI), PCI class match, `probe`, `attach`, `detach`, in one
  registry. `drivers_probe_all` probes platform drivers once and offers each
  PCI device to the matching drivers. AHCI, NVMe, virtio-blk and ATA use it;
  the PS/2, timer and interrupt-controller code is brought up by hand
  during early boot as before.
- **Interrupts for PCI devices are MSI only.** Routing a legacy INTx line
  needs the `_PRT` methods in ACPI's AML, and the kernel has no AML
  interpreter. A device without MSI is polled. Vectors 0x40–0xEF are handed
  out by `interrupt_alloc_vector`; messages go to the bootstrap CPU.
- **AHCI** completes commands by MSI (QEMU) with a semaphore the handler
  raises; it falls back to polling where there is no MSI (VirtualBox).
- **NVMe** and **virtio-blk** are polled (their interrupts are MSI-X,
  which is not implemented yet). NVMe: one admin and one I/O queue,
  namespace 1, 512-byte blocks. virtio: the modern PCI transport and split
  queues in `drivers/virtio.cpp`, written to be reused by virtio-net.
  NVMe was not in the spec's phase 10; it is added because most current PCs
  boot from it (docs/INSTALLER.md), as proposed to the owner.
- **Keyboard:** scancode set 2 untranslated, each code mapped to the key's
  set-1 make code (the controller's own table) so one key map serves both
  modes; set 1 through the controller remains the fallback. Repeats come
  from the keyboard (500 ms, 30 per second) and are marked on the event.
  US layout only; other layouts belong with the settings application.
- **Input events** (`drivers/input.cpp`): 24-byte records in a ring per
  device, read through `/dev/input/kbd0` and `/dev/input/mouse0` (blocking,
  whole records). The in-kernel desktop still reads the drivers' own
  queues; the device files are what the userland window server will use.
- **Access rule:** "only the window server's credential" is, until users
  exist (phase 15), "root only": the nodes are mode 0600. `runas` (shell)
  and a credential parameter on `process_spawn` exist to test it.
- **Power:** S5 through the FADT's PM1 control ports, with the sleep type
  taken from the `\_S5_` package found by name in the DSDT (no AML
  interpreter); restart through the FADT reset register, then the keyboard
  controller, then a triple fault. `reboot` is system call 64.
- **/tmp** is mode 1777 and the VFS enforces the sticky rule.

Deviations from SPEC phase 10, for the owner to see:

- No MSI-X; NVMe and virtio-blk are polled. No legacy INTx routing.
- NCQ is not used (optional in the spec); one command at a time per disk.
- The real-time clock is read once at boot and kept by the reference clock,
  not by counting timer ticks.
- The desktop does not yet take its input from `/dev/input` (it is still in
  the kernel until phase 12).
- No keyboard LEDs.

Rejected: an AML interpreter now (large; only `_S5` and interrupt routing
need it so far); keeping the controller's translation as the only mode (it
cannot express some keys and hides the keyboard's real codes).

---

## 2026-10-04 — Phase 11: IPC, signals, threads

- **Kernel objects are descriptors.** A port, a shared-memory block and an
  event queue are each a File whose vnode has type Object (`ipc/object.h`).
  They close, duplicate, survive `fork` and travel through ports like any
  other descriptor, and die with their last reference. `port_close` (SPEC
  number 54) is therefore not needed: `close` does it.
- **Ports.** `port_create(name, mode)` gives a listener; `port_connect`
  makes a two-way channel and hands the server its end through `port_recv`
  on the listener (like accept). Queue state lives under the scheduler
  lock, so "queue empty, go to sleep" is one step. Limits: 64 KiB per
  message, 8 descriptors per message, 64 messages or 1 MiB queued per
  direction, 32 connections waiting.
- **What may be passed:** files, devices and shared memory. Ports and event
  queues may not: a descriptor queued inside the object it names (directly
  or through a second port) keeps both alive for ever, and the kernel has
  no cycle collector. The window server does not need it (clients connect
  to its named port themselves).
- **Shared memory** is physically contiguous and mapped as a device range,
  which is what makes a futex in it the same futex in every process (the
  key is the physical address). Large blocks can fail on a fragmented
  machine; paged shared memory comes when something needs it.
- **Signals** are posted to the process and taken by whichever thread next
  returns to user mode (`user_return`, called at the end of every system
  call and interrupt from ring 3). The handler frame (registers and FPU
  state) is on the thread's own stack; the handler returns into a
  trampoline page the kernel maps into every program, which calls
  `sigreturn`. sigreturn leaves through `iretq` so every register comes
  back. No signal masks, no queued signals (one pending bit each), no
  alternate stack.
- **Ending a process with several threads** uses the same check: the
  ending thread marks the process, interrupts the others and waits; they
  leave at their next return to user mode. Waits made for a user program
  (sleep, waitpid, port, futex, event, input) are interruptible so this is
  prompt. `execve` does the same before it replaces the image.
- **Descriptor table.** Threads share it, so lookups take a reference for
  the length of the call under one short lock (`fd_get`), and the working
  directory likewise.
- **Thread-local storage:** the FS base, set by `set_tls` and
  `thread_spawn(entry, arg, stack, tls)` (one argument more than the spec's
  prototype), kept per thread and across `fork`. The C library lays out the
  program's PT_TLS template below the thread pointer (variant II,
  local-exec), so `__thread` works; `errno` is thread-local.
- **Event queues** are level-triggered and built on one readiness function
  (`file_poll`) plus a global "something changed" wake-up; fine for the
  handful of servers there will be, to be made per-object if it shows up
  in profiles.
- **File-backed mmap** is private only and reads the file in when mapped
  (no demand paging from the page cache yet). Shared file mappings are
  refused; shared memory covers the IPC use.
- **Identity:** `setuid`/`setgid` are root-only and one-way; there is one
  identity per process (no effective/saved ids) until phase 15.
- New numbers outside the spec's table: 58 `port_peer`, 67 `time_us`,
  130 `set_tls`.

Deviations from SPEC phase 11, for the owner to see: `port_send` always
waits when the queue is full (no non-blocking flag yet); no `getgroups`/
`setgroups` (phase 15); a signal interrupts every waiting thread of the
process, not just one.

Rejected: a separate handle table for IPC objects (two namespaces to pass
and inherit); delivering signals by interrupting kernel code (every wait
would need unwinding); `pipe` now (ports do the job until the POSIX layer
in phase 17).

---

## 2026-10-04 — Desktop: Settings, launcher, resolution (owner request)

The owner asked for a settings page (colours, three wallpapers, RGB borders
with effects), a logo start button with app search, and a way to change the
screen size. All of it is in the kernel-hosted desktop for now and moves
with it to Pane and the Settings application (phases 12 and 13).

- **Preferences** are one struct in memory (`g_prefs`). They are not saved:
  the root file system is read-only and there is no per-user storage until
  phase 15. They reset at boot.
- **Border effects** are drawn per pixel as a ring between two rounded
  rectangles; an animated effect redraws the affected windows about 30
  times a second. With the effect off (the default) an idle desktop still
  draws nothing.
- **Resolution** changes go through the Bochs display interface (ports
  0x1CE/0x1CF), which QEMU's standard VGA and VirtualBox's VBoxVGA/VBoxSVGA
  provide. The adapter's whole video memory (PCI BAR 0, at most 64 MiB) is
  mapped once. The driver only claims the adapter if the bootloader's
  frame buffer lies inside that BAR and the adapter reports the mode the
  bootloader set; real graphics cards keep the firmware's resolution until
  there are native drivers (phase 19).
- **Launcher**: applications are a table with keywords; search is a
  case-insensitive substring match on both. Power off and Restart in the
  menu sync the disks first.

Rejected: saving preferences to `/tmp` (lost at reboot anyway, and would
look like persistence); VBE BIOS calls for mode setting (need real mode or
an emulator).

---

## 2026-10-05 — Desktop polish before phase 12 (owner request)

The owner chose "more polish until we start phase 12" and, during the day,
asked for a calendar with reminders, all common resolutions and refresh
rates, ultrawide support, a cleaner Settings window without hint text, and
a fix for the mouse at 1920x1080 on VirtualBox. All of it lives in the
kernel-hosted desktop and moves with it to Pane (phase 12).

- **Animations** are done without an off-screen window buffer: the back
  buffer region is saved, the window is drawn normally (shifted), and the
  result is mixed back with the saved pixels by the window's opacity
  (`draw_faded`). A closed window leaves a "ghost" that owns its buffers
  until it has faded. 160 ms, eased. The same helper fades the launcher,
  the calendar and notifications.
- **Frame rate** in Settings is a cap on how often the compositor presents
  (30–144 Hz). A virtual machine has no monitor and no vertical refresh,
  so there is nothing else to set; the chip row is labelled "Frame rate",
  not "Refresh rate", to be honest about it. On real hardware the monitor's
  EDID will give the real modes (phase 19).
- **Resolutions** are a fixed table of 18 common sizes (4:3 to 32:9
  ultrawide, up to 5120x1440) filtered by what the adapter's video memory
  holds; there is no EDID in a VM to read.
- **VirtualBox guest device** (`kernel/drivers/vmmdev.cpp`): the host's
  absolute pointer position is read on every PS/2 packet (as the VirtualBox
  Linux driver does) rather than through the device's own interrupt, which
  keeps the driver to one request block and no IRQ. Our own cursor is kept
  (NEW_PROTOCOL flag, no host cursor). This fixes B-003 (docs/BUGS.md).
- **Calendar and reminders** are in the compositor's memory (32 reminders,
  40 characters each); the clock, the calendar and reminders use a UTC
  offset from Settings. They are lost at restart like the preferences
  (B-011) until there is a place to save them.
- **Notifications** are a four-slot list drawn last; other threads post
  through a lock-free ring (`gui_notify`), which the shell's `notify` uses.
- **Print Screen** writes an uncompressed 32-bit BMP to `/tmp` from the
  back buffer (the composed scene without the cursor).
- **Limine** boots with `timeout: 0`: the menu never showed its countdown
  on VirtualBox (B-002).
- **Settings layout**: titled cards, measured by painting the card's body
  once with an empty clip (every primitive honours the clip), then painted
  for real; no hint sentences. Tabs: Appearance, Wallpaper, Window borders,
  Display.
- **Docs**: two new files the owner asked for. docs/BUGS.md is the bug
  ledger (every bug: symptom, cause, fix, guard, with ids `B-nnn`);
  docs/FEATURES.md is the inventory of what exists and how it is used.
  Both are read at the start of a session (CLAUDE.md).
- **Owner's working style** (recorded so it is kept): after each iteration
  update the HTML list on their Windows desktop with what was fixed or
  added, and end it with 8 numbered QoL candidates for them to choose
  from; put every build into VirtualBox (`dist/cerberus.iso`) and make the
  VM say which version it is; the owner has an ultrawide monitor.

**OPEN (owner):** should Cerberus mount the first cerfs disk it finds at
boot (e.g. at `/data`) so that Settings, reminders and screenshots can be
kept? Until then nothing persists (B-011).

Rejected: a true monitor refresh-rate setting (nothing in a VM to set it
on); reading the host pointer through the VMMDev interrupt (more code for
the same result); persisting preferences in `/tmp` (not persistence).
