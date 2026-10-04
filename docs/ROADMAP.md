# Cerberus Roadmap

**What this file is for.** One readable overview for the owner: what Cerberus
can do today, and everything still to add, implement, fix and speed up,
in the order it will be built. It summarises docs/SPEC.md (the full plan,
which wins if the two disagree), docs/TO_FINISH.md (the working checklist)
and docs/STATUS.md (what is verified).

**When to update it.** At every release, and whenever a phase is finished,
re-ordered or re-scoped, or the owner adds a request.

**How to update it.** Rewrite "What Cerberus can do today" so it stays true
and is written for a person, not a programmer; tick items and move finished
phases into it. Keep phase numbers identical to docs/SPEC.md §5.

Last updated: **2026-10-04**, at release **0.0.5e** (phase 10 complete).

---

## What Cerberus can do today (0.0.5e)

Cerberus is a 64-bit operating system written from scratch. It boots in
QEMU and VirtualBox from an ISO (`dist/cerberus.iso`), with old BIOS or UEFI.

**Starting up**
- Boots through the Limine boot menu into its own kernel in about a
  second; prints a boot log on screen and on the serial port.
- Finds the processor, memory, interrupt controllers, timers and the
  real-time clock on its own.
- Uses **every processor core** (tested with 4).

**Memory**
- Manages all of the machine's RAM: hands out and takes back memory pages,
  gives every program its own private memory space, and loads pages only
  when they are first touched.
- When a program is copied (`fork`), the copy shares memory until one side
  writes ("copy-on-write"), which keeps it cheap.
- A kernel memory allocator with a separate pool per core.

**Running programs**
- Runs many things at once with a preemptive scheduler: four priority
  levels, time slices, sleeping, waiting, and idle cores taking work from
  busy ones.
- Runs real user programs in a protected mode (ring 3). A crashing program
  is stopped and reported; the system keeps going.
- 14 system calls (open/read/write/close files, memory mapping, fork,
  execve, wait, exit, random numbers and so on) and a small C library for
  programs.
- Programs included: `hello`, `args`, `forktest`, `badptr`, `crash`,
  `aslr`, `sysbench`, `sysfuzz`, plus `init` (the first process).

**Desktop (a preview of the real one)**
- A graphical desktop with a wallpaper, a taskbar (launcher button, open
  windows, memory use, clock and date) and a launcher menu.
- Windows with rounded corners, shadows and title-bar buttons; they can be
  moved, resized, stacked, minimised, maximised and closed with the mouse.
- Four windows: Terminal (the kernel shell), System Monitor, Memory Map,
  About.
- Keyboard shortcuts: Alt+Tab, Alt+F4, Super (menu), Super+T (terminal),
  Super+M (maximise).
- PS/2 keyboard (every key, keypad, repeat) and mouse (with wheel).
- Smooth, anti-aliased text everywhere.
- `poweroff` and `reboot` work.

**Files and disks**
- Real files and folders: `/` (read-only, from the boot archive), `/tmp`
  (in memory), `/dev` (devices), and disks mounted wherever you like.
- **cerfs**, Cerberus's own disk format, with a journal: pulling the plug
  while it writes never leaves the disk broken. Format a disk with
  `mkfs.cerfs -y /dev/disk/sda`, then `mount -t cerfs /dev/disk/sda /mnt`.
- SATA, NVMe, virtio and older IDE disks.
- File permissions, read-only and no-programs mounts, and a disk in use
  can't be overwritten.
- A page cache and a name cache make repeated reads come from memory.
- Programs: `ls`, `cat`, `echo`, `mkdir`, `rm`, `rmdir`, `mv`, `cp`,
  `touch`, `stat`, `ln -s`, `sync`, `mount`, `umount`, `pwd`, `write`,
  `mkfs.cerfs`, `fstest`.

**Built-in shell (in the Terminal window)**
- Type a program's name to run it (`ls -l /bin`); `> file` and `>> file`
  send its output to a file; `cd` and `pwd`; `mount` lists file systems.
- `help`, `ticks`, `mem`, `ps`, `heapstat`, `irqs`, `gui`, `pci`, `bench`,
  `run <program>`, `test <name>`, `sym`, `idle`, `timermode`, `panic`,
  `halt`, `reboot`.
- `test all` runs the kernel's own self-tests (memory, scheduler,
  multi-core, timers and more).

**Security already in place**
- Programs can't touch kernel memory or each other's memory. Every
  system-call argument is checked.
- No memory is both writable and executable. Memory is wiped before reuse.
- Randomised program layout (ASLR), stack-overflow guards, stack-smashing
  detection, hardened memory allocator, hardware protections (SMEP, SMAP,
  UMIP) where the CPU offers them.
- A cryptographic random number generator (ChaCha20).
- Automatic fuzz testing of the program loader, archive reader, system
  calls and the disk file system (thousands of damaged disks a minute).

**What it cannot do yet:** connect to a network or the internet, play sound, use USB devices, have user accounts or
passwords, install to a hard drive, or run ordinary Linux/Windows software.
The desktop still runs inside the kernel and has only a few built-in
windows. The Spec programming language has not been started.

---

## How the work is ordered

- Phases are built in order; each ends with a bootable system and an
  acceptance test. The owner approves each phase's contents before it
  starts.
- Every release moves one letter: 0.0.5a → 0.0.5b → … → 0.0.5j → 0.0.6a.
- Every phase must also meet its security rules (SPEC §19) and speed
  budgets (SPEC §20). Each line below is a deliverable unless marked
  *(fix)* or *(speed)*.

---

## Done in 0.0.5b: desktop polish

- [x] Windows-style caption buttons (— □ ✕, close turns red on hover).
- [x] Larger, anti-aliased rounded corners on windows, buttons, menu.
- [x] Terminal scrollback: 1,000 lines, mouse wheel, Shift+Page Up/Down,
  scrollbar; resizing keeps the text.
- [x] Only changed pixels are written to the screen (about 5× fewer while
  dragging); frames drawn as soon as input arrives.
- [x] Super opens the menu only on its own; the open menu takes all keys;
  About window fixes.
- [ ] Still to do: open/close animations; frame pacing from a one-shot
  timer; send input interrupts to a core that is not drawing.

---

## Done in 0.0.5c: Phase 9 — Files and file systems

- [x] Virtual file system: mounts, `.`/`..`, symbolic links (loop limit 40),
  permissions on every operation, mount flags (`ro`, `noexec`, `nodev`,
  `nosuid`), `openat`/`O_NOFOLLOW`/`O_CLOEXEC`, per-process working
  directory, `dup`/`dup2`/close-on-exec.
- [x] tmpfs (`/tmp`), the boot archive as a read-only `/`, devfs (`/dev`:
  `null`, `zero`, `random`, `urandom`, `console`, `tty0`, `fb0`, `input/*`,
  `disk/*`).
- [x] **cerfs** with a write-ahead journal and CRC32C metadata checksums;
  `mkfs.cerfs` on the build machine (with an independent checker) and inside
  Cerberus.
- [x] One page cache with read-ahead and write-back; a name cache; closed
  files stay cached.
- [x] Disk drivers brought forward from phase 10: AHCI (SATA, DMA) and IDE.
- [x] Proven: 5-level tree and 4 MiB file survive a remount byte for byte;
  power cut mid-write leaves a consistent disk; a non-root user can't read a
  private file; `noexec` stops programs; a 64 MiB file's second read comes
  from the cache (6 ms); thousands of damaged disk images a minute never
  crash the kernel.
- [ ] Moved to phase 11: memory-mapping files (`mmap` of a file).

---

## Done in 0.0.5e: Phase 10 — Drivers

- [x] Driver model with a registry (`drivers`); PCI capabilities and
  message interrupts (MSI).
- [x] Disks: SATA (AHCI, by interrupt where the controller offers MSI),
  NVMe, virtio, IDE.
- [x] Full keyboard driver (native scancode set 2, whole key map, keypad
  and Num Lock, repeat) and mouse, both readable from `/dev/input`.
- [x] `/dev/fb0`; screen, input and disk devices closed to non-root
  programs; sticky `/tmp`.
- [x] `poweroff` and `reboot` through ACPI.
- [ ] Left for later: MSI-X (NVMe and virtio disks are polled), keyboard
  LEDs, other keyboard layouts.

---

## Phase 11 — Programs talking to each other (IPC)

**Add**
- [ ] **Ports**: named message channels (create, connect, send, receive),
  messages up to 64 KiB that can carry open files and shared memory.
- [ ] **Shared memory** that can be handed to another program.
- [ ] **Signals**: kill, terminate, segmentation fault, child exited, user
  signals, with handlers in programs.
- [ ] **Futex**: fast locks for programs without a system call when
  uncontended.
- [ ] **Threads in programs**: create/join, thread-local storage, a
  pthread-style API in the C library.
- [ ] **Event waiting** over files, ports, timers and child processes
  (`event_create/ctl/wait`), so servers sleep instead of polling.

**Security**
- [ ] Who may connect to a port is checked; the receiver learns the
  sender's identity from the kernel, so it can't be faked.
- [ ] Message sizes and queue lengths are limited; a full queue blocks or
  says "try again", never grows without limit.

**Done when:** two programs exchange 100,000 messages in order with none
lost; shared memory is visible to both; a futex lock keeps an exact count
across 4 processes; an idle server uses 0% CPU and wakes within 1 ms.

---

## Phase 12 — Pane: the real window server

The desktop moves out of the kernel into its own protected program.

**Add**
- [ ] Pane, a user program that owns the screen: double-buffered,
  redraws only what changed.
- [ ] Window management: create, close, move, resize, stack, focus,
  minimise, maximise, full screen.
- [ ] Each window draws into its own shared-memory buffer and reports what
  changed (zero-copy).
- [ ] Transparency, per-window opacity, shadows, rounded corners.
- [ ] Keyboard goes to the focused window, mouse to the window under it,
  with drag grabs. Software cursor drawn last.
- [ ] `libpane`, the library programs use to make windows.
- [ ] The kernel's current desktop is the model to port; the desktop
  polish above carries over.

**Security**
- [ ] A program sees only its own windows and its own input; it can't fake
  input, move others' windows, or read their titles or contents.
- [ ] Screenshots, screen recording, global shortcuts and clipboard history
  need the user's permission per application (denied by default).
- [ ] Clipboard goes only to the focused window on an explicit paste.
- [ ] Secure password entry: while a password box has focus, nothing else
  receives keys and screen capture is refused.
- [ ] Every message is checked; a bad one disconnects the client, never
  crashes Pane. Fuzzed.

**Speed**
- [ ] *(speed)* A full frame in under 8 ms at 1920×1080 with 10 windows; no
  work at all when nothing changed.

**Done when:** three windows drag, resize, stack and focus with no tearing
or flicker; a 10×10 repaint recomposites only that area; a misbehaving test
client is refused and logged; 60 s of protocol fuzzing leaves Pane running.

---

## Phase 13 — Toolkit, desktop shell and applications

**Add**
- [ ] **Facet**, the widget toolkit: layout containers, buttons, text
  boxes, lists, menus, scroll bars, tabs, dialogs, theming, keyboard
  navigation and accessibility basics (SPEC §10).
- [ ] The desktop shell: taskbar, launcher, notifications, wallpaper,
  settings for theme and layout (SPEC §11).
- [ ] Applications (SPEC §12):
  - [ ] **Ember** — terminal (selection, scrollback, colours and escape
    sequences, tabs).
  - [ ] **Crate** — file manager.
  - [ ] **Slate** — text editor.
  - [ ] **Gauge** — system monitor.
  - [ ] **Dial** — settings.
  - [ ] **Tally** — calculator.
  - [ ] **Frame** — image viewer (PNG, BMP, TGA).
  - [ ] **Daub** — paint program.
  - [ ] **Chronos** — clock.
  - [ ] **Warren** — a small game.
  - [ ] **lsh** — the command shell, plus core command-line tools (`ls`,
    `cat`, `cp`, `mv`, `rm`, `mkdir`, and so on).
- [ ] TrueType fonts.

**Security**
- [ ] Each application declares the permissions it needs.
- [ ] File-open dialogs run in the trusted shell and hand the app only the
  file the user picked.
- [ ] Every file-format reader (PNG, BMP, TGA, TTF, tar, `.desktop`,
  `.conf`) is bounds-checked and fuzzed.

**Speed**
- [ ] *(speed)* Power-on to a usable desktop in under 3 s (QEMU/KVM).

**Done when:** boot to the desktop, open a text file from the file
manager, edit and save it in the editor, reopen it and see the change, run
commands in the terminal, all with mouse and keyboard.

---

## Phase 14 — Networking and internet

**Add**
- [ ] Network card drivers: **virtio-net** (QEMU) and **e1000** (VirtualBox
  and QEMU).
- [ ] `netd`, a user-mode network server: Ethernet, ARP, IPv4 (with
  fragment reassembly), ICMP (ping), UDP, TCP (full state machine,
  retransmission, congestion control, window scaling), loopback.
- [ ] DHCP (get an address automatically) and DNS (look up names).
- [ ] Socket system calls (`socket`, `bind`, `connect`, `listen`, `accept`,
  `send`, `recv`, …) usable with event waiting and non-blocking mode.
- [ ] Tools: `ping`, `ifconfig`, `nslookup`, `fetch` (download a web
  page), `netstat`.
- [ ] Network page in Settings; connection indicator on the taskbar.

**Security**
- [ ] Firewall: incoming connections blocked by default; rules in
  `/etc/cerberus/firewall.conf`.
- [ ] Per-application network permission.
- [ ] Random TCP sequence numbers, ports and DNS query IDs; bounded caches.
- [ ] Every packet parser fuzzed. Nothing is sent at boot except DHCP.

**Done when:** DHCP gets an address; ping, name lookup and fetching
`http://example.com/` work; a 100 MB transfer matches by checksum; a
blocked port drops and logs; a program without permission can't open a
socket.

**Owner decision first:** write the TCP/IP stack ourselves or port lwIP.

---

## Phase 15 — Users, passwords, sandboxing and secure connections

**Add**
- [ ] User accounts and groups; passwords stored as Argon2id hashes.
- [ ] Login screen and lock screen (Super+L, idle timeout); no auto-login
  by default.
- [ ] Process identities (user and group IDs). No setuid programs: admin
  actions go through `authd`, which asks for the password and logs the
  action.
- [ ] `restrict`: a program can permanently give up network, files outside
  given folders, starting programs, or raw devices. The desktop applies
  each app's declared permissions with it.
- [ ] Permission prompts (screen capture, clipboard history, network),
  remembered per app.
- [ ] Keyring (password manager) for the user's own secrets, encrypted with
  a key from the login password.
- [ ] TLS (HTTPS) for `fetch` and the package manager, with a certificate
  store.
- [ ] Audit log of logins, failures, privilege use and permission
  decisions.

**Done when:** one user can't read another's files; wrong passwords are
refused with growing delays; Argon2id matches the official test vectors; a
sandboxed program is refused network, outside files and `fork`; killing the
lock screen doesn't unlock it; `fetch https://example.com/` works and a bad
certificate is refused.

**Owner decision first:** TLS/crypto library: port Mbed TLS (recommended),
port BearSSL, or write our own.

---

## Phase 15B — Our own web browser

Decided by the owner on 2026-10-04. Needs phases 13–15 (toolkit,
networking, HTTPS and the sandbox). Name still to choose.

**Add**
- [ ] HTTP/1.1 client (keep-alive, redirects, chunked, gzip) and HTTPS.
- [ ] Cache and cookies (per site, per user, clearable; third-party
  cookies off by default).
- [ ] HTML parser following the web standard for common pages, tolerant of
  broken markup; a document tree (DOM).
- [ ] CSS for a documented subset: selectors, box model, colours, fonts,
  borders, backgrounds, block/inline/flex basics, positioning.
- [ ] Layout and painting: text wrapping, images (PNG, JPEG, GIF), basic
  tables, smooth scrolling.
- [ ] Browser window: address bar, back/forward/reload, tabs, bookmarks,
  history, downloads, find in page, zoom.
- [ ] Later (separate decision): JavaScript — write an engine or port
  QuickJS.

**Security**
- [ ] Runs sandboxed: network and its own folder only; files only through
  the trusted dialogs; tabs isolated from each other.
- [ ] Every parser fuzzed; a bad page can crash a tab, never the system.
- [ ] HTTPS by default, plain HTTP marked "not secure", no telemetry.

**Done when:** `http://example.com/` and `https://example.com/` render
readably; a set of test pages matches reference screenshots; fuzzing finds
no crash; a page can't read files or other tabs.

---

## Phase 16 — The Spec language compiles for Cerberus

Nothing of the compiler exists yet (`spec/` holds only empty folders).

- [ ] The language itself (SPEC §13.1) and the compiler pipeline: lexer,
  parser, type checker, intermediate code (§13.2).
- [ ] x86-64 code generation and an ELF writer.
- [ ] Standard library and Cerberus system-call bindings (§13.3).
- [ ] Tooling: formatter, test runner, build tool (§13.4).
- [ ] At least three applications rewritten in Spec, one with a GUI.

**Done when:** `specc hello.spec -o hello` makes a program that runs on Cerberus,
and a Spec GUI application runs on the desktop.

---

## Phase 17 — Running real software

- [ ] Shared libraries and a dynamic linker (`/lib/ld-cerberus.so`, full
  RELRO), `dlopen`.
- [ ] A POSIX-compatible C library big enough to build unmodified programs
  (listed in `docs/LIBC.md`). Proof: `zlib`, `lua`, `tinycc`, `make`.
- [ ] Pseudo-terminals, job control (Ctrl+Z, `fg`, `bg`), full signals.
- [ ] Package manager `pkg` (install, remove, list, search, upgrade) with
  signed (Ed25519) packages; install scripts sandboxed.
- [ ] *(speed)* Measure program start-up and dynamic-link time.

**Done when:** Lua and the Tiny C Compiler built from upstream source run
on Cerberus; tcc compiles and runs a C program on Cerberus; a tampered package is
refused; a 50-library GUI app starts in under 200 ms.

**Owner decision first:** grow our own C library or port mlibc.

---

## Phase 18 — Installing to a disk, updates, encryption, recovery

Design notes for the installer and dual boot: docs/INSTALLER.md.

- [ ] Live installer, like Windows Setup:
  - [ ] pick a disk (model, size, partitions and existing systems shown);
  - [ ] "Erase this disk and install Cerberus", or "Install alongside the
    existing system" in free space;
  - [ ] review every change before anything is written; erasing needs the
    disk's name typed;
  - [ ] create a user, copy the system, install the boot loader, reboot.
- [ ] Boot menu listing Cerberus, the previous Cerberus kernel, Cerberus recovery
  and **Windows** (or other systems found).
- [ ] UEFI boot entry so the PC starts Cerberus's menu; BitLocker, Secure Boot
  and Fast Startup checks with clear instructions.
- [ ] GPT and FAT32 support; making a bootable USB documented (Rufus "DD
  image" mode or balenaEtcher).
- [ ] Optional full-disk encryption (AES-256-XTS or XChaCha20, key from a
  passphrase via Argon2id).
- [ ] Signed updates; the previous kernel stays bootable; never updates on
  its own.
- [ ] Recovery boot entry with a read-only root shell; `fsck.cerfs` that
  repairs.
- [ ] Service manager (start order, restart policy, per-service sandbox,
  resource limits), `svc` tool, log viewer.

**Done when:** installed to a blank disk it boots without the ISO; an
encrypted disk shows only ciphertext; an interrupted update still boots;
recovery repairs a damaged disk. Proposed addition: installing beside a
fake Windows leaves every Windows byte unchanged and both boot from the
menu.

---

## Phase 19 — Stretch goals

- [ ] USB (XHCI controller, keyboards and mice, USB sticks).
- [ ] Sound (Intel HD Audio, a mixing server).
- [ ] IPv6.
- [ ] Kernel address randomisation (KASLR).
- [ ] Booting on real PCs (and installing beside Windows on them).
- [ ] The Spec compiler compiling itself; a Spec package ecosystem.
- [ ] HiDPI scaling; more accessibility features.
- [ ] ARM64 port.

---

## Known bugs and loose ends (fix list)

- [ ] *(fix)* Backtraces can't name a function that crashes in its first
  instructions (a limit of the frame-pointer method).
- [ ] *(fix)* The shell polls the serial port every tick instead of using
  its interrupt.
- [ ] *(fix)* `init` starts nothing yet; it only collects orphaned
  processes.
- [ ] *(fix)* Terminal window: no selection or colours (solved by Ember in
  phase 13).
- [ ] *(fix)* Remove old diagnostic shell commands (`timermode`, `gui`,
  `irqs`) or fold them into Gauge and the test suite.
- [ ] *(fix)* `early_map` should hand its addresses over to the VMM's
  device mappings.
- [ ] *(fix)* "Clock skew detected" warnings from `make` on the Windows
  drive (harmless).
- [ ] Security left-overs: decide on retpolines; a per-core stack-protector
  value.
- [ ] Not yet checked: UEFI boot and software emulation for this release; a
  non-debug (`DEBUG=0`) build since 0.5.0.

---

## Performance: where we are and what to improve

Budgets are defined for QEMU/KVM (docs/BENCH.md has every measurement).

| What | Now (0.0.5a) | Budget | Status |
|---|---|---|---|
| Switching between threads | 22–24 ns | 2,000 ns | well within |
| Waking a waiting thread | 7–9 µs | 1,000 µs | well within |
| Kernel memory allocate + free | 76–84 ns | 100 ns | within |
| System call round trip | 33–34 ns | 300 ns | well within |
| First touch of a memory page (minor fault) | 2.0–2.9 µs | 2 µs | **over** |
| Idle desktop CPU use | 0 % | under 1 % | within |
| Full-screen redraw on VirtualBox | ~23 ms | 8 ms (phase 12) | **over** |

**Improvements planned**
- [ ] *(speed)* Profile and cut the minor page-fault cost under 2 µs.
- [x] *(speed)* Faster window dragging: only changed pixels written
  (0.0.5b). The framebuffer was already write-combining.
- [ ] *(speed)* One-shot timer: frame pacing at the display rate, and later
  a tickless kernel (idle cores fully asleep, better for laptops).
- [ ] *(speed)* Cheaper cross-core wake-ups: let an idle core poll briefly
  before sleeping, or wake a thread on the waker's core when the waker is
  about to sleep.
- [ ] *(speed)* Split the single scheduler lock if measurements show
  contention with more cores.
- [ ] *(speed)* Use more than 32 cores (thread affinity is a 32-bit mask).
- [ ] *(speed)* Page cache with read-ahead (phase 9); zero-copy window
  buffers (phase 12); zero-copy network receive (phase 14).
- [ ] *(speed)* Boot to desktop under 3 s (phase 13); app start under
  200 ms (phase 17).
- [ ] *(speed)* Measure a non-debug build regularly (debug builds carry
  extra checks).

---

## Decisions waiting on the owner

1. None blocking right now: on 2026-10-04 the owner said to finish all
   phases, so they follow one another. Answered that day: our own web
   browser is wanted (phase 15B); the OS is called Cerberus.
2. TCP/IP: write our own or port lwIP (before phase 14).
3. TLS/crypto: Mbed TLS (recommended), BearSSL, or our own (before
   phase 15).
4. C library: grow our own or port mlibc (before phase 17).
5. Confirm the phase order and the non-goals list (spec v2).
6. Confirm the scope of the network toolbox: connecting with your own
   credentials, a password manager for your own secrets, diagnostics for
   machines you own. Breaking into other people's Wi-Fi is not built.
7. (Done in 0.0.5e: NVMe support.)
