# Features and capabilities

**What this file is for.** The inventory of what Cerberus can do today, by
area, with how each thing is used (key, click, command). It is the list to
read before proposing or building anything new, so that ideas build on
what exists instead of duplicating it, and so that nothing already built is
forgotten. docs/STATUS.md says whether each is verified; this file says what
there is and how to reach it.

**When to update it.** In the same commit as any change that adds, removes
or renames something a user can see or use.

**How to read it.** Each line is a capability and the way to use it. An
item in *italics* is a limit worth knowing.

Last updated: **2026-10-05** (0.0.5h).

---

## Boot and hardware

- Boots from the hybrid ISO on BIOS and UEFI (Limine); straight into the
  kernel, no menu wait. *Only in VMs so far (QEMU, VirtualBox).*
- All CPUs started (tested with 4); per-CPU scheduling with work stealing.
- Memory: frame allocator, address spaces, demand paging, copy-on-write
  fork, guard pages, kernel heap with corruption checks.
- Timers: APIC timer at 100 Hz; reference clock from the TSC, HPET or PIT.
- ACPI power off and restart (`poweroff`, `reboot`, launcher buttons).
- PCI with MSI; disks: AHCI (DMA, interrupt-driven), NVMe, virtio-blk,
  IDE (PIO). PS/2 keyboard (set 2, full map, Num Lock, repeat) and mouse
  (wheel). Framebuffer; resolution switching on the Bochs/VirtualBox/QEMU
  display adapter (`bga`), 640x480 up to 5120x1440 (32:9 ultrawide).
- VirtualBox guest device (`vmmdev`): absolute mouse position, so the
  pointer matches the host's and the mouse need not be captured.
- `/dev/input/kbd0`, `/dev/input/mouse0`, `/dev/fb0` for user programs
  (root only).

## Files

- VFS: one tree, mounts, `.`/`..`, symbolic links, permissions, mount
  flags (`ro`, `noexec`, `nodev`, `nosuid`), name cache.
- tmpfs; the boot archive as a read-only `/`; `/dev`; sticky `/tmp`.
- **cerfs**: journaled, CRC32C-checksummed on-disk file system; survives
  power cuts; `mkfs.cerfs` on the host (`--check`) and inside the OS.
  *No `fsck` yet; no hard links.*
- Page cache with read-ahead and write-back.
- **/data**: the first disk with a cerfs volume labelled `data` is mounted
  there at boot (`mkfs.cerfs -L data`, or Settings → Storage). Settings,
  reminders and Notes live there.
- Programs: `ls cat echo mkdir rm rmdir mv cp touch stat ln sync mount
  umount pwd write fstest mkfs.cerfs`.
- Shell redirection `>` and `>>`.

## Programs and IPC

- Ring-3 processes from position-independent ELF files; ASLR; `fork`,
  `execve`, `waitpid`; 74 system calls (docs/SYSCALLS.md); a small libc.
- Threads inside a program (`pthread_create`/`join`, mutexes, condition
  variables, `__thread`), futexes (also across processes).
- Named **ports** (messages up to 64 KiB, carry open files and shared
  memory, permission-checked, sender identity from the kernel), shared
  memory, signals with handlers, event queues (`event_wait` over ports,
  timers, input devices, child exit).
- Private file `mmap`, `mprotect`, `kill`, `getrandom`, `sysinfo`.
- `runas <uid> <program>`; a process can drop root for good.

## Kernel shell (the Terminal window, also COM1)

- Commands grouped by `help`; `help <command>`, `help tests`.
- Line editing: Backspace, Ctrl-U; **history** with Up/Down (Ctrl-P/N on
  serial); **Tab completion** of command names.
- `run <path>` or just a program's name; `cd`, `pwd`, `mount`, `ls`…
- Diagnostics: `ps`, `heapstat`, `irqs`, `pci`, `drivers`, `bench`,
  `test <name|all>`, `sym <addr>`, `gui`, `ticks`, `idle`, `timermode`.
- `resolution [<w> <h>]`, `notify <text>` (a desktop notification).
- Scrollback of 1,000 lines: wheel, Shift+PgUp/PgDn, scrollbar.

## Desktop (kernel-hosted preview of Pane)

### Windows
- Move by the title bar; resize at any edge or corner (cursor changes);
  minimise / maximise / close buttons (Windows style); stacking; focus
  follows clicks.
- **Double-click** a title bar: maximise/restore. **Drag** a maximised or
  snapped window: it takes its old size under the pointer.
- **Snap**: drop a window at the left or right screen edge for half the
  screen, at the top to maximise (an outline previews it);
  Super+Left/Right/Up; Super+Down restores, then minimises.
- Alt+F4 closes; **Alt+Tab** opens a switcher (tiles; Tab moves on,
  Shift+Tab back, release Alt to pick, Escape cancels).
- **Super+D** hides every window and brings them back in the same order.
- Open/close/minimise/restore **animations** (fade + slide, 160 ms).
- Windows: Terminal (one, the kernel shell), Files, Notes, Calculator,
  About, System Monitor, Memory Map, Settings. *One window per kind.*
- **Lock screen**: Super+L (password from Settings → Security); the clock
  and date on the wallpaper; Enter unlocks.

### Clipboard
- Drag over the terminal to select text (it is copied at once); Ctrl+C /
  Ctrl+Shift+C copy; Ctrl+V / Ctrl+Shift+V paste into the terminal, the
  launcher search, the calendar line, Notes and the Calculator.

### Applications (built in, one window each)
- **Files**: Up, Home (/data or /), the path, New folder, Rename (F2),
  Delete (twice), double-click or Enter to open a folder or a text file
  (in Notes), Backspace to go up, wheel/arrows, F5 refresh.
- **Notes**: typing, arrows, Home/End, PgUp/PgDn, Shift+arrows select,
  mouse click and drag, wheel, Ctrl+A/C/X/V, Ctrl+Z undo (8 steps),
  Ctrl+F find (Enter next, Esc), Ctrl+S save, Ctrl+N new. Files up to
  64 KiB; a new note saves to /data/notes.txt or /tmp/notes.txt.
- **Calculator**: keys or clicks; `+ - * / % ^ ( )`, decimals; Enter or
  `=`; Backspace; Esc clears; result also as hex and binary; M+/MR; copy.

### Taskbar
- Floating rounded bar (or docked, edge to edge); glass or solid.
- Logo button → launcher; search box → launcher with the keyboard in it.
- A button per window with its app icon; click to focus/minimise;
  **middle-click closes**; the focused one is marked.
- Memory meter; clock with date (12/24 h, seconds on/off, time zone).
- **Clock click → calendar**. **Tray chevron → quick settings**: night
  light, show desktop, lock, Settings, accent, frame rate, CPU per core,
  RAM, notification history (Clear).
- CAPS and "NUM off" pills when those locks are on/off.

### Launcher
- Super (released alone) or the logo. Type to filter; Enter opens the
  first match; Up/Down choose; Escape closes. Power off and Restart.
- Right-click the desktop: Terminal, Settings, Change wallpaper, Show
  desktop, About.

### Calendar and reminders
- Month view; page with the arrows, the wheel, Left/Right; "Today".
- Click a day; type `14:30 Text` (or just `Text` for 09:00) + Enter to
  add a reminder; × removes it; days with reminders show a dot.
- A reminder fires as a notification at its minute. *Kept in memory only
  (B-011).*

### Notifications
- Cards top-right, fade in, gone after 8 s or when clicked. From the shell:
  `notify <text>`. Used by reminders and Print Screen.

### Print Screen
- Saves the screen as `/tmp/screenshot-N.bmp` and says so.

### Settings (launcher → Settings, or right-click → Settings)
- **Appearance**: accent colour (9 swatches or any hue from the strip,
  live), window corners (square/soft/round), taskbar floating/docked and
  glass/solid, clock 12/24 h and seconds, time zone (UTC±, half hours).
- **Wallpaper**: Nebula, Aurora, Ember, Ocean, Sunset, Graphite.
- **Window borders**: effect (static, breathing, flashing, rainbow,
  chase), colour, thickness 1–4 px, speed, focused/all windows, glow.
- **Display**: resolution (18 common sizes, 4:3 to 32:9, filtered by what
  the adapter can hold), compositor frame rate 30–144 Hz.
- **Keyboard**: layout US / UK / German / French (AltGr for the third
  characters), repeat delay 250 ms–1 s, repeat rate 10/20/30 per second,
  the lock keys' state.
- **Storage**: every disk with its size and state; "Use for settings"
  formats a blank disk as the `data` volume (asks twice).
- **Security**: set, change or remove the lock-screen password; Lock now.
- Everything is saved to `/data/desktop.conf` when a data disk exists.

## Security and privacy (in place; see docs/STATUS.md for the full list)

- W^X everywhere, SMEP/SMAP/UMIP, stack protectors, zeroed memory, heap
  free-list encoding, guard pages, user-copy routines only, ASLR, CSPRNG.
- File permissions and mount flags enforced; ports permission-checked;
  signals only within a user; device nodes root-only.
- Fuzzers: ELF loader, tar reader, cerfs images, random system calls.
- Lock-screen password: salted SHA-256, constant-time compare, one second
  between tries (`test sha256` checks the hash). *A reboot bypasses the
  lock until phase 15: the root file system is open to anyone at the
  machine.*
- No networking: nothing leaves the machine. *No user accounts yet; every
  program runs as root (phase 15).*

## Tooling (host side)

- `make`, `make iso`, `make test` (49 integration tests in QEMU/KVM),
  `make bench`, `make fuzz`, `make dist` (also fixes VirtualBox VMs).
- `tools/qemu-probe.py`: boots the ISO headless and drives it:
  `!key` (text + Enter, including shifted punctuation), `!keys` (key
  combinations: `alt tab`, `meta_l d`, `print`), `!mouseto`, `!mouse`,
  `!button`, `!dblclick`, `!wheel`, `!screenshot`, `!wait`, `!kill`.
- `tools/run-tests.sh <names>`; `tools/vbox-attach.sh`.
- `run-cerberus.cmd` boots the ISO in QEMU with KVM from Windows.

## Not there yet (so nobody looks for it)

Networking, user accounts and login, sound, USB, printing, real hardware,
an installer, the Spec compiler, images or any file type but text in the
built-in apps.
