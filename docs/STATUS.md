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

Last updated: **2026-10-06**.

## Version

Latest release: **0.0.5j** (2026-10-06, tag `v0.0.5j`): hard links, and
polish round 4 — virtual desktops, the System Monitor with graphs and a
process table, a BMP image viewer and wallpaper from a file, repeating
reminders with snooze, auto-lock, Settings scrolling (B-012), `clear`,
the task-button menu, two-press power off. Before it: 0.0.5i
(2026-10-05): the page fault inside its budget, `sigprocmask`,
`port_try_send`, keyboard lights; 0.0.5h (2026-10-05), polish round 2,
the owner's eight picks: saved settings and reminders on a `/data` disk,
text selection and a clipboard, Files, Notes, Calculator, keyboard
layouts and repeat, a lock screen (salted SHA-256), a tray with quick
settings and a CPU meter. Before it: 0.0.5g (2026-10-05, polish round 1:
calendar, notifications, animations, snapping, Alt+Tab, Settings cards,
VirtualBox absolute mouse), 0.0.5f (2026-10-04, phase 11, IPC), 0.0.5e (phase 10, drivers), 0.0.5d (anti-aliased text), 0.0.5c (phase 9,
files; the OS renamed **Cerberus**, formerly Lumen), 0.0.5b (desktop
polish), 0.0.5a (phase 8). Every release moves one letter (`0.0.5a` …
`0.0.5j`, then `0.0.6a`; docs/SPEC.md §23.1); releases 0.3.0 to 0.7.0 keep
their old numbers. The release is always `dist/cerberus.iso`
(`dist/VERSION.txt` names the version); each release overwrites it. The
tree now builds as `0.0.6a-dev+<commit>`. The source is also on GitHub:
https://github.com/ArsenalRX/CerberusOS (`origin`; pushed on the owner's
request, 2026-10-04: push `main` and the tags after every verified release,
and keep README.md and `docs/screenshots/` current). History:
docs/CHANGELOG.md.

**Where the last session stopped (2026-10-06, early):** 0.0.5j is
released, in `dist/`, pushed; the owner's VirtualBox VM is "Cerberus
0.0.5j" (in their `VirtualBox VMs` folder, 4 CPUs, a 1 GiB disk for
`/data`). The owner's last instruction: polish for a three-hour box, then
**move on to phase 12**. The box is spent: round 3 has a–(hard links)
done and b–f open; round 4 has items 1, 2, 5, 8 done and 3, 4, 6, 7 part
done (docs/DECISIONS.md, 2026-10-06, lists what is left). **Next session
starts phase 12 (Pane)** per docs/TO_FINISH.md unless the owner picks
more polish by number from the Desktop list. Nothing is half-written.

---

## Current phase

**Phase 11 — IPC: COMPLETE (2026-10-04).** Phases 0–3 complete
2026-09-14; phases 4–7 complete 2026-10-03; phases 8 to 11 complete
2026-10-04. Desktop preview (owner-requested, brought forward from phases
12-13) runs in its own thread; 0.0.5g (2026-10-05) is a polish release of
it.

Next up: more desktop polish rounds chosen by the owner by number, then
**Phase 12 — Pane, the window server as a user program** (SPEC §5 and
§5A) when the owner says so.

Spec is version 2 (2026-10-03): security, privacy, networking and
performance are requirements; our own web browser is phase 15B (owner,
2026-10-04). See docs/DECISIONS.md.

## Last verified

All on 2026-10-06, on the 0.0.5j code (0.0.5h and 0.0.5i the day before:
49 of 49 each).

- `make test` in QEMU/KVM, 4 CPUs: all **53** integration tests passed
  (0.0.5j added `cerfs-links`, `desktop-qol2`, `desktop-desks`,
  `desktop-viewer`). The run was on the final kernel; the release ISO
  differs only in its version string.
- `make bench` (0.0.5i): docs/BENCH.md block of 2026-10-05 — minor page
  fault 1,394 ns, within budget; the rest unchanged.
- `make fuzz`: not re-run since 0.0.5f.
- **VirtualBox 7.2.6 (Hyper-V backend), a temporary VM "cerberus-check"
  with 4 CPUs, 2 GiB, VBoxVGA**: the first build faulted at boot (B-013,
  fixed); the final build booted to the desktop, `vmmdev` reported that
  the host sends absolute mouse positions, Super opened the launcher,
  Alt+Tab switched windows. The pointer itself cannot be driven by
  VBoxManage: the owner checks it on their ultrawide.
- Not run this session: UEFI boot, software emulation (TCG), a `DEBUG=0`
  build (last checked on 0.5.0), the fatal-error tests on VirtualBox, a
  power cut on VirtualBox (done in QEMU only), mouse dragging on VirtualBox
  (VBoxManage cannot move the mouse), NVMe and virtio disks outside QEMU.

## How to run it

- `run-cerberus.cmd` (double-click on Windows): boots the ISO in QEMU with KVM
  inside WSL2, window on the desktop via WSLg, serial log in
  `logs/qemu-serial.log`. `run-cerberus.cmd uefi` boots through OVMF;
  `run-cerberus.cmd build` rebuilds first. This is the fast, accurate path.
- VirtualBox, 2026-10-05: the "Cerberus" VM described below is no longer
  registered. The only VM is the owner's wizard-made **"6"** (2 CPUs,
  2.7 GiB, IDE, DVD = `dist/cerberus.iso`); it was 32-bit and was switched
  to 64-bit that day (B-001). It was left in a *saved* state, so it could
  not be renamed; when powered off, rename it `Cerberus <version>`
  (CLAUDE.md). The boot menu no longer waits (B-002). Automated checks use
  a temporary "cerberus-check" VM in the session scratchpad (4 CPUs,
  2 GiB, VBoxVGA, serial to a file), deleted afterwards. Earlier, as of
  2026-10-04 (late evening), one VM was registered:
  **"Cerberus"** in `C:\Users\<owner>\VirtualBox VMs\Cerberus` (64-bit,
  4 CPUs, 2 GiB, VBoxVGA with 32 MiB, DVD = `dist/cerberus.iso`, a 1 GiB
  SATA disk, serial log in `logs/vbox-serial.log`). It was recreated that
  evening: the earlier "Cerberus" VM and the temporary "cerberus-check" VM
  (which the owner had been using; it lived in a temp folder that was
  cleaned) were both gone. Never keep a VM the owner uses in a temp folder. In Cerberus,
  format the disk once with `mkfs.cerfs -y /dev/disk/sda`, then
  `mount -t cerfs /dev/disk/sda /mnt`. For a VM made with the wizard, point its DVD drive at
  `dist/cerberus.iso`. The wizard creates such VMs as OS type "Other/Unknown", which is 32-bit and
  hides 64-bit mode, and the bootloader then reports that the CPU is not
  64-bit. `make dist` (via `tools/vbox-attach.sh`) fixes that and re-points
  the DVD drive for every powered-off VM that boots an ISO from `dist/`.
  If the boot menu stays on screen, press Enter. VirtualBox runs on the
  Hyper-V backend on this host (WSL2 keeps Hyper-V on), so it is slower
  than `run-cerberus.cmd`.
- `dist/cerberus.iso`: the release ISO for any VM (always this name).
  `make RELEASE=1 dist` overwrites it and re-points
  the VirtualBox VM.

## What works (verified 2026-10-05 by `make test` in QEMU/KVM)

- Host environment: WSL2 Ubuntu 24.04 with the cross toolchain
  (`toolchain/out/`, binutils 2.42 + GCC 13.3.0), QEMU 8.2 with KVM, OVMF.
- Build: `make` (two-pass link embedding the symbol table), `make iso`
  (hybrid BIOS/UEFI, Limine 11.4.1), `make dist`, `make bench`.
- Test: `make test` runs every `tests/integration/*.expect` through
  `tools/qemu-probe.py` (headless QEMU + QMP; directives `!send`, `!key`,
  `!mouse`, `!mouseto`, `!button`, `!wheel`, `!screenshot`, `!wait`, `!kill`;
  `#!` lines set the machine type, attach fresh or kept disk images and ask
  for a host-side check afterwards). The probe waits for expected output
  rather than fixed times. `tools/run-tests.sh name...` runs a few.
- Fuzz: `make fuzz` (`FUZZ_SECONDS=60` per harness by default): host builds
  of `kernel/proc/elf.cpp` and `kernel/fs/ustar.cpp` under AddressSanitizer
  (`tests/fuzz/`), then `/bin/sysfuzz` and `test cerfsfuzz 60` inside QEMU. A crashing input is
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
- Phase 8: every CPU runs threads (started through the bootloader); per-CPU
  data behind GS; per-CPU run queues with work stealing under one scheduler
  lock; inter-processor interrupts (reschedule, TLB flush, stop on panic);
  TLB shootdown before any frame is freed; a lock per address space;
  per-CPU heap slabs; lock-rank checker (`kernel/lib/lock_order.h`);
  reference clock on the time-stamp counter; `test smp`, `ps` per CPU.
- Phase 9: files. VFS (`kernel/fs/vfs.cpp`): one tree over several file
  systems, mounts, `.`/`..`, symbolic links (limit 40), permission checks,
  mount flags, a name cache. tmpfs; the boot archive unpacked as a
  read-only `/`; `/dev` with device nodes; `/tmp`. Page cache with
  read-ahead and a write-back thread. **cerfs** (journal, CRC32C) with
  `mkfs.cerfs` on the host (also `--check`) and inside the OS. Disk drivers:
  AHCI (DMA) and ATA PIO; PCI enumeration (`pci`). 39 system calls. libc
  file API. Programs: `ls cat echo mkdir rm rmdir mv cp touch stat ln sync
  mount umount pwd write fstest mkfs.cerfs`. Shell: programs by name, `>`
  and `>>`, `cd`, `pwd`, `mount`. `test vfs`, `test cerfsfuzz`.
- VirtualBox guest device (`kernel/drivers/vmmdev.cpp`, 0.0.5g): the
  host's absolute mouse position on every PS/2 packet (B-003).
- 0.0.5j: hard links; four virtual desktops; System Monitor graphs and
  process table (`sched_process_snapshot`); BMP viewer
  (`kernel/gui/viewer.cpp`) and wallpaper from a file; repeating
  reminders and snooze; auto-lock; Settings scrolling; `clear`.
- 0.0.5h: `/data` (the first cerfs disk labelled `data`, mounted at boot;
  `fs_data_format` from Settings → Storage); `desktop.conf` and
  `reminders.txt` on it; a clipboard and terminal selection; Files, Notes
  and Calculator (`kernel/gui/files.cpp`, `notes.cpp`, `calc.cpp`);
  keyboard layouts US/UK/DE/FR and typematic settings (`ps2kbd`); a lock
  screen with salted SHA-256 (`kernel/lib/sha256.cpp`, `test sha256`);
  the tray (night light, CPU per core, notification history). The full
  list is docs/FEATURES.md.
- Phase 10: drivers. A driver registry (`drivers`); PCI capabilities and MSI;
  AHCI by interrupt where MSI exists; NVMe and virtio-blk (polled); the
  keyboard in scancode set 2 with the full key map, Num Lock and repeat;
  input events through `/dev/input/kbd0` and `mouse0`; `/dev/fb0`; ACPI
  power off and restart (`poweroff`, `reboot`, system call 64); `runas`;
  sticky `/tmp`.
- Phase 11: IPC. Kernel objects behind descriptors (`kernel/ipc/`): named
  ports with channels, access control and kernel-recorded peer identity;
  shared memory; futexes (shared and private); event queues over ports,
  input devices, timers and child exit. Signals with user handlers
  (`kernel/proc/signal.cpp`). Threads in a process (`thread_spawn`/`join`,
  FS-base TLS), a process ending with all its threads, a descriptor table
  safe to share. Private file `mmap`, `mprotect`, `kill`, identity calls,
  `sysinfo`: 74 system calls. libc: pthreads, mutex and condition variable
  on futexes, `__thread`, a locked allocator. Programs `ipctest`,
  `threadtest`, `sigtest`, `eventtest`, `mmaptest`.
- Fatal-error tests (each halts by design): `test exceptions
  de|ud|pf|pfw|gp|bp` (CPU exceptions), `so` (a runaway thread reported as
  a kernel stack overflow), `ub` (undefined behaviour), `fl|waf|df` (heap
  corruption), `lo` (lock-order violation).
- Desktop preview (`kernel/gui/`, `kernel/gfx/`), now driven by the
  compositor thread:
  - libgfx: fills, rounded rects, alpha blits, scaled blits, lines, circles,
    gradients, box blur, PSF text with ellipsis, clip stack.
  - Compositor: wallpaper, back buffer, damage rectangles, windows with
    rounded corners, cached shadows, title bars with close/maximise/
    minimise, move and resize by mouse (the cursor changes shape over
    edges and corners since 0.0.5f), stacking, software cursor.
  - Panel: a floating rounded bar (or docked): logo launcher button, search
    box, task buttons with app icons (which narrow to fit), a memory meter,
    clock and date. Launcher menu with a search field, app
    list, Power off and Restart (0.0.5f).
  - Windows: Terminal (the kernel shell), About, System Monitor, Memory
    Map, Settings.
  - Settings (0.0.5f): accent colour, three wallpapers, corner shape,
    taskbar and clock options; window-border effects (static, breathing,
    flashing, rainbow, chase; colour, width, speed, glow); screen
    resolution through `kernel/drivers/bga.cpp` where the adapter has the
    Bochs interface. Preferences are not saved across a restart.
  - Hotkeys: Alt+Tab (switcher), Alt+F4, Super (on release, alone),
    Super+T, Super+M, Super+D, Super+arrows (snap), Print Screen.
  - 0.0.5g: open/close/minimise animations; snapping at the screen edges;
    double-click to maximise; middle-click a task button to close;
    right-click menu on the desktop; the clock opens a calendar with
    reminders; notifications (`notify`); Settings as cards (any accent
    hue, six wallpapers, time zone, 18 resolutions to 5120x1440, frame
    rate 30–144); terminal history and Tab completion; the full list is
    docs/FEATURES.md.
  - Windows-style caption buttons; anti-aliased rounded corners (0.0.5b).
  - Terminal scrollback of 1,000 lines: mouse wheel, Shift+Page Up/Down,
    scrollbar; resizing keeps the text (0.0.5b, `test terminal`).
  - Only changed pixels are written to the screen; frames follow input
    immediately (0.0.5b).
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
- Unmapping or restricting memory is withdrawn from every CPU before the
  change returns or the frame is reused (TLB shootdown). (0.0.5a)
- Lock order is checked on every acquisition in debug builds. (0.0.5a)
- A panic stops every CPU. (0.0.5a)
- The user-copy bounds check masks the address without a branch, so a
  mispredicted check cannot read kernel memory speculatively. (0.0.5a)
- File permissions and mount flags (`ro`, `noexec`, `nodev`, `nosuid`) are
  enforced in the VFS; `test vfs` shows a non-root credential refused a
  0600 root file and a `noexec` mount refusing to run a program. The root
  file system is read-only. (0.0.5c)
- cerfs validates every on-disk field and checksums its metadata; damaged
  images give an I/O error (`make fuzz`: thousands of damaged images, no
  panic). A power cut leaves the disk consistent (`cerfs-crash-1/2`). A new
  block never exposes a deleted file's contents. (0.0.5c)
- A disk claimed by a mounted file system cannot be opened for writing.
  (0.0.5c)
- `/dev/random` and `/dev/urandom` exist; screen and input device nodes are
  mode 0600. (0.0.5c)
- A program that is not root cannot open the screen, the keyboard, the
  mouse or a disk (`drivers` test, as uid 1000); `/tmp` is sticky. (0.0.5e)

- A port refuses a connection from a user without write permission to it,
  and the receiver's knowledge of who connected comes from the kernel
  (`user-ipc`, as uid 1000). Queues are bounded. (0.0.5f)
- Signals go only to processes of the same user; root, once given up with
  `setuid`, cannot be regained (`user-signals`, `user-ipc`). (0.0.5f)
- The new calls are in the random system-call fuzzer. (0.0.5f)

Still missing:

- The desktop and the kernel shell still run in ring 0. Permissions are
  enforced, but there are no user accounts yet: every program runs as uid 0
  (phase 15), so in practice any program can read and change any file and
  disk.
- Where the hypervisor hides SMEP/SMAP (VirtualBox on this host), the
  kernel has only its software checks.
- Retpolines have not been evaluated; the stack-protector guard is one
  global value, not per CPU.
- Nothing is encrypted on disk (phase 18).
- No kernel ASLR (phase 19).
- No networking, so nothing leaves the machine. Files written to a cerfs
  disk persist, unencrypted.

Do not put real data in the system before phase 15.

## Performance state

Numbers are in docs/BENCH.md (latest block 2026-10-04, 0.0.5c). Under
QEMU/KVM with 4 CPUs: context switch 24 ns, system-call round trip 32 ns,
wake-up latency 9 µs average, allocation 80 ns per pair (debug build), idle
desktop 0% of ticks busy — all within budget. **Over budget: a minor page
fault takes 2.0–2.9 µs against 2 µs** (not profiled). Storage: 87 MiB/s
sequential read from SATA with a cold cache, a cached 64 MiB read in 6 ms;
IDE by programmed I/O manages 2 MiB/s.

On VirtualBox (Hyper-V backend) writing to the screen is slow (about 30
million pixels per second); since 0.0.5b only changed pixels are written.
The clock there is the PIT: VirtualBox's time-stamp counter is not
trustworthy (docs/DECISIONS.md, 2026-10-04).

Known limits:

- One VFS lock serialises all file-system work. NVMe, virtio and (on
  VirtualBox) SATA disks are polled: a thread reading a disk keeps its CPU
  busy. One command at a time per disk.
- Frame pacing follows the 10 ms tick (no one-shot timer). Input is handled
  immediately.
- Waking a thread on another CPU costs an inter-processor interrupt; one
  scheduler lock for all CPUs (DECISIONS, phase 8).
- The shell polls the serial port once per tick (no serial interrupt yet).
- Debug builds carry the undefined-behaviour checks and heap red zones.

## Half-done

- `early_map` still hands out kernel virtual addresses on its own (first GiB
  of the vmalloc region); drivers can move to `mmap_device` when convenient.
- File-backed `mmap` is private and read in when mapped; shared and
  demand-paged file mappings are not done. Phase 11 deviations are in
  docs/DECISIONS.md (2026-10-04, phase 11): no signal masks, `port_send`
  cannot be non-blocking, ports cannot be passed through ports, unjoined
  threads are kept until their process ends. `read` from the console returns 0 for programs (terminals are
  phase 17). No hard links. No `fsck.cerfs` (phase 18): after a power cut a
  file deleted while still open can leave a leaked inode, which the host
  checker reports as a warning.
- `init` only collects orphaned children; it starts nothing.
- Phase 7 deviations from the spec are listed in docs/DECISIONS.md
  (2026-10-03, phase 7).
- One terminal window (one kernel shell). No text selection or escape
  sequences in the terminal (scrollback exists since 0.0.5b).
- Unregistered "Lumen-dev" and "Lumen" folders are left under `VirtualBox VMs` from
  VMs that were removed; they are not used.
- Phase 8 deviations from the spec are listed in docs/DECISIONS.md
  (2026-10-03, phase 8): one scheduler lock, cross-CPU wake-up costs an
  interrupt, not tickless, at most 32 CPUs.

## Open decisions waiting on the owner

Recorded in docs/DECISIONS.md (2026-10-03).

2. **libc: grow the in-tree libc or port mlibc.** Phase 7 wrote the minimal
   in-tree library the spec asks for; the choice matters from phase 17.
3. TCP/IP: write in-tree (current plan) or port lwIP. Needed before
   phase 14.
4. TLS and cryptography source: port Mbed TLS (recommended), port BearSSL,
   or write in-tree. Needed before phase 15.
5. Confirm the v2 phase order and the non-goals list.
6. Still open from 2026-09-14: confirm the scope of the network toolbox
   (docs/TO_FINISH.md, "Owner-added requirements").

## Next

1. Phase 12: Pane, the window server as a user program, on ports, shared
   memory and event queues; `libpane`; the kernel desktop is the model.
2. Look at the minor-page-fault cost (over budget).
3. Desktop: window open/close animations; frame pacing from a one-shot
   timer.

## Known bugs

The ledger is docs/BUGS.md (open: B-010 `eventtest` on VirtualBox, B-011
nothing is saved across a restart, B-012 Settings does not scroll below
its minimum size). Other oddities:

- `help` is now longer than the terminal window is tall; it relies on the
  scrollback.
- `make` on the Windows-mounted tree occasionally prints
  "Clock skew detected" (drvfs timestamp rounding). Harmless so far.
- Backtraces cannot name a function that faults inside its own prologue
  (inherent to RBP walking); tests avoid it by making a call before faulting.
- `kmalloc`+`kfree` in the debug build sits at the edge of its 100 ns
  budget (80–101 ns between runs) now that the kernel has a stack
  protector.
