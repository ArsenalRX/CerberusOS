# Status

**What this file is for.** It describes what is true about the project right
now. It is the first file every session reads (docs/SPEC.md §18).

**When to update it.** At the end of every session, without exception, and
immediately whenever something listed under "What works" stops working.

**How to update it.** Rewrite the sections in place so the file always
describes the present; do not append a history (history lives in git and in
docs/DECISIONS.md). Use absolute dates. Under "What works", only list things
that were verified with real output, and keep the "Last verified" line
honest: if this session did not run `make test`, say so. Move anything
started but not finished to "Half-done". Keep "Next" matching
docs/TO_FINISH.md.

**Why.** A session has no memory of the previous one. If this file claims
something works that does not, the next session builds on a broken base; if
it omits half-done work, that work is lost or done twice.

Last updated: **2026-10-03**.

## Version

Released: **0.4.0** (2026-10-03, tag `v0.4.0`, `dist/lumen-0.4.0.iso`).
The tree now builds as `0.4.1-dev+<commit>`. Scheme: docs/SPEC.md §23;
history: docs/CHANGELOG.md.

---

## Current phase

**Phase 4 — Virtual memory: COMPLETE (2026-10-03).**
Phases 0–3 complete (2026-09-14). Desktop preview (owner-requested, brought
forward from phases 12-13): first version done (2026-09-14).
Next up: **Phase 5 — Kernel heap** (SPEC §5 and §5A). The owner approves
each phase's contents before it starts.

**Spec is version 2 (2026-10-03).** Security, privacy, networking and
performance are requirements. Phases 14+ were renumbered (networking 14,
security model 15, language backend 16, software platform 17,
installer/updates/encryption 18, stretch 19). See docs/DECISIONS.md.

## Last verified

- `make test`: all **11** integration tests passed on **2026-10-03** in
  QEMU/KVM on the code released as 0.4.0: boot-banner, desktop, keyboard,
  pmm, shell-tests, vmm, exception-de/pf/ud/so/ub.
- The `vmm` test also passed under software emulation (TCG), and the kernel
  boots to the shell under UEFI (OVMF) when QEMU is run directly.
- **`tools/qemu-probe.py --uefi` does not work:** the kernel is entered but
  blocks in `serial_putc` waiting for the UART, with both 0.3.0 and 0.4.0.
  The same ISO under the same firmware with QEMU's serial on stdout boots
  fine, so this is a probe problem, not a kernel one. Not yet investigated.
- **VirtualBox: not re-verified on 2026-10-03.** The "Lumen-dev" VM is in a
  saved state; booting the new ISO would discard it, which needs the
  owner's permission. Last VirtualBox observations are from 2026-09-14.

## What changed on 2026-10-03

- The work left uncommitted on 2026-09-14 was verified and committed
  (shared PS/2 drain routine, spin-idle under the desktop, `gui`/`irqs`
  commands); the periodic `diag:` serial line was removed because it broke
  two tests. Released as 0.3.0 with the version scheme and spec v2.
- **Phase 4** (released as 0.4.0):
  - `kernel/mm/vmm.*`: `AddressSpace` over 4-level page tables (map, unmap,
    protect, translate), VMAs (anonymous, device, guard), `mmap`/`munmap`/
    `mprotect`, demand paging, copy-on-write `clone`, 2 MiB kernel leaves,
    guarded kernel stacks, page-fault handler.
  - Boot-time hardening: NX on everything in the kernel half outside kernel
    text, kernel image mapped by section, lower half emptied, WP and NXE on.
  - `kernel/mm/probe.*`: fault-safe read/write/execute probes.
  - `boot_park_aps()`: the other CPUs now wait in kernel text.
  - `kernel/lib/result.h`: `Result<T>` and `Error` (SPEC §6.3).
  - `kernel/lib/ubsan.cpp` and build flags: zero-initialised locals always,
    a subset of the undefined-behaviour sanitizer in debug builds.

## How to run it

- `run-lumen.cmd` (double-click on Windows): boots the ISO in QEMU with KVM
  inside WSL2, window on the desktop via WSLg, serial log in
  `logs/qemu-serial.log`. `run-lumen.cmd uefi` boots through OVMF;
  `run-lumen.cmd build` rebuilds first. This is the fast, accurate path.
- `dist/lumen-0.4.0.iso`: the release ISO for any VM (`make RELEASE=1 dist`
  writes `dist/lumen-<version>.iso`; the older `lumen-0.0.1.iso` snapshot is
  still there).
- VirtualBox: the owner's VM is "Lumen 0.0.1" (COM1 → `logs/vbox-serial.log`,
  HPET on, PS/2 mouse+keyboard). **Caveat:** on this host VirtualBox runs on
  the Hyper-V backend ("AMD-V is not available" in VBox.log, because
  WSL2/Docker keep Hyper-V on), which makes the guest laggy and its clock run
  ~3x slow while idle. See docs/DECISIONS.md. "Lumen-dev" is a headless VM
  used for automated VirtualBox checks (log `logs/vbox-dev-serial.log`).

## What works (verified 2026-10-03 by `make test` in QEMU/KVM)

- Host environment: WSL2 Ubuntu 24.04 with the cross toolchain
  (`toolchain/out/`, binutils 2.42 + GCC 13.3.0), QEMU 8.2 with KVM, OVMF.
- Build: `make` (two-pass link embedding the symbol table), `make iso`
  (hybrid BIOS/UEFI, Limine 11.4.1), `make dist`.
- Run: `make run` / `run-uefi` / `run-headless` / `debug` / `gdb` (KVM when
  `/dev/kvm` exists; `QEMU_ACCEL=tcg` forces TCG); `make vbox` / `vbox-log`.
- Test: `make test` runs every `tests/integration/*.expect` through
  `tools/qemu-probe.py` (headless QEMU + QMP; directives `!send`, `!key`,
  `!mouse`, `!mouseto`, `!button`, `!screenshot`, `!wait`). 8 tests:
  boot-banner, shell-tests, keyboard, pmm, desktop, exception-de/pf/ud.
- Phase 1: Limine base revision 3 → `g_boot_info`; COM1; `kprintf`; PSF2
  framebuffer console (shadow buffer, batched); boot banner.
- Phase 2: GDT/TSS, IDT + exception dumps with symbolised backtraces,
  embedded symbol table, PIC, ACPI (RSDT/XSDT, MADT), HPET, Local APIC, I/O
  APIC, self-correcting 100 Hz APIC timer, `early_map`, kernel shell, RTC.
- Phase 3: bitmap PMM (`kernel/mm/pmm.cpp`), `test pmm` (10,000 random
  chunks, shuffled frees, exhaustion, checksum-verified).
- Phase 4: VMM (`kernel/mm/vmm.cpp`): address spaces, VMAs, demand paging,
  copy-on-write clone, guard pages, 2 MiB kernel leaves; kernel half
  hardened at boot; `test vmm` passes twice in a row with no frame leaked;
  `test exceptions so` reports a stack overflow through the guard page;
  `test exceptions ub` shows the sanitizer stopping on signed overflow.
- Desktop preview (`kernel/gui/`, `kernel/gfx/`):
  - libgfx: fills, rounded rects, alpha blits, scaled blits, lines, circles,
    linear/radial gradients, box blur, PSF text with ellipsis, clip stack.
  - Compositor: wallpaper, back buffer, damage rectangles, windows with
    rounded corners, blurred cached shadows, title bars with
    close/maximise/minimise, focus styling, move and resize by mouse
    (8 edges), stacking, minimise/restore, maximise; software cursor
    restored from the back buffer; ~60 Hz frame pacing.
  - Panel: launcher button + menu (Terminal, System Monitor, Memory Map,
    About), task buttons, RAM %, clock and date from the RTC.
  - Windows: Terminal (the kernel shell, keyboard input when focused),
    About, System Monitor (1 Hz refresh), Memory Map.
  - Hotkeys: Alt+Tab, Alt+F4, Super (menu, arrows+Enter), Super+T, Super+M.
  - Early PS/2 mouse (IRQ 12, wheel detection) and keyboard key events
    with modifiers (`ps2kbd_poll_event`).
  - Panics and exceptions switch back to the text console so the dump is
    visible.

## Security and privacy state (honest summary)

In place since 0.4.0 (each demonstrated by `test vmm` or an exception test):

- W^X: no mapping can be writable and executable.
- Everything in the kernel half outside kernel text is non-executable;
  kernel text and read-only data are not writable; CR0.WP is on.
- Anonymous memory is zeroed before it is mapped; the first 64 KiB of a
  user address space cannot be mapped; the lower half of the kernel's own
  address space is empty.
- Guard pages under stacks allocated by the VMM.
- Zero-initialised stack variables; undefined-behaviour checks in debug
  builds.

Still missing, because the mechanisms they attach to do not exist yet:

- Everything runs in ring 0, including the desktop and the shell. There is
  no user mode, no process isolation, no users, no permissions.
- The bootstrap stack and the IST stacks have no guard page (thread stacks
  get one in phase 6).
- SMEP/SMAP/UMIP are not enabled (phase 7, with user mode).
- No randomness source, no ASLR, no stack canaries (kernel is built
  `-fno-stack-protector`).
- No networking, so nothing leaves the machine.
- No persistent storage, so no user data is kept.

This is acceptable only because the system runs no untrusted code and
holds no user data today. Do not put real data in it before phase 15.

## Performance state

No benchmarks exist yet (`make bench` arrives in phase 6; budgets in
docs/SPEC.md §20.1). The only measurement so far is the first desktop frame,
which varied between 21 and 29 ms at 1280×800 across boots on 2026-10-03 —
too noisy to compare builds with. Known issues:

- The compositor is pumped from the shell idle loop, and the console spins
  instead of halting while the desktop is up, so an idle desktop uses a full
  CPU. Both are fixed by phase 6 (compositor thread, idle `hlt`).
- The three other CPUs spin in a `pause` loop in kernel text until phase 8
  (they spun inside the bootloader before; no change in cost).
- Debug builds carry the undefined-behaviour checks: kernel text is about
  twice the size of 0.3.0's.

## Half-done

- VirtualBox re-verification of the spin-idle change (see "Last verified").
- `early_map` still hands out kernel virtual addresses on its own (first GiB
  of the vmalloc region); drivers can move to `mmap_device` when convenient.
- File-backed memory regions wait for the VFS (phase 9).
- The compositor is pumped from the shell's idle loop (no scheduler yet):
  a long shell command (e.g. `test idle`) freezes the desktop meanwhile.
- One terminal window (one kernel shell). Closing it and reopening from the
  launcher gives a fresh grid; the shell keeps running.
- No text selection, scrollback, or escape sequences in the terminal.

## Open decisions waiting on the owner

Recorded in docs/DECISIONS.md (2026-10-03). None blocks phases 5–13.

1. TLS and cryptography source: port Mbed TLS (recommended), port BearSSL,
   or write in-tree. Needed before phase 15.
2. libc: grow the in-tree libc or port mlibc. Needed before phase 17;
   deciding before phase 7 avoids writing a libc twice.
3. TCP/IP: write in-tree (current plan) or port lwIP. Needed before
   phase 14.
4. Confirm the v2 phase order and the non-goals list.
5. Still open from 2026-09-14: confirm the scope of the network toolbox
   (docs/TO_FINISH.md, "Owner-added requirements").

## Next

1. Get the owner's go-ahead for phase 5 (the owner approves each phase's
   contents before it starts), and permission to reset the "Lumen-dev" VM.
2. Phase 5: slab heap (`kmalloc`), red zones, leak tracking, encoded
   free-list pointers, `kfree_sensitive`; the desktop moves its static
   pools to it; the VMM's private pools move onto it.
3. Phase 6: threads and the scheduler; compositor thread; idle `hlt`;
   `make bench` and docs/BENCH.md begin.
4. Desktop polish as the owner sends reference screenshots.

## Known bugs

- `make` on the Windows-mounted tree occasionally prints
  "Clock skew detected" (drvfs timestamp rounding). Harmless so far.
- Backtraces cannot name a function that faults inside its own prologue
  (inherent to RBP walking); tests avoid it by making a call before faulting.
- `tools/qemu-probe.py --uefi` hangs the guest in `serial_putc` (see "Last
  verified"); UEFI boots fine with QEMU run directly.
- The About window's CPU line is clipped at the window edge on long brand
  strings (needs ellipsis on the value column).
