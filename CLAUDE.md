# Cerberus — working notes for Claude Code

(The OS was called Lumen until 2026-10-04; history files and git log use
the old name. Its file system lumfs is now cerfs.)

Cerberus is a from-scratch x86-64 hybrid-kernel OS with its own desktop, plus
**Spec**, its systems language (the spec calls it "Glint"; renamed, see
docs/DECISIONS.md top: `glintc`→`specc`, `.gl`→`.spec`). Owner-facing
overview: docs/ABOUT.md. Plain-language plan: docs/ROADMAP.md.

## Start of every session

1. Read `docs/STATUS.md` (what is true now) and `docs/TO_FINISH.md` ("Right
   now" names the next job). `docs/SPEC.md` is the full plan and wins over
   every other doc; `docs/DECISIONS.md` records why things are the way they
   are (read the newest entries).
2. Stop after each phase or distinct piece of work: report, list what the
   next phase adds, and wait for the owner's go-ahead — unless the owner has
   said to keep going for the session.

## Build, test, run (all inside WSL Ubuntu-24.04)

```sh
wsl -d Ubuntu-24.04 -u root -- bash -c 'cd /mnt/d/Programs/OS && make iso'
```

- `make` / `make iso` — kernel, userland, `build/cerberus.iso`.
- `make test` — every `tests/integration/*.expect` in headless QEMU/KVM (4
  CPUs) via `tools/qemu-probe.py`; ~25 min. Logs: `build/test-<name>.log`.
- `make bench` (record in docs/BENCH.md), `make fuzz` (60 s per harness).
- `make dist` — overwrites `dist/cerberus.iso` (+`VERSION.txt`) and re-points
  every powered-off VirtualBox VM that boots from `dist/`. `make RELEASE=1
  dist` for a release (tagged commit, plain version number).
- Probe directives in `.expect` files: `!send`, `!key`, `!wait`, `!mouse`,
  `!mouseto`, `!button`, `!wheel`, `!screenshot`, `!kill` (power cut); other
  lines must appear in the serial log in order. Setup lines at the top:
  `#!machine pc` (legacy IDE; default q35 = AHCI), `#!disk new|blank <size>
  <name>` / `#!disk keep <name>` (images in `build/disk-<name>.img`),
  `#!hostcheck[-replay] <name>` (independent check with `mkfs.cerfs
  --check` afterwards).
- `tools/run-tests.sh name...` runs chosen integration tests and prints the
  key lines; `make tools` builds the host `build/tools/mkfs.cerfs`. A scratch `.expect` in `build/` plus
  `python3 tools/qemu-probe.py build/cerberus.iso --wait 6 --expect F` (no
  `--quiet`, to see serial output) is the quickest way to look at a change.

## Pitfalls (learned the hard way)

- **From Git Bash, always `export MSYS_NO_PATHCONV=1`** before calling
  `wsl`, or `/tmp/...` arguments are rewritten to Windows paths.
- Inside `wsl ... bash -c '...'`, `$?` and `$var` arrive empty or
  pre-expanded. Check build results by grepping the log
  (`grep -E "error:|undefined reference" build/iso.log`) and the ISO's
  timestamp — never trust an `rc=` echo. A failed link leaves the **old**
  ISO in place, and testing it wastes a cycle.
- Long jobs (`make test`, `make fuzz`): start detached with
  `setsid nohup bash -c "... > build/x.log 2>&1; echo DONE >> build/x.log" &`
  and poll the log in `build/` (WSL's `/tmp` can vanish when WSL restarts).
- `sed` with `\n` in a replacement inside a C string literal splits the
  string; use the Edit tool or a Python script (run with WSL's `python3`;
  Windows `python3` is not installed) for multi-line source edits.
- A Python patch script written through a Git Bash heredoc loses one level
  of backslashes (`\\n` arrives as a real newline inside a C string). Write
  the script with the Write tool into `build/wip/` (ignored by git, not
  compiled) and run it with WSL's python3. After any scripted edit, grep
  for a line that is just `");`.
- A detached job started as the last thing in a `wsl ... bash -c` dies with
  it: put `sleep 2` after the `setsid nohup ... &` and check that the log
  file exists before waiting on it.
- `make test` now takes about 25 minutes (41 tests). It tests
  `build/cerberus.iso`: do not rebuild while it runs. To keep working, copy
  the ISO and test the copy.
- Timing checks in tests: a wait counted in timer ticks and measured with
  the reference clock can read 1 ms short. Leave a margin.
- The kernel links without libgcc: no 128-bit division (`__udivti3`); 128-bit
  multiplication is fine.
- VirtualBox on this host runs on the Hyper-V backend: slow video memory
  (~100 MB/s), slow interrupts, no HPET (the clock falls back to the PIT,
  the TSC there reads ~4–7 MHz). Budgets in SPEC §20 are for QEMU/KVM.
  Always also verify on VirtualBox with **4 CPUs** — it has caught bugs
  QEMU did not (PIT race, keyboard stall).
- VBoxManage: check `VBoxManage list vms` and `showvminfo <vm>
  --machinereadable | grep VMState` before touching a VM; the owner creates
  and deletes VMs in the GUI. Wizard-made VMs are 32-bit ("stuck at Limine")
  until `--ostype Other_64 --longmode on`. Use a temporary VM in the
  scratchpad for automated checks and delete it afterwards;
  `keyboardputscancode` (arrows need the `e0` prefix) and `screenshotpng`
  drive it; the mouse cannot be driven.

## Owner preferences

- After every finished update, `make dist` so `dist/cerberus.iso` (one file,
  always overwritten) boots the new build in the owner's VirtualBox VM.
- Versions: `0.0.5a`, `0.0.5b` … `0.0.5j`, then `0.0.6a` (SPEC §23.1); the
  next version is in `./VERSION`; history in docs/CHANGELOG.md.
- Desktop must look sleek ("Linux and Windows combined"), Windows-style
  caption buttons, smooth and responsive; privacy and security first;
  efficient on memory and CPU.
- Commit messages: `phaseN: …`, `release: …`, `desktop: …`, `docs: …`, with
  the Co-Authored-By line.
- GitHub: `origin` is https://github.com/ArsenalRX/CerberusOS. After every
  verified release push `main` and the tags, with README.md (the public
  overview) and `docs/screenshots/` brought up to date.
- The owner steers by screenshot: they send pictures of the VM and ask for
  visual changes mid-task. Do those promptly, show a screenshot back (the
  probe's `!screenshot`), and keep the desktop customisable.

## Code map

- `kernel/arch/x86_64/` GDT/IDT/interrupts, per-CPU data (GS), SMP and IPIs,
  CPU features (`g_cpu`), ACPI.
- `kernel/mm/` frame allocator (`pmm`), virtual memory (`vmm`: address
  spaces, VMAs, COW, shootdown), heap (`kheap`: per-CPU slabs), user copies.
- `kernel/sched/` scheduler (one lock, per-CPU queues, work stealing), sync
  primitives, interruptible waits. `kernel/proc/` processes and their
  threads, ELF loader, `signal.cpp` (handlers, `user_return` on every way
  back to ring 3). `kernel/syscall/` dispatch generated from `table.def`
  (→ docs/SYSCALLS.md); descriptors are looked up with `FdRef`.
- `kernel/ipc/` kernel objects behind descriptors (`object.cpp`: also the
  shared descriptor table and `poll_wake`), `port`, `shm`, `futex`, `event`.
- `kernel/drivers/` LAPIC/IOAPIC/HPET/PIT, `refclock` (TSC, else HPET, else
  PIT), PS/2, serial, RTC, framebuffer console, `pci`, disks (`ahci` DMA,
  `ata` PIO, `ramdisk`), `nvme`, `virtio_blk`, `input` (/dev/input), `bga`
  (resolution switching on VM display adapters).
- `kernel/fs/` `vfs` (tree, mounts, permissions, name cache, one sleeping
  VFS lock), `file` (open files, flags), `tmpfs` (also the read-only root
  from the boot archive and `/dev`), `dev` (device switch), `block` (disks),
  `pagecache` (one cache, read-ahead, write-back thread, journal holds),
  `cerfs` + `cerfs_format.h` + `cerfs_mkfs.h` (on-disk format shared with
  `tools/mkfs-cerfs.cpp` and `userland/bin/mkfs.cerfs.cpp`), `fs.cpp` (boot
  mounts).
- `kernel/gfx/` libgfx software renderer (anti-aliased rounded shapes).
  `kernel/gui/` the in-kernel desktop (`desktop.cpp`: compositor, windows,
  floating panel, launcher with search, Settings window and `g_prefs`,
  border effects, wallpapers, `set_resolution`; `terminal.cpp`: shell
  terminal with scrollback). Desktop tests click fixed coordinates
  (1280x800): moving anything in the panel, launcher or Settings means
  updating `tests/integration/desktop*.expect`. It
  moves to userland as Pane in phase 12.
- `kernel/lib/` kprintf, console lock, kernel shell (`shell.cpp`), panic,
  CSPRNG, lock ranks (`lock_order.h` — add new locks there).
- `userland/` libc (`cerberus.h`; `thread.cpp` has TLS, pthreads and the
  futex locks; `ipc.cpp` the phase 11 wrappers), programs in `bin/` (packed into
  `boot/initrd.tar`; `fileutils.cpp` is one program under many names, listed
  in the Makefile's `FILEUTILS_NAMES`), `etc/` files.
- `tests/kernel/` in-kernel self-tests (register in `registry.cpp`; `test
  all` runs them — update `tests/integration/shell-tests.expect` when the
  count changes). `tests/integration/` QEMU scenarios. `tests/fuzz/` host
  fuzzers.
- `spec/` the Spec language: empty until phase 16.
- System calls: add to `kernel/syscall/table.def` (generates the dispatcher,
  docs/SYSCALLS.md and libc's numbers), write `sys_<name>`, extend
  `userland/bin/badptr.cpp` with bad-pointer checks for it.

## Rules that never bend (SPEC §19)

Never dereference a user pointer outside the user-copy routines; never map
memory writable and executable; never log user content; never add a
dependency or a network connection the owner has not approved; treat every
value from a device, disk, file or network as hostile.
