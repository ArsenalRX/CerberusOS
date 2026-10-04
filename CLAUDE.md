# Cerberus — working notes for Claude Code

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
  CPUs) via `tools/qemu-probe.py`; ~8 min. Logs: `build/test-<name>.log`.
- `make bench` (record in docs/BENCH.md), `make fuzz` (60 s per harness).
- `make dist` — overwrites `dist/cerberus.iso` (+`VERSION.txt`) and re-points
  every powered-off VirtualBox VM that boots from `dist/`. `make RELEASE=1
  dist` for a release (tagged commit, plain version number).
- Probe directives in `.expect` files: `!send`, `!key`, `!wait`, `!mouse`,
  `!mouseto`, `!button`, `!wheel`, `!screenshot`; other lines must appear in
  the serial log in order. A scratch `.expect` in `build/` plus
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

## Code map

- `kernel/arch/x86_64/` GDT/IDT/interrupts, per-CPU data (GS), SMP and IPIs,
  CPU features (`g_cpu`), ACPI.
- `kernel/mm/` frame allocator (`pmm`), virtual memory (`vmm`: address
  spaces, VMAs, COW, shootdown), heap (`kheap`: per-CPU slabs), user copies.
- `kernel/sched/` scheduler (one lock, per-CPU queues, work stealing), sync
  primitives. `kernel/proc/` processes, ELF loader. `kernel/syscall/`
  dispatch generated from `table.def` (→ docs/SYSCALLS.md).
- `kernel/drivers/` LAPIC/IOAPIC/HPET/PIT, `refclock` (TSC, else HPET, else
  PIT), PS/2, serial, RTC, framebuffer console.
- `kernel/gfx/` libgfx software renderer (anti-aliased rounded shapes).
  `kernel/gui/` the in-kernel desktop (`desktop.cpp`: compositor, windows,
  panel, menu, input; `terminal.cpp`: shell terminal with scrollback). It
  moves to userland as Pane in phase 12.
- `kernel/lib/` kprintf, console lock, kernel shell (`shell.cpp`), panic,
  CSPRNG, lock ranks (`lock_order.h` — add new locks there).
- `userland/` libc, programs in `bin/` (packed into `boot/initrd.tar`).
- `tests/kernel/` in-kernel self-tests (register in `registry.cpp`; `test
  all` runs them — update `tests/integration/shell-tests.expect` when the
  count changes). `tests/integration/` QEMU scenarios. `tests/fuzz/` host
  fuzzers.
- `spec/` the Spec language: empty until phase 16.

## Rules that never bend (SPEC §19)

Never dereference a user pointer outside the user-copy routines; never map
memory writable and executable; never log user content; never add a
dependency or a network connection the owner has not approved; treat every
value from a device, disk, file or network as hostile.
