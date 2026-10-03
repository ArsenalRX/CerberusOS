# Status

**What this file is for.** It describes what is true about the project right
now. It is the first file every session reads (docs/SPEC.md §18).

**When to update it.** At the end of every session, without exception, and
immediately whenever something listed under "What works" stops working.

**How to update it.** Rewrite the sections in place so the file always
describes the present; do not append a history (history lives in git, in
docs/CHANGELOG.md and in docs/DECISIONS.md). Use absolute dates. Under "What
works", only list things that were verified with real output, and keep the
"Last verified" section honest: if this session did not run `make test`, say
so. Move anything started but not finished to "Half-done". Keep "Next"
matching docs/TO_FINISH.md.

**Why.** A session has no memory of the previous one. If this file claims
something works that does not, the next session builds on a broken base; if
it omits half-done work, that work is lost or done twice.

Last updated: **2026-10-03**.

## Version

Released: **0.6.0** (2026-10-03, tag `v0.6.0`, `dist/lumen-0.6.0.iso`).
`dist/` holds only the newest ISO. The tree now builds as
`0.6.1-dev+<commit>`. Scheme: docs/SPEC.md §23; history: docs/CHANGELOG.md.

Releases made on 2026-10-03: 0.3.0 (versioning, PS/2 fixes), 0.4.0 (phase 4),
0.5.0 (phase 5), 0.5.1 (VirtualBox keyboard fix), 0.6.0 (phase 6).

---

## Current phase

**Phase 6 — Threads and scheduling: COMPLETE (2026-10-03).**
Phases 0–3 complete 2026-09-14; phases 4 and 5 complete 2026-10-03. Desktop
preview (owner-requested, brought forward from phases 12-13): first version
2026-09-14, now running in its own thread.

Next up: **Phase 7 — Userland and syscalls** (SPEC §5 and §5A). The owner
approves each phase's contents before it starts.

Spec is version 2 (2026-10-03): security, privacy, networking and
performance are requirements. See docs/DECISIONS.md.

## Last verified

- `make test`: all **16** integration tests passed on **2026-10-03** in
  QEMU/KVM on the 0.6.0 release build: boot-banner, desktop, keyboard, pmm,
  vmm, heap, sched, shell-tests, exception-de/pf/ud/so/ub, heap-freelist,
  heap-write-after-free, heap-double-free.
- `make bench`: recorded in docs/BENCH.md (2026-10-03).
- On the phase 6 code before the release commit: `vmm` passed under UEFI
  (OVMF) through the test probe, and `heap` passed under software emulation
  (TCG).
- **VirtualBox 7.2.6 (Hyper-V backend), VM "Lumen", 4 CPUs, BIOS:** the
  phase 6 build booted to the desktop; typed commands worked;
  `test sched 3`, `test idle` and `bench` passed (2026-10-03). 0.5.0 and
  earlier lose the keyboard after a few keys there with more than one CPU.
- A `DEBUG=0` build was last checked on 0.5.0 (`test heap` only).

## How to run it

- `run-lumen.cmd` (double-click on Windows): boots the ISO in QEMU with KVM
  inside WSL2, window on the desktop via WSLg, serial log in
  `logs/qemu-serial.log`. `run-lumen.cmd uefi` boots through OVMF;
  `run-lumen.cmd build` rebuilds first. This is the fast, accurate path.
- VirtualBox: start the VM **"Lumen"** from the VirtualBox window (created
  2026-10-03: Other 64-bit, BIOS, 4 CPUs, 2 GiB, HPET and I/O APIC on, PS/2
  keyboard and mouse, COM1 → `logs/vbox-serial.log`). Each release points
  it at the new ISO. A VM made by hand must use OS type "Other/Unknown
  (64-bit)": the 32-bit "Other" type hides 64-bit mode and the bootloader
  then refuses to start the kernel. VirtualBox runs on the Hyper-V backend
  on this host (WSL2 keeps Hyper-V on), so it is slower than
  `run-lumen.cmd`.
- `dist/lumen-<version>.iso`: the release ISO for any VM.
  `make RELEASE=1 dist` writes it, deletes the previous one and re-points
  the VirtualBox VM.

## What works (verified 2026-10-03 by `make test` in QEMU/KVM)

- Host environment: WSL2 Ubuntu 24.04 with the cross toolchain
  (`toolchain/out/`, binutils 2.42 + GCC 13.3.0), QEMU 8.2 with KVM, OVMF.
- Build: `make` (two-pass link embedding the symbol table), `make iso`
  (hybrid BIOS/UEFI, Limine 11.4.1), `make dist`, `make bench`.
- Test: `make test` runs every `tests/integration/*.expect` through
  `tools/qemu-probe.py` (headless QEMU + QMP; directives `!send`, `!key`,
  `!mouse`, `!mouseto`, `!button`, `!screenshot`, `!wait`). The probe waits
  for expected output rather than fixed times.
- Phase 1: Limine base revision 3 → `g_boot_info`; COM1; `kprintf`; PSF2
  framebuffer console; boot banner.
- Phase 2: GDT/TSS, IDT + exception dumps with symbolised backtraces,
  embedded symbol table, PIC, ACPI, HPET, Local APIC, I/O APIC,
  self-correcting 100 Hz APIC timer, `early_map`, kernel shell, RTC.
- Phase 3: bitmap PMM, `test pmm`.
- Phase 4: VMM (`kernel/mm/vmm.cpp`): address spaces, VMAs, demand paging,
  copy-on-write clone, guard pages, 2 MiB kernel leaves; kernel half
  hardened at boot; `test vmm`.
- Phase 5: kernel heap (`kernel/mm/kheap.cpp`); `test heap`; `heapstat`.
- Phase 6: scheduler (`kernel/sched/`): threads and processes, four-level
  preemptive scheduling, sleep and wait queues, spinlock/mutex/semaphore/
  condition variable/reader-writer lock, idle and reaper threads;
  `test sched` (five preempted threads, 10 s producer/consumer with exact
  totals, sleep accuracy, locks, latency, two processes with separate
  memory, no leaks); `ps`, `bench`.
- Fatal-error tests (each halts by design): `test exceptions
  de|ud|pf|pfw|gp|bp` (CPU exceptions), `so` (a runaway thread reported as
  a kernel stack overflow), `ub` (undefined behaviour), `fl|waf|df` (heap
  corruption).
- Desktop preview (`kernel/gui/`, `kernel/gfx/`), now driven by the
  compositor thread:
  - libgfx: fills, rounded rects, alpha blits, scaled blits, lines, circles,
    gradients, box blur, PSF text with ellipsis, clip stack.
  - Compositor: wallpaper, back buffer, damage rectangles, windows with
    rounded corners, cached shadows, title bars with close/maximise/
    minimise, move and resize by mouse, stacking, software cursor.
  - Panel: launcher button + menu, task buttons, RAM %, clock and date.
  - Windows: Terminal (the kernel shell), About, System Monitor, Memory Map.
  - Hotkeys: Alt+Tab, Alt+F4, Super, Super+T, Super+M.
  - Panics and exceptions switch back to the text console.

## Security and privacy state (honest summary)

In place (each demonstrated by a test):

- W^X: no mapping can be writable and executable. Everything in the kernel
  half outside kernel text is non-executable; kernel text and read-only data
  are not writable; CR0.WP is on. (0.4.0)
- Anonymous memory is zeroed before it is mapped; the first 64 KiB of a user
  address space cannot be mapped; the lower half of the kernel's own address
  space is empty. (0.4.0)
- Zero-initialised stack variables; undefined-behaviour checks in debug
  builds. (0.4.0)
- Heap free lists are encoded with a per-boot secret and validated; double
  frees and foreign pointers are refused; debug builds add red zones,
  poisoning and write-after-free detection. (0.5.0)
- Every stack in use has a guard page: thread stacks and the exception
  stacks. (0.6.0)

Still missing, because the mechanisms they attach to do not exist yet:

- Everything runs in ring 0, including the desktop and the shell. There is
  no user mode, no system calls, no users, no permissions. Processes exist
  as a kernel structure with separate address spaces, but nothing runs in
  ring 3. (Phase 7.)
- SMEP/SMAP/UMIP are not enabled (phase 7).
- No randomness source, no ASLR, no stack canaries. The heap's free-list
  secret comes from RDRAND and the time-stamp counter until the CSPRNG
  exists (phase 7).
- No networking, so nothing leaves the machine. No persistent storage, so
  no user data is kept.

Do not put real data in the system before phase 15.

## Performance state

Numbers are in docs/BENCH.md (first block 2026-10-03). In short, under
QEMU/KVM: context switch 13 ns, wake-up latency 6 µs average, allocation
85 ns per pair (debug build), idle desktop 0% of ticks busy — all within
budget. **Over budget: a minor page fault takes about 4.1 µs against 2 µs;
not yet investigated.**

Known limits:

- The three other CPUs are halted until phase 8.
- Frame pacing follows the 10 ms tick: continuous animation would run at
  50 frames per second (see docs/DECISIONS.md, phase 6). Input is handled
  immediately.
- The shell polls the serial port once per tick (no serial interrupt yet).
- Debug builds carry the undefined-behaviour checks and heap red zones.

## Half-done

- `early_map` still hands out kernel virtual addresses on its own (first GiB
  of the vmalloc region); drivers can move to `mmap_device` when convenient.
- File-backed memory regions, and the file-descriptor table and working
  directory in `Process`, wait for the VFS (phase 9).
- One terminal window (one kernel shell). No text selection, scrollback, or
  escape sequences in the terminal.
- An unregistered "Lumen-dev" folder is left under `VirtualBox VMs` from the
  VM that was removed; it is not used.

## Open decisions waiting on the owner

Recorded in docs/DECISIONS.md (2026-10-03).

1. **libc: grow the in-tree libc or port mlibc.** Phase 7 writes the first
   libc code, so this should be answered before phase 7 starts.
2. TCP/IP: write in-tree (current plan) or port lwIP. Needed before
   phase 14.
3. TLS and cryptography source: port Mbed TLS (recommended), port BearSSL,
   or write in-tree. Needed before phase 15.
4. Confirm the v2 phase order and the non-goals list.
5. Still open from 2026-09-14: confirm the scope of the network toolbox
   (docs/TO_FINISH.md, "Owner-added requirements").

## Next

1. Get the owner's go-ahead for phase 7 and the libc decision.
2. Phase 7: ring 3, `syscall`/`sysret`, the dispatch table, validated user
   copies, ELF loader, first libc, `init`; SMEP/SMAP/UMIP, user ASLR, stack
   protector, CSPRNG, `make fuzz`.
3. Look at the minor-page-fault cost (over budget).
4. Desktop polish as the owner sends reference screenshots.

## Known bugs

- `make` on the Windows-mounted tree occasionally prints
  "Clock skew detected" (drvfs timestamp rounding). Harmless so far.
- Backtraces cannot name a function that faults inside its own prologue
  (inherent to RBP walking); tests avoid it by making a call before faulting.
- The About window's CPU line is clipped at the window edge on long brand
  strings (needs ellipsis on the value column).
- On VirtualBox the guest's reference clock appears to lag while the guest
  is busy, so sub-millisecond timings measured there are unreliable (one
  context-switch measurement came out as 2 ns).
