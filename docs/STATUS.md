# Status

Updated at the end of every session. Read this first (docs/SPEC.md §18).

## Current phase

**Phase 3 — Physical memory: COMPLETE (2026-09-14).**
**Desktop preview (owner-requested, brought forward from phases 10-13):
first version done (2026-09-14).**
Next up: **Phase 4 — Virtual memory**, then the desktop grows alongside.

## How to run it

- `run-lumen.cmd` (double-click on Windows): boots the ISO in QEMU with KVM
  inside WSL2, window on the desktop via WSLg, serial log in
  `logs/qemu-serial.log`. `run-lumen.cmd uefi` boots through OVMF;
  `run-lumen.cmd build` rebuilds first. This is the fast, accurate path.
- `dist/lumen-0.0.1.iso`: snapshot ISO for any VM (`make dist` refreshes it).
- VirtualBox: the owner's VM is "Lumen 0.0.1" (COM1 → `logs/vbox-serial.log`,
  HPET on, PS/2 mouse+keyboard). **Caveat:** on this host VirtualBox runs on
  the Hyper-V backend ("AMD-V is not available" in VBox.log, because
  WSL2/Docker keep Hyper-V on), which makes the guest laggy and its clock run
  ~3x slow while idle. See docs/DECISIONS.md. "Lumen-dev" is a headless VM
  used for automated VirtualBox checks (log `logs/vbox-dev-serial.log`).

## What works

- Host environment: WSL2 Ubuntu 24.04 with the cross toolchain
  (`toolchain/out/`, binutils 2.42 + GCC 13.3.0), QEMU 8.2 with KVM, OVMF.
- Build: `make` (two-pass link embedding the symbol table), `make iso`
  (hybrid BIOS/UEFI, Limine 11.4.1), `make dist`.
- Run: `make run` / `run-uefi` / `run-headless` / `debug` / `gdb` (KVM when
  `/dev/kvm` exists; `QEMU_ACCEL=tcg` forces TCG); `make vbox` / `vbox-log`.
- Test: `make test` runs every `tests/integration/*.expect` through
  `tools/qemu-probe.py` (headless QEMU + QMP; directives `!send`, `!key`,
  `!mouse`, `!mouseto`, `!button`, `!screenshot`, `!wait`). All 8 pass:
  boot-banner, shell-tests, keyboard, pmm, desktop, exception-de/pf/ud.
- Phase 1: Limine base revision 3 → `g_boot_info`; COM1; `kprintf`; PSF2
  framebuffer console (shadow buffer, batched); boot banner.
- Phase 2: GDT/TSS, IDT + exception dumps with symbolised backtraces,
  embedded symbol table, PIC, ACPI (RSDT/XSDT, MADT), HPET, Local APIC, I/O
  APIC, self-correcting 100 Hz APIC timer, `early_map`, kernel shell, RTC.
- Phase 3: bitmap PMM (`kernel/mm/pmm.cpp`), `test pmm` (10,000 random
  chunks, shuffled frees, exhaustion, checksum-verified).
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
- Verified: QEMU/KVM `make test` including scripted drag, launcher menu and
  application start with screenshots (`build/desktop-*.png`).

## Half-done

- The compositor is pumped from the shell's idle loop (no scheduler yet):
  a long shell command (e.g. `test idle`) freezes the desktop meanwhile.
- One terminal window (one kernel shell). Closing it and reopening from the
  launcher gives a fresh grid; the shell keeps running.
- No text selection, scrollback, or escape sequences in the terminal.

## Next

1. Phase 4: 4-level paging `AddressSpace`, map/unmap/protect, VMA list,
   COW clone, demand paging, guard pages; `test vmm`.
2. Phase 5: slab heap (`kmalloc`), red zones, leak tracking; the desktop
   moves its static pools to it.
3. Phase 6: threads and the scheduler, after which the compositor gets its
   own thread and the desktop stays responsive during shell commands.
4. Desktop polish as the owner sends reference screenshots.

## Known bugs

- `make` on the Windows-mounted tree occasionally prints
  "Clock skew detected" (drvfs timestamp rounding). Harmless so far.
- Backtraces cannot name a function that faults inside its own prologue
  (inherent to RBP walking); tests avoid it by making a call before faulting.
- The About window's CPU line is clipped at the window edge on long brand
  strings (needs ellipsis on the value column).
