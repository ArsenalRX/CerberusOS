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

Released: **0.5.1** (2026-10-03, tag `v0.5.1`, `dist/lumen-0.5.1.iso`; `dist/` holds
only the newest ISO).
The tree now builds as `0.5.2-dev+<commit>`. Scheme: docs/SPEC.md §23;
history: docs/CHANGELOG.md.

---

## Current phase

**Phase 5 — Kernel heap: COMPLETE (2026-10-03).** Phase 4 (virtual memory)
completed the same day.
Phases 0–3 complete (2026-09-14). Desktop preview (owner-requested, brought
forward from phases 12-13): first version done (2026-09-14).
Next up: **Phase 6 — Threads and scheduling** (SPEC §5 and §5A). The owner approves
each phase's contents before it starts.

**Spec is version 2 (2026-10-03).** Security, privacy, networking and
performance are requirements. Phases 14+ were renumbered (networking 14,
security model 15, language backend 16, software platform 17,
installer/updates/encryption 18, stretch 19). See docs/DECISIONS.md.

## Last verified

- `make test`: all **15** integration tests passed on **2026-10-03** in
  QEMU/KVM on the code released as 0.5.0: boot-banner, desktop, keyboard,
  pmm, shell-tests, vmm, heap, exception-de/pf/ud/so/ub,
  heap-freelist, heap-write-after-free, heap-double-free.
- A `DEBUG=0` build also passes `test heap` (checked 2026-10-03).
- On the 0.4.0 code (not repeated for 0.5.0): the `vmm` test passed under
  software emulation (TCG), and the kernel booted to the shell under UEFI
  (OVMF) when QEMU was run directly.
- **`tools/qemu-probe.py --uefi` does not work:** the kernel is entered but
  blocks in `serial_putc` waiting for the UART, with both 0.3.0 and 0.4.0.
  The same ISO under the same firmware with QEMU's serial on stdout boots
  fine, so this is a probe problem, not a kernel one. Not yet investigated.
- **VirtualBox 7.2.6 (Hyper-V backend), VM "Lumen", 4 CPUs, BIOS: verified
  on 2026-10-03 with 0.5.1.** Boots to the desktop; typed commands work;
  `test vmm`, `test heap` and `test idle` pass (timer delivered to a halted
  CPU at 99.8 Hz). 0.5.0 and earlier lose the keyboard after a few keys
  there when the VM has more than one CPU (fixed in 0.5.1).
- One run of `shell-tests` on the 0.5.1 release ISO failed in `test idle`
  (tick rate measured 107–119 Hz against the HPET); three immediate re-runs
  of the same ISO passed. The host was also running VirtualBox at the time.
  Treat as timing noise under host load until it recurs.

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
- **Phase 5** (released as 0.5.0):
  - `kernel/mm/kheap.*`: slab caches for 16–2048 bytes, whole pages above
    that; `kmalloc`/`kzalloc`/`kfree`/`krealloc`/`kfree_sensitive`/`ksize`;
    encoded and validated free lists; debug red zones, poisoning, call-site
    tracking; `heapstat`.
  - The desktop's pixel buffers and terminal cells, and the VMM's VMA and
    address-space records, now come from the heap.

## How to run it

- `run-lumen.cmd` (double-click on Windows): boots the ISO in QEMU with KVM
  inside WSL2, window on the desktop via WSLg, serial log in
  `logs/qemu-serial.log`. `run-lumen.cmd uefi` boots through OVMF;
  `run-lumen.cmd build` rebuilds first. This is the fast, accurate path.
- `dist/lumen-0.5.1.iso`: the release ISO for any VM (`make RELEASE=1 dist`
  writes `dist/lumen-<version>.iso`, deletes the previous one, and re-points
  the VirtualBox VM at it).
- VirtualBox: the VM is **"Lumen"** (created 2026-10-03: Other 64-bit, BIOS,
  4 CPUs, 2 GiB, HPET and I/O APIC on, PS/2 keyboard and mouse, COM1 →
  `logs/vbox-serial.log`). Start it from the VirtualBox window. A VM made by
  hand must use OS type "Other/Unknown (64-bit)": the 32-bit "Other" type
  hides 64-bit mode and the bootloader then refuses to start the kernel.
  On this host VirtualBox runs on the Hyper-V backend ("AMD-V is not
  available" in VBox.log, because WSL2/Docker keep Hyper-V on), so it is
  slower than `run-lumen.cmd`. The earlier VMs "Lumen 0.0.1" and "Lumen-dev"
  no longer exist in VirtualBox (an unregistered "Lumen-dev" folder is left
  under `VirtualBox VMs`).

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
- Phase 5: kernel heap (`kernel/mm/kheap.cpp`); `test heap` (100,000 random
  allocate/free pairs, every buffer verified, zero leaks, run twice);
  `test exceptions fl|waf|df` show a corrupted free list, a write after
  free and a double free each stopping the kernel; `heapstat`.
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

In place since 0.5.0 (each demonstrated by an exception test):

- Heap free lists are encoded with a per-boot secret and validated; double
  frees and foreign pointers are refused; debug builds add red zones,
  poisoning and write-after-free detection. `kfree_sensitive` exists for
  secrets.

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
  `-fno-stack-protector`). The heap's free-list secret comes from RDRAND
  and the time-stamp counter until the CSPRNG exists (phase 7).
- No networking, so nothing leaves the machine.
- No persistent storage, so no user data is kept.

This is acceptable only because the system runs no untrusted code and
holds no user data today. Do not put real data in it before phase 15.

## Performance state

No benchmarks exist yet (`make bench` arrives in phase 6; budgets in
docs/SPEC.md §20.1). The only measurement so far is the first desktop frame,
which varied between 21 and 29 ms at 1280×800 across boots on 2026-10-03 —
too noisy to compare builds with. One budget has a real number: a
`kmalloc(64)`+`kfree` pair takes 74–83 ns in the debug build and 13–19 ns
with `DEBUG=0` (budget 100 ns; `test heap`, QEMU/KVM, 2026-10-03). Known
issues:

- The compositor is pumped from the shell idle loop, and the console spins
  instead of halting while the desktop is up, so an idle desktop uses a full
  CPU. Both are fixed by phase 6 (compositor thread, idle `hlt`).
- The three other CPUs spin in a `pause` loop in kernel text until phase 8
  (they spun inside the bootloader before; no change in cost).
- Debug builds carry the undefined-behaviour checks: kernel text is about
  twice the size of 0.3.0's.

## Half-done

- **Phase 6 is started, not built.** `kernel/sched/sched.h`, `sched.cpp` and
  `switch.asm` (threads, run queues, sleep list, wait queues, processes) are
  written but have never been compiled; nothing calls them yet. They are in
  the working tree, uncommitted.
- The spin-instead-of-halt idle from 0.3.0 is still in `console.cpp`; with
  the parked CPUs halted the timer does reach a halted CPU on VirtualBox
  (`test idle`, 0.5.1), so phase 6's idle thread can halt there too.
- `early_map` still hands out kernel virtual addresses on its own (first GiB
  of the vmalloc region); drivers can move to `mmap_device` when convenient.
- File-backed memory regions wait for the VFS (phase 9).
- The compositor is pumped from the shell's idle loop (no scheduler yet):
  a long shell command (e.g. `test idle`) freezes the desktop meanwhile.
- One terminal window (one kernel shell). Closing it and reopening from the
  launcher gives a fresh grid; the shell keeps running.
- No text selection, scrollback, or escape sequences in the terminal.

## Open decisions waiting on the owner

Recorded in docs/DECISIONS.md (2026-10-03). None blocks phase 6. The libc
question (2) should be answered before phase 7.

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

1. Phase 6 was approved by the owner on 2026-10-03 and is in progress (see
   "Half-done" and docs/TO_FINISH.md).
2. Phase 6: threads and the scheduler; compositor thread; idle `hlt`;
   guard pages under every kernel stack; `make bench` and docs/BENCH.md
   begin.
3. Phase 7: user mode and system calls.
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
