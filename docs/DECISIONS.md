# Design decisions

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
   remain declined (see docs/TO_FINISH.md).

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
