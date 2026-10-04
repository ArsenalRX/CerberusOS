# Changelog

What changed in each version of Cerberus, newest first.

> **Name:** the operating system was called **Lumen** until 2026-10-04,
> when the owner renamed it **Cerberus** (its file system lumfs became
> **cerfs**). Entries written before that date keep the old names.

**How this file is maintained** (docs/SPEC.md §23): every commit that changes
behaviour adds one line under "Unreleased", in the same commit, under one of
the headings Security, Fixed, Performance, Added, Changed, Removed (in that
order; omit empty ones). Lines are written for the person using the OS. At a
release, "Unreleased" is renamed to the version and date and a fresh
"Unreleased" block is added above it. A version with a Security entry is
marked "(security release)". Released blocks are never edited afterwards.

Version numbers before 1.0 (since 2026-10-04): `0.0.5a`, `0.0.5b` … `0.0.5j`,
then `0.0.6a`, one letter per release (docs/SPEC.md §23.1). Releases
0.3.0 to 0.7.0 used the older `0.PHASE.PATCH` scheme.

---

## Unreleased

## 0.0.5c — 2026-10-04 (security release)

Spec phase 9, files and file systems, is complete: Cerberus can keep files
on a disk. The system has a new name: Cerberus.

### Security
- File permissions (owner, group, others) are checked on every file
  operation; a user cannot read another user's private file, write a file
  they may only read, or look inside a directory closed to them.
- Mount options: read-only, no programs (`noexec`), no device files
  (`nodev`), no set-user (`nosuid`). The root file system is read-only;
  `/tmp` refuses device files; `/dev` refuses programs.
- The disk file system treats every disk as hostile: every field is checked
  before use, metadata carries CRC32C checksums, and a damaged disk gives an
  input/output error, never a crash. It is fuzzed: thousands of damaged disk
  images a minute are mounted and used by the real driver.
- A disk in use by a mounted file system cannot be opened for writing, so it
  cannot be formatted or overwritten by accident.
- A new disk block is never shown with what a deleted file left in it.
- Keyboard, mouse and screen device files are readable by root only (this
  matters from phase 15: until user accounts exist, every program runs as
  root).

### Performance
- One page cache for file contents and disks, with read-ahead for
  sequential reads and background write-back every 5 seconds: reading a
  64 MiB file a second time takes 6 ms instead of 0.7 s.
- A name cache remembers recently used paths.
- Closed files stay cached, so opening them again finds their data in memory.
- SATA disks are read and written by DMA (about 87 MB/s in QEMU).

### Added
- Files and folders: `/` (read-only, from the boot archive), `/tmp` (in
  memory), `/dev` (devices), and disks mounted anywhere, for example `/mnt`.
- **cerfs**, the disk file system, with a journal: pulling the power while
  files are being written leaves the disk consistent (tested by killing the
  machine mid-write and checking the disk with an independent reader).
- `mkfs.cerfs` formats a disk, both inside Cerberus (`mkfs.cerfs -y
  /dev/disk/sda`) and on the build machine (`mkfs.cerfs --check` verifies an
  image).
- Disk drivers: SATA (AHCI) and the older IDE controller. `pci` lists the
  PCI devices.
- Programs: `ls`, `cat`, `echo`, `mkdir`, `rm`, `rmdir`, `mv`, `cp`, `touch`,
  `stat`, `ln -s`, `sync`, `mount`, `umount`, `pwd`, `write`, `fstest`,
  `mkfs.cerfs`.
- The shell runs programs by name (`ls` instead of `run /bin/ls`), sends
  their output to a file with `>` or `>>`, has `cd` and `pwd`, and `mount`
  lists file systems and cache statistics.
- System calls for files: `open` with all the usual flags, `openat`,
  `seek`, `stat`, `lstat`, `fstat`, `mkdir`, `rmdir`, `unlink`, `rename`,
  `readdir`, `chdir`, `getcwd`, `dup`, `dup2`, `ioctl`, `truncate`, `sync`,
  `fsync`, `symlink`, `readlink`, `chmod`, `chown`, `mount`, `umount`,
  `time_ms` (docs/SYSCALLS.md). Programs inherit the shell's working
  directory; close-on-exec works.
- `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/urandom`, `/dev/console`.
- Self-tests `test vfs` and `test cerfsfuzz`.

### Fixed
- On VirtualBox the clock could run unevenly (it trusted a processor
  counter that VirtualBox does not keep steady), which made timing
  self-tests fail there. Cerberus now uses that counter only on real
  hardware that declares it steady and under KVM, and checks it three times
  before trusting it.

### Changed
- Opening a file in `/bin` for writing now fails with "read-only file
  system" instead of "operation not permitted".
- The operating system is now called **Cerberus** (it was Lumen). Its disk
  file system is **cerfs** (`mkfs.cerfs`, `mount -t cerfs`), the release
  image is `dist/cerberus.iso`, the shell prompt is `cerberus>`, and the
  launcher for Windows is `run-cerberus.cmd`.

## 0.0.5b — 2026-10-04

Desktop polish: smoother, rounder, and the terminal scrolls.

### Added
- The terminal keeps the last 1,000 lines. Scroll with the mouse wheel or
  Shift+Page Up / Shift+Page Down; a scrollbar shows where you are, and
  typing jumps back to the bottom.
- `test terminal` self-test (scrollback), included in `test all`.
- `gui` reports the slowest frame since it was last asked and how many
  pixels were actually written.

### Changed
- Window buttons look like Windows: minimise, maximise/restore and close as
  thin symbols in flat buttons; close turns red under the mouse.
- Rounded corners are larger and anti-aliased (smooth edges instead of
  steps) on windows, taskbar buttons, the menu and the scrollbar.
- The Super key opens the menu when pressed and released on its own; in a
  shortcut such as Super+T it no longer toggles the menu.
- Resizing the terminal keeps its text instead of clearing it.
- The About window shortens long values with "…" and no longer claims the
  Spec compiler is in progress (it has not been started).

### Performance
- Only pixels that changed are written to the screen. Dragging a window
  writes about a fifth of the pixels it did (82,000 instead of about
  424,000 per frame in a measured drag), which matters most in VirtualBox,
  where writing to the screen is slow.
- Frames are drawn as soon as mouse or keyboard input arrives (up to about
  160 per second) instead of waiting for the next 10 ms timer tick; the
  mouse pointer no longer reads back from video memory.

### Fixed
- Keys typed while the launcher menu is open no longer reach the terminal.

## 0.0.5a — 2026-10-04 (security release)

Spec phase 8, SMP, is complete: Lumen now uses every processor.

This is the first release under the new version scheme. It comes after
0.7.0 but is numbered 0.0.5a: from now on each release moves one letter
(0.0.5a, 0.0.5b … 0.0.5j, then 0.0.6a).

### Security
- Memory that is unmapped or made read-only is withdrawn from every
  processor before the change returns (TLB shootdown), so no processor can
  keep using a page that has been taken away.
- The order in which kernel locks may be taken is written down in one place
  and checked on every acquisition in debug builds; a wrong order stops the
  kernel with a report instead of becoming a rare deadlock.
- A fatal error stops all processors, not just the one that hit it.
- The kernel's reads and writes of program memory can no longer be steered
  at kernel memory speculatively.

### Performance
- All processors run threads. Work is spread across idle processors, and a
  processor with nothing to do takes waiting work from a busy one.
- The kernel heap gives each processor its own pool, so allocating memory
  does not make processors wait for each other.
- The kernel keeps time with the processor's own counter where it runs at a
  steady rate, instead of reading a timer chip: reading the time is far
  cheaper (wake-up latency 7–9 µs instead of 18–19 µs under QEMU).

### Added
- `test smp` self-test, and `test exceptions lo` (lock-order violation).
- `ps` shows which processor each thread is on and per-processor statistics.

### Changed
- The boot log shows each processor reporting in ("smp: cpu 1 online").
- "init: started as pid 1" is now printed by the kernel when it starts
  `init`.

### Fixed
- The scheduler benchmarks no longer depend on there being one processor.
- On machines without an HPET (such as VirtualBox), the clock ran several
  times too fast when two processors read it at once; it is now read by
  one processor at a time.
- The scheduler and SMP self-tests measure their busy work in timer ticks
  and no longer fail on slow virtual machines; `test all` includes
  `test smp`.

## 0.7.0 — 2026-10-03 (security release)

Spec phase 7, userland and system calls, is complete.

### Security
- Programs now run in user mode (ring 3), each in its own address space,
  and reach the kernel only through system calls. A program that crashes,
  touches kernel memory or runs a privileged instruction is ended and
  reported; the system carries on.
- Every system-call argument is checked. Bad pointers, lengths that wrap
  and unknown flags are refused with an error instead of reaching kernel
  code; the kernel reads and writes program memory through one guarded
  path only.
- SMEP, SMAP and UMIP are turned on when the CPU has them: the kernel can
  neither run nor casually touch program memory.
- Address-space layout randomisation: a program's code, stack and `mmap`
  memory land at different addresses on every run. Only
  position-independent programs are loaded, and nothing can be mapped
  writable and executable.
- A kernel random-number generator (ChaCha20, seeded from the CPU's
  hardware generator and interrupt timing) now supplies all randomness;
  programs get it through `getrandom`.
- Stack-overflow detection (stack protector) in the kernel and in every
  program.
- `make fuzz`: the program loader, the boot-archive reader and the
  system-call interface are attacked with damaged and random input on every
  release.

### Added
- System calls `exit write read open close mmap munmap fork execve waitpid
  getpid yield sleep_ms getrandom` (reference: `docs/SYSCALLS.md`, generated
  from the kernel's table).
- A small C library for programs (`userland/libc`) and the first programs,
  carried in a boot archive: `init` (process 1), `hello`, and test programs.
- Shell command `run <path> [arguments]` starts a program and reports how
  it ended, for example `run /bin/hello`.
- `bench` measures a system-call round trip.

### Fixed
- The context-switch benchmark (`bench`, `test sched`) no longer reports
  nonsense, or fails, when the shell thread happens to have been lowered in
  priority; this showed up on VirtualBox.

- A program's output line can no longer be split in the middle by a kernel
  message.

### Changed
- `dist/` holds exactly one file to boot, always named `lumen.iso`;
  `dist/VERSION.txt` says which version it is.

## 0.6.0 — 2026-10-03

Spec phase 6, threads and scheduling, is complete.

### Security
- Every kernel stack now has a guard page below it, including the stacks
  used for fatal errors. A runaway thread is reported as "kernel stack
  overflow" with its name instead of overwriting other memory.

### Performance
- The desktop stays responsive while long commands run: the compositor has
  its own high-priority thread and is woken the moment a key or mouse
  movement arrives.
- An idle desktop now uses almost no CPU (0% of timer ticks busy in a
  3-second sample); before, it spun a full core.

### Added
- Threads and processes, a four-level preemptive scheduler, sleeping and
  wait queues, and locks: spinlock, mutex, semaphore, condition variable,
  reader-writer lock.
- Shell commands `ps` (thread list) and `bench` (micro-benchmarks);
  `make bench`; `docs/BENCH.md` with the first recorded numbers.
- `test sched` self-test.

### Changed
- The kernel shell and the desktop run as threads. `idle hlt|spin` now sets
  what the idle thread does.

### Fixed
- The automated tests no longer fail on a busy host, and UEFI boots work in
  the test tool.

## 0.5.1 — 2026-10-03

### Fixed
- VirtualBox: the keyboard no longer stops responding after a few keys when
  the VM has more than one CPU. The CPUs the kernel is not using yet now
  sleep instead of spinning, which also stops them using a full host core
  each.

### Changed
- `dist/` now holds only the newest ISO; making a release replaces the old
  one and points the VirtualBox VM "Lumen" at the new file.

## 0.5.0 — 2026-10-03 (security release)

Spec phase 5, the kernel heap, is complete.

### Security
- The kernel's memory allocator protects its own bookkeeping: a corrupted
  free list is detected and stops the system instead of being followed.
- Freeing the same memory twice, or freeing something that was never
  allocated, stops the system with a clear message.
- Debug builds also catch writes past the end or before the start of an
  allocation, and writes to memory after it was freed.
- `kfree_sensitive` wipes memory before releasing it (for keys and
  passwords in later phases).

### Added
- Kernel heap: `kmalloc`, `kzalloc`, `kfree`, `krealloc`.
- `heapstat` shell command: heap usage and, in debug builds, every
  outstanding allocation grouped by the code that made it.
- `test heap` self-test and three corruption tests
  (`test exceptions fl|waf|df`).

### Changed
- The desktop's pixel buffers and the memory manager's records now come
  from the heap.

## 0.4.0 — 2026-10-03 (security release)

Spec phase 4, virtual memory, is complete.

### Security
- No memory can be both writable and executable; every request for such a
  mapping is refused.
- All memory outside the kernel's code is now non-executable. Before this
  release the bootloader's mapping left all of RAM executable.
- Kernel code and read-only data can no longer be written; kernel data can
  no longer be executed.
- Memory handed to an address space is always zeroed first, so nothing left
  behind by a previous owner can be read.
- The first 64 KiB of an address space can never be mapped, so a null
  pointer always faults.
- A stack overflow on a guarded kernel stack is reported and stops the
  system instead of overwriting other memory.
- Stack variables are zero-initialised by the compiler; debug builds stop
  with the source line on undefined behaviour such as signed overflow.

### Added
- Virtual memory manager: private address spaces, memory regions, memory
  allocated on first use, copy-on-write copies of an address space, 2 MiB
  pages for large kernel mappings.
- `test vmm` self-test; `test exceptions so` (stack overflow) and
  `test exceptions ub` (undefined behaviour).

### Changed
- The other CPUs now wait inside the kernel instead of inside the
  bootloader's code.

## 0.3.0 — 2026-10-03

First release under the `0.PHASE.PATCH` scheme: spec phases 0–3 are complete.

### Fixed
- Keyboard and mouse can no longer swallow each other's input: both
  interrupts now read the PS/2 controller through one shared routine that
  sends each byte to the right device.
- The desktop no longer freezes on VirtualBox when it runs on the Hyper-V
  backend: while the desktop is up the kernel keeps polling instead of
  halting the CPU, and copies pixels to the screen with plain stores. (This
  uses a full CPU while idle; the scheduler in phase 6 replaces it. Verified
  in QEMU; not yet re-verified on VirtualBox.)

### Added
- Shell commands `gui` (compositor statistics) and `irqs` (interrupt counts).
- The boot log reports how long the first desktop frame took.

### Changed
- Versions are now labelled `0.PHASE.PATCH`; development builds show
  `-dev+<commit>` after the number. The number lives in the `VERSION` file.
- Specification version 2: security, privacy, networking and performance are
  requirements (see `docs/DECISIONS.md`, 2026-10-03).

## 0.0.1 — 2026-09-14

First snapshot. Covers spec phases 0–3 and the desktop preview.

### Added
- Boots through Limine on BIOS and UEFI; boot banner on screen and serial.
- CPU setup: GDT/TSS, interrupts with symbolised crash dumps, ACPI, APIC,
  HPET, a 100 Hz timer, real-time clock.
- Physical memory allocator with a self-test.
- Kernel shell with PS/2 keyboard input.
- Desktop preview: windows with shadows that move, resize, stack, minimise
  and maximise; panel with launcher, task buttons, memory use and clock;
  Terminal, About, System Monitor and Memory Map windows; PS/2 mouse;
  Alt+Tab, Alt+F4 and Super hotkeys.
