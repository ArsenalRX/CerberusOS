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

Released: **0.7.0** (2026-10-03, tag `v0.7.0`). The release is always
`dist/lumen.iso` (`dist/VERSION.txt` names the version); each release
overwrites it. The tree now builds as `0.7.1-dev+<commit>`. Scheme: docs/SPEC.md §23; history: docs/CHANGELOG.md.

Releases made on 2026-10-03: 0.3.0 (versioning, PS/2 fixes), 0.4.0 (phase 4),
0.5.0 (phase 5), 0.5.1 (VirtualBox keyboard fix), 0.6.0 (phase 6),
0.7.0 (phase 7).

---

## Current phase

**Phase 7 — Userland and syscalls: COMPLETE (2026-10-03).**
Phases 0–3 complete 2026-09-14; phases 4, 5 and 6 complete 2026-10-03. Desktop
preview (owner-requested, brought forward from phases 12-13): first version
2026-09-14, now running in its own thread.

Next up: **Phase 8 — SMP** (SPEC §5 and §5A). The owner approves each
phase's contents before it starts; phase 8 has not been approved yet.

Spec is version 2 (2026-10-03): security, privacy, networking and
performance are requirements. See docs/DECISIONS.md.

## Last verified

All on 2026-10-03, on the phase 7 code.

- `make test` in QEMU/KVM: all **22** integration tests passed:
  boot-banner, desktop, keyboard, pmm, vmm, heap, sched, shell-tests,
  exception-de/pf/ud/so/ub, heap-freelist, heap-write-after-free,
  heap-double-free, user-hello, user-fork, user-badptr, user-crash,
  user-aslr, user-sysfuzz.
- `make fuzz` (60 s per harness): ELF loader 4.5 million mutated inputs,
  tar reader 126 thousand, no crash; random system calls: 600 processes
  making up to 1.2 million calls, kernel still running.
- `make bench`: recorded in docs/BENCH.md.
- **VirtualBox 7.2.6 (Hyper-V backend), a temporary headless VM with
  4 CPUs, BIOS** (created for the check and deleted afterwards): booted;
  `run /bin/hello`, `forktest`, `badptr`, `crash null`, `crash smash`,
  `aslr` and `sysfuzz 40 500` behaved as in QEMU; `test vmm` passed;
  `test sched 3` passed six times in a row; `bench` ran. VirtualBox on this
  host does not offer SMEP, SMAP or UMIP to the guest, so there the kernel
  prints a warning and runs without them (the range checks and fault fixups
  still apply); under QEMU/KVM all three are on.
- Not run this session: UEFI boot, software emulation (TCG), a `DEBUG=0`
  build (last checked on 0.5.0).

## How to run it

- `run-lumen.cmd` (double-click on Windows): boots the ISO in QEMU with KVM
  inside WSL2, window on the desktop via WSLg, serial log in
  `logs/qemu-serial.log`. `run-lumen.cmd uefi` boots through OVMF;
  `run-lumen.cmd build` rebuilds first. This is the fast, accurate path.
- VirtualBox: the owner creates VMs in the VirtualBox window. As of
  2026-10-03 (evening) no VM is registered. Point a new VM's DVD drive at
  `dist/lumen.iso`. The wizard creates such VMs as OS type "Other/Unknown", which is 32-bit and
  hides 64-bit mode, and the bootloader then reports that the CPU is not
  64-bit. `make dist` (via `tools/vbox-attach.sh`) fixes that and re-points
  the DVD drive for every powered-off VM that boots an ISO from `dist/`.
  If the boot menu stays on screen, press Enter. VirtualBox runs on the
  Hyper-V backend on this host (WSL2 keeps Hyper-V on), so it is slower
  than `run-lumen.cmd`.
- `dist/lumen.iso`: the release ISO for any VM (always this name).
  `make RELEASE=1 dist` overwrites it and re-points
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
- Fuzz: `make fuzz` (`FUZZ_SECONDS=60` per harness by default): host builds
  of `kernel/proc/elf.cpp` and `kernel/fs/ustar.cpp` under AddressSanitizer
  (`tests/fuzz/`), then `/bin/sysfuzz` inside QEMU. A crashing input is
  saved as `build/fuzz/crash-<name>.bin`; files in
  `tests/fuzz/corpus/<name>/` are replayed first on every run.
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
- Phase 7: user mode. Programs run in ring 3 from the boot archive
  (`boot/initrd.tar`, built from `userland/`); `syscall`/`sysret` entry;
  14 system calls (docs/SYSCALLS.md, generated from
  `kernel/syscall/table.def`); validated user copies with fault fixups;
  hardened ELF loader (position-independent programs only); `fork` with
  copy-on-write, `execve`, `waitpid`; per-process FPU/SSE state; a faulting
  program is killed and reported; `init` is pid 1; in-tree libc
  (`userland/libc`). Shell command `run <path> [args]`. Programs: `hello`,
  `args`, `forktest`, `badptr`, `crash`, `aslr`, `sysbench`, `sysfuzz`.
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

- User programs run in ring 3 in their own address spaces; a program that
  faults is killed and the system carries on (`user-crash`). (0.7.0)
- Every system-call argument is validated; user memory is reached only
  through the user-copy routines (`user-badptr` walks every call with
  kernel addresses, unmapped addresses, wrapping ranges and unknown flags;
  `user-sysfuzz` makes random calls). (0.7.0)
- SMEP, SMAP and UMIP are enabled when the CPU offers them; `test vmm`
  shows a direct kernel read of a user page faulting under SMAP. (0.7.0)
- User ASLR (code, mmap area and stack differ per run: `user-aslr`);
  only position-independent programs load; W^X holds for `mmap` and for
  program segments. (0.7.0)
- Kernel CSPRNG (ChaCha20, RDSEED/RDRAND plus timing); `getrandom`; it
  seeds ASLR, the stack guards and the heap secret. (0.7.0)
- Stack protector in the kernel and in every user program
  (`crash smash`). (0.7.0)
- The ELF loader, the tar reader and the system-call interface are fuzzed
  by `make fuzz`. (0.7.0)

Still missing:

- The desktop and the kernel shell still run in ring 0. User programs
  exist but there are no users, no permissions and no file system: every
  program can read every file in the boot archive.
- Where the hypervisor hides SMEP/SMAP (VirtualBox on this host), the
  kernel has only its software checks.
- Speculation hardening is partial: the system-call number is masked; the
  user-copy bounds check is not, and retpolines have not been evaluated
  (phase 8 audit).
- No `/dev/random` (no device files until phase 9); programs use
  `getrandom`.
- No kernel ASLR (phase 19).
- No networking, so nothing leaves the machine. No persistent storage, so
  no user data is kept.

Do not put real data in the system before phase 15.

## Performance state

Numbers are in docs/BENCH.md (latest block 2026-10-03, 0.7.0). In short,
under QEMU/KVM: context switch 14 ns, system-call round trip 36 ns, wake-up
latency 7 µs average, allocation 80–100 ns per pair (debug build), idle
desktop 0% of ticks busy — all within budget. **Over budget: a minor page
fault takes 2.2–3.3 µs against 2 µs** (it was 4.1 µs in 0.6.0; the figure
varies from run to run and has not been profiled).

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
- File-backed memory regions and a working directory in `Process` wait for
  the VFS (phase 9). The file-descriptor table exists (32 entries) but
  only knows the console and read-only boot-archive files; `read` from the
  console returns 0.
- `init` only collects orphaned children; it starts nothing.
- Phase 7 deviations from the spec are listed in docs/DECISIONS.md
  (2026-10-03, phase 7).
- One terminal window (one kernel shell). No text selection, scrollback, or
  escape sequences in the terminal.
- Unregistered "Lumen-dev" and "Lumen" folders are left under `VirtualBox VMs` from
  VMs that were removed; they are not used.

## Open decisions waiting on the owner

Recorded in docs/DECISIONS.md (2026-10-03).

1. **libc: grow the in-tree libc or port mlibc.** Phase 7 wrote the minimal
   in-tree library the spec asks for; the choice matters from phase 17.
2. TCP/IP: write in-tree (current plan) or port lwIP. Needed before
   phase 14.
3. TLS and cryptography source: port Mbed TLS (recommended), port BearSSL,
   or write in-tree. Needed before phase 15.
4. Confirm the v2 phase order and the non-goals list.
5. Still open from 2026-09-14: confirm the scope of the network toolbox
   (docs/TO_FINISH.md, "Owner-added requirements").

## Next

1. Get the owner's go-ahead for phase 8.
2. Phase 8: start the other CPUs (INIT/SIPI trampoline), per-CPU data and
   run queues, work stealing, TLB shootdown, a spinlock inside every
   primitive that now relies on "interrupts off", lock-rank checker,
   per-CPU slab caches, `test smp`.
3. Look at the minor-page-fault cost (over budget).
4. Desktop polish as the owner sends reference screenshots.

## Known bugs

- `make` on the Windows-mounted tree occasionally prints
  "Clock skew detected" (drvfs timestamp rounding). Harmless so far.
- Backtraces cannot name a function that faults inside its own prologue
  (inherent to RBP walking); tests avoid it by making a call before faulting.
- The About window's CPU line is clipped at the window edge on long brand
  strings (needs ellipsis on the value column).
- `kmalloc`+`kfree` in the debug build sits at the edge of its 100 ns
  budget (80–101 ns between runs) now that the kernel has a stack
  protector.
