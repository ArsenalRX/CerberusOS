# Cerberus — To Finish

A living checklist of what is done, what is in progress, and what remains.
Pairs with `docs/STATUS.md` (current state) and `docs/SPEC.md` (the full plan).
Read this to pick up work. Dates are absolute. Last updated 2026-10-05 (release 0.0.5g).

**What this file is for.** The ordered list of everything left to build, and
the one place that says what to do first.

**When to update it.** Whenever a phase or item is finished, added,
re-scoped or re-ordered; whenever the "Right now" section stops being true;
and at the end of any session that changed what remains.

**How to update it.** Tick finished items and move them to "Done" with the
date. Keep the phase list identical in numbering and scope to docs/SPEC.md
§5 and §5A; if they disagree, the spec wins and this file is fixed in the
same session. Rewrite "Right now" so it always names the single next job.
Update the "Last updated" date.

**Why.** docs/STATUS.md says what is true; this file says what to do about
it. If it drifts from the spec, sessions build the wrong thing or in the
wrong order.

**Before starting any item below,** follow docs/SPEC.md §21 (implementation
procedure): read, threat-check (§19), budget-check (§20), design, test
first, implement, self-review, verify with real output, document, commit.

---

## Right now: desktop polish rounds, then phase 12 (Pane)

Version **0.0.5g** (desktop polish: calendar, reminders, notifications,
animations, snapping, Alt+Tab, Settings cards, VirtualBox mouse) is the
release of 2026-10-05. The owner said on 2026-10-05: "do more polish until
we start phase 12", with a fixed loop: each iteration ships to VirtualBox,
updates the HTML list on their Windows desktop, and ends with **8
numbered candidates**; the owner picks by number (CLAUDE.md, "After every
iteration"). docs/FEATURES.md lists what exists; docs/BUGS.md the bugs.

**The owner's instruction of 2026-10-05 (evening):** "do all 1-8
additions and re-go through all phases 1-11 and see if you can iterate to
make it better each phase; i want you to complete it. I mess around with
it then ask you to iterate." So, in order, each step a verified build in
`dist/` and an updated Desktop HTML list:

1. **Round 2 (→ 0.0.5h), the 8 additions:** (1) saved settings and
   reminders — the owner's "do all" is the yes to auto-mounting a cerfs
   disk at boot (`/data`), with a Settings button that formats an empty
   disk for it; (2) text selection, copy and paste in the terminal and a
   desktop clipboard; (3) a Files window; (4) a Notes text editor;
   (5) a Calculator; (6) keyboard settings (layouts, repeat, lock
   indicators); (7) a lock screen (Super+L, password in Settings, hashed);
   (8) a system tray with quick settings, a CPU/RAM meter and the
   notification history. New windows go in their own files under
   `kernel/gui/` (an app = paint + key + click + wheel over a Surface),
   dispatched from `desktop.cpp`.
2. **Round 3 (→ 0.0.5i…), phases 1–11 revisited:** for each phase, read
   its SPEC §5/§5A rows and docs/DECISIONS.md deviations, run its tests
   and benchmarks, and close what is closable: phase 4 the minor-page-fault
   cost; phase 6 one-shot timer / tickless idle; phase 9 `fsck`-lite and
   hard links; phase 10 MSI-X (NVMe/virtio interrupts), keyboard LEDs;
   phase 11 non-blocking `port_send`, signal masks, demand-paged file
   mmap, passing ports through ports; `make bench` and a new
   docs/BENCH.md block. Record each in docs/BUGS.md or docs/DECISIONS.md.
3. Then phase 12 (below) when the owner says so.

First steps of phase 12, in order (SPEC §5, §5A and §9 first):

1. Write the Pane protocol (docs/SPEC.md §9) as a header shared by server
   and clients: messages over a port, window buffers as shared memory,
   input events back to the focused client only.
2. `pane`, a user program: opens `/dev/fb0` and `/dev/input/*`, creates the
   port `pane`, waits in `event_wait`. It needs an `mmap` of `/dev/fb0`
   (not there yet: `/dev/fb0` is read/write only) and a way to change the
   resolution from user mode (an ioctl over `kernel/drivers/bga.cpp`).
3. Move libgfx (`kernel/gfx/`) into a library both the kernel and user
   programs build (it is freestanding already; user programs have SSE).
4. `libpane` and one client (a terminal needs ptys, which are phase 17: start
   with a clock or the System Monitor, which needs only `sysinfo`).
5. Port the compositor, window management, panel, launcher and Settings
   from `kernel/gui/desktop.cpp`; keep the kernel desktop bootable behind a
   switch until Pane passes the `desktop*` tests.
6. The +v2 rows: client isolation tests, fuzz the protocol, 8 ms frames.

- Phase 11 left-overs: demand-paged and shared file mappings, a
  non-blocking `port_send`, signal masks, passing ports through ports.
- Phase 10 left-overs: MSI-X (NVMe and virtio-blk are polled), keyboard
  LEDs, layouts other than US.
- **Minor page fault is over budget** (docs/BENCH.md). Profile it first.
- Desktop left-overs: one-shot-timer frame pacing, saving preferences and
  reminders (needs the auto-mount decision, B-011), a Settings window that
  scrolls (B-012), a real application icon set, text selection and
  copy/paste in the terminal, a file manager and an editor (phase 13).
- `make bench` was not re-run for 0.0.5f: run it and add a block to
  docs/BENCH.md (port round trip, futex, thread create/join are new).
- Overview for the owner: docs/ROADMAP.md; explaining Cerberus: docs/ABOUT.md;
  installer research: docs/INSTALLER.md; own web browser: phase 15B.

---

## Done (committed)

- **Phase 0 — Scaffolding.** Repo layout, cross toolchain
  (`toolchain/build-cross.sh`, GCC 13.3.0 x86_64-elf), Limine 11.4.1, all
  `make` targets, QMP boot probe.
- **Phase 1 — Boot and output.** BootInfo from Limine, COM1 serial, `kprintf`,
  PSF2 framebuffer console, boot banner.
- **Phase 2 — CPU structures and interrupts.** GDT/TSS, IDT with symbolised
  exception dumps and backtraces, embedded symbol table, PIC/ACPI/APIC/IOAPIC,
  HPET, self-correcting 100 Hz timer, kernel shell, RTC, early PS/2 keyboard.
- **Phase 3 — Physical memory.** Bitmap frame allocator, `test pmm`.
- **Desktop preview** (owner-requested, brought forward). libgfx software
  renderer, damage-tracked compositor, panel + launcher, Terminal/About/
  System Monitor/Memory Map windows, PS/2 mouse, drag/resize/stack, hotkeys.
- Infrastructure the owner asked for: KVM-accelerated QEMU path
  (`run-cerberus.cmd`), per-run VirtualBox serial logs (`logs/`), `make dist`.
- **Release 0.3.0 (2026-10-03).** Shared PS/2 drain routine, spin-idle under
  the desktop, `gui`/`irqs` shell commands, version scheme (`VERSION` file,
  `docs/CHANGELOG.md`, spec §23), spec v2.
- **Phase 4 — Virtual memory. Release 0.4.0 (2026-10-03).** `AddressSpace`
  (map/unmap/protect/translate), VMAs (anonymous, device, guard),
  `mmap`/`munmap`/`mprotect`, demand paging, COW clone, guarded kernel
  stacks, 2 MiB kernel leaves; W^X, NX outside kernel text, image mapped by
  section, zeroed frames, null guard; other CPUs parked in kernel text;
  zero-initialised locals and a UBSAN subset; `test vmm`,
  `test exceptions so|ub`. File-backed regions deferred to phase 9; boot and
  IST stacks get guard pages in phase 6.
- **Phase 5 — Kernel heap. Release 0.5.0 (2026-10-03).** Slab caches 16–2048
  bytes, whole pages above; `kmalloc/kzalloc/kfree/krealloc/kfree_sensitive`;
  encoded and validated free lists; debug red zones, poisoning and call-site
  tracking; `heapstat`; `test heap`, `test exceptions fl|waf|df`. Desktop
  pixel buffers and VMM records moved onto it.
- **Release 0.5.1 (2026-10-03).** Parked CPUs halt instead of spinning (fixes
  the keyboard on VirtualBox with several CPUs); `dist/` keeps one ISO.
- **Phase 6 — Threads and scheduling. Release 0.6.0 (2026-10-03).** Threads,
  processes, four-level preemptive scheduler, sleep and wait queues,
  spinlock/mutex/semaphore/condition variable/reader-writer lock, idle and
  reaper threads; compositor thread; idle `hlt`; guard pages under every
  stack; atomic `kprintf`; `ps`, `bench`, `make bench`, `docs/BENCH.md`;
  `test sched`; the test probe waits for output instead of fixed times.
  `Process` gets its file table and working directory in phase 9.
- **Phase 7 — Userland and syscalls. Release 0.7.0 (2026-10-03).** Ring 3,
  `syscall`/`sysret`, dispatch generated from `table.def`
  (docs/SYSCALLS.md), validated user copies, hardened ELF64 loader (PIE
  only), 14 system calls, `fork`/`execve`/`waitpid`, in-tree libc, `init`,
  boot archive; SMEP/SMAP/UMIP, user ASLR, stack protector everywhere,
  ChaCha20 CSPRNG + `getrandom`, `make fuzz` (ELF, tar, random system
  calls); `run` shell command; system-call benchmark. Deviations recorded
  in docs/DECISIONS.md.
- **Phase 8 — SMP. Release 0.0.5a (2026-10-04).** All CPUs started through
  the bootloader; per-CPU data, run queues, work stealing; reschedule, TLB
  and stop IPIs; TLB shootdown; per-address-space VMM lock; per-CPU heap
  slabs; lock-rank checker; every "interrupts off" lock replaced; FPU state
  saved on every switch; branch-free user-copy masking; time-stamp-counter
  reference clock (and a PIT race fixed); `test smp`,
  `test exceptions lo`. Deviations recorded in docs/DECISIONS.md. New
  version scheme (owner).
- **Release 0.0.5b (2026-10-04): desktop polish.** Windows-style caption
  buttons, anti-aliased rounded shapes, terminal scrollback (wheel,
  Shift+PgUp/PgDn, scrollbar), only changed pixels written, input-paced
  frames, Super-key and menu fixes, `test terminal`.
- **Phase 9 — Files and file systems. Release 0.0.5c (2026-10-04).** VFS
  (mounts, symlinks, permissions, mount flags, name cache), tmpfs, read-only
  initramfs root, devfs, page cache (read-ahead, write-back, journal holds),
  cerfs with a journal and CRC32C, mkfs.cerfs (host and in-OS), AHCI and
  IDE drivers (from phase 10), 25 new system calls, file tools, shell
  redirection; tests: acceptance (IDE and AHCI), power cut and recovery,
  cache, permissions, in-kernel fuzzing. The OS renamed Cerberus.
- **Release 0.0.5d (2026-10-04): anti-aliased text.** Build-time rendered
  DejaVu fonts (`tools/gen-fonts.py`, `kernel/gfx/fonts.bin`), smooth close
  button, dithered wallpaper.
- **Phase 10 — Drivers. Release 0.0.5e (2026-10-04).** Driver model, PCI
  MSI, AHCI by interrupt, NVMe, virtio transport + virtio-blk, PS/2
  keyboard in set 2 with the full map, `/dev/input`, `/dev/fb0`, device
  access rule, ACPI power off and restart, `runas`, sticky `/tmp`.
- **Phase 11 — IPC. Release 0.0.5f (2026-10-04).** Plus the Settings
  window, launcher with search, floating taskbar, resolution switching.
- **Release 0.0.5g (2026-10-05): desktop polish.** Calendar with
  reminders, notifications, animations, window snapping, Alt+Tab
  switcher, Super+D, double-click maximise, right-click menu, Print
  Screen, terminal history and completion, Settings as cards with a hue
  strip, six wallpapers, time zone, 18 resolutions (ultrawide), frame
  rate; VirtualBox absolute mouse (`vmmdev`); Limine without a menu wait;
  docs/BUGS.md and docs/FEATURES.md.

---

## Spec phases remaining (in order — do not start N+1 before N passes)

Each ends with a bootable system and its acceptance test (see `docs/SPEC.md`
§5). The desktop preview grows into the real thing across phases 6, 11, 12, 13.

Spec v2 (2026-10-03) added security and performance rows to phases 4–13
(§5A; marked **+v2** below) and renumbered phases 14 onward. A phase is done
only when its §5 criteria *and* its §5A rows are demonstrated with real
output.

- [x] **Phase 11 — IPC and window-server foundation.** Done 2026-10-04
  (0.0.5f): ports, shared memory, signals, futex, threads in programs,
  event queues, private file `mmap`. Tests `user-ipc`, `user-threads`,
  `user-signals`, `user-events`, `user-mmap`.
- [ ] **Phase 12 — Pane: window server as a userland process.** Move the
  compositor out of the kernel behind the port protocol (`docs/SPEC.md` §9);
  per-window shm buffers, `libpane`. The kernel-hosted desktop is the
  reference implementation to port.
  **+v2:** client isolation (no reading other windows, no input injection,
  no key logging); user-mediated screen capture and clipboard; secure
  password entry; fuzzed protocol; 8 ms frame budget.
- [ ] **Phase 13 — Facet toolkit, shell, applications.** Widget toolkit,
  desktop shell, and the apps in §12 (Ember terminal, Crate files, Slate
  editor, Gauge, Dial, Tally, Frame, Daub, Chronos, Warren, lsh, CLI utils).
  **+v2:** per-application permission declarations; trusted file dialogs
  that hand over a file descriptor; every file-format parser fuzzed; boot
  to desktop under 3 s.
- [ ] **Phase 14 — Networking.** virtio-net + e1000; `netd` userland stack
  (Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS, loopback); socket
  syscalls 70–83; default-deny inbound firewall; per-app network
  permission; `ping ifconfig nslookup fetch netstat`; network page in Dial.
  Acceptance includes a capture proving no unsolicited traffic. *Owner
  decision needed first: in-tree stack vs lwIP.*
- [ ] **Phase 15 — Security model, users, encrypted transport.** Users and
  groups, Argon2id password hashes, credentials, `authd` broker (no
  setuid), login and lock screens, `restrict` sandbox, permission prompts,
  keyring, TLS client with certificate store, audit log. *Owner decision
  needed first: TLS/crypto source.*
- [ ] **Phase 15B — Our own web browser** (owner, 2026-10-04). HTTP/HTTPS,
  HTML (WHATWG parsing, common cases), a CSS subset, block/inline layout,
  images, tabbed UI; no JavaScript yet. Sandboxed, every parser fuzzed.
  See docs/SPEC.md phase 15B.
- [ ] **Phase 16 — Spec native backend** (was phase 14). x86-64 codegen, ELF
  emitter, stdlib, Cerberus syscall bindings; rewrite ≥3 apps in Spec.
  (Spec = the spec's "Glint".)
- [ ] **Phase 17 — Software platform.** Dynamic linking with full RELRO,
  POSIX-subset libc (`docs/LIBC.md`), ptys and job control, full signals,
  signed package manager `pkg`; proof ports: zlib, Lua, tinycc, make.
- [ ] **Phase 18 — Installer, updates, storage encryption, recovery.**
  Live-ISO installer (GPT, cerfs + FAT32 EFI), optional full-disk
  encryption, signed updates with a fallback kernel, recovery boot entry
  and repairing `fsck.cerfs`, service manager with per-service sandbox.
  Owner asked (2026-10-04) for a Windows-Setup-like flow (pick a drive,
  erase or keep the existing system) and a boot menu with Windows: design
  notes in docs/INSTALLER.md.
- [ ] **Phase 19 — Stretch** (was phase 15). USB, audio, IPv6, KASLR, real
  hardware, self-hosting Spec, HiDPI, ARM64.

---

## Waiting on the owner

Details in `docs/DECISIONS.md`, entry 2026-10-03.

- [ ] TLS/cryptography source: port Mbed TLS (recommended), port BearSSL, or
  write in-tree. Before phase 15.
- [ ] libc: in-tree or port mlibc. Before phase 17 (phase 7 wrote the minimal
  in-tree one).
- [ ] TCP/IP: in-tree or port lwIP. Before phase 14.
- [ ] Confirm the v2 phase order and non-goals.

---

## Owner-added requirements (beyond the spec)

- [ ] **Sleek desktop, "Linux and Windows combined."** Ongoing across the
  desktop phases. Waiting on the owner's reference screenshots to lock the
  theme, panel, and window chrome. Keep shadows, rounded corners,
  animations, coherent dark theme (`docs/SPEC.md` §10).
- [ ] **Internet access.** Now spec phase 14 (networking) and phase 15 (TLS,
  keyring): virtio-net and e1000 NICs (QEMU and VirtualBox both emulate
  e1000), a TCP/IP stack in a userland server, a connection UI in the
  desktop, a password manager (the keyring) for the user's own credentials.
- [ ] **Privacy and security first (owner, 2026-10-03).** Spec §19 and the
  §5A rows; checklist in §19.12.
- [ ] **Smooth, memory- and hardware-efficient (owner, 2026-10-03).** Spec
  §20 budgets, measured by `make bench` from phase 6.
- [x] **Smoothness on VirtualBox.** Handled as far as the kernel can (console shadow buffer,
  plain-store framebuffer copies, spin-idle). Fundamentally limited by the
  Hyper-V backend; `run-cerberus.cmd` (QEMU+KVM) is the smooth path. Revisit
  once the compositor has its own thread (phase 6).
- [ ] **All-in-one toolbox — DECLINED as framed.** "Get into wifis and find
  passwords" is unauthorized network access and credential theft; not built,
  regardless of host OS. The legitimate version, which will be built: connect
  to networks with credentials the user has, a password manager for the
  user's own secrets, and network diagnostics for machines they own or are
  authorized to test. Awaiting the owner to confirm that scope.

---

## Known issues to clean up

- [ ] Backtraces can't name a function that faults in its own prologue
  (inherent to RBP walking).
- [ ] Terminal has no selection or escape sequences (Ember, the real
  terminal, is phase 13); scrollback done in 0.0.5b.
- [ ] `make` on the Windows-mounted tree occasionally warns "Clock skew
  detected" (drvfs timestamps). Harmless.
- [ ] Remove the phase-2/3 diagnostics (`timermode`, `gui`, `irqs`
  shell commands) once no longer needed, or fold them into a proper
  `Gauge`/`test` surface. (`gui_diag_line` and its timer hook were removed
  on 2026-10-03.)

---

## How to work (quick reference)

- Build and test run in WSL2 Ubuntu-24.04:
  `wsl -d Ubuntu-24.04 -u root -- bash -c 'cd /mnt/d/Programs/OS && make ...'`.
- `make` builds, `make iso` builds the ISO, `make test` runs the integration
  tests headless in QEMU (KVM), `make fuzz` runs the fuzz harnesses, `make dist` snapshots to `dist/`.
- Run for real: double-click `run-cerberus.cmd` (QEMU window via WSLg), or boot
  `dist/cerberus.iso` in a VirtualBox VM.
- Long builds: run detached (`setsid nohup … &`) and poll a log; the Bash tool
  caps at 10 minutes.
- Write source files with the editor, not shell heredocs (quotes break the
  Bash tool). Don't put `$var` inside `wsl … bash -c '…'` (arrives empty).
- Before touching a VM, check `VBoxManage showvminfo <vm> --machinereadable |
  grep VMState` so you don't kill the owner's session.
- Commit per phase: `phaseN: …`.
- Every change follows `docs/SPEC.md` §21; every doc edit follows §22.
- Never dereference a user pointer, never create a writable+executable
  mapping, never log user content, never add a dependency or a network
  connection the owner did not approve (`docs/SPEC.md` §19).
