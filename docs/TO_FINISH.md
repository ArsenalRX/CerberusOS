# Lumen — To Finish

A living checklist of what is done, what is in progress, and what remains.
Pairs with `docs/STATUS.md` (current state) and `docs/SPEC.md` (the full plan).
Read this to pick up work. Dates are absolute. Last updated 2026-10-04 (after release 0.0.5a).

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

## Right now: waiting for the owner's go-ahead (phase 9 or desktop polish)

Version **0.0.5a** (phase 8, SMP) was released on 2026-10-04 (tag
`v0.0.5a`; first release under the letter scheme, docs/SPEC.md §23.1). All
24 integration tests pass in QEMU/KVM, and the build was walked through on
VirtualBox with 4 CPUs. The tree is on `0.0.5b` development builds. The
owner approves each phase's contents before work starts, so **do not start
phase 9 until the owner says so**.

Owner requests of 2026-10-04, waiting for go-ahead:

- **Desktop polish:** minimise/maximise/close buttons in the Windows style
  (simple glyphs, close turns red on hover) instead of the coloured
  macOS-style dots; rounder, smoother (anti-aliased) window corners;
  smoother window dragging. On VirtualBox a full-screen composite takes
  23 ms (about 130 MB/s into video memory): check whether the framebuffer
  is mapped write-combining there (PAT) before anything else.
- Fix: Super used in a shortcut also toggles the launcher menu; keys typed
  with the menu open reach the terminal.
- **Installer and dual boot:** researched in docs/INSTALLER.md (feeds
  phase 18; NVMe would need adding to phase 10 for real PCs).

Open items carried over:

- `/dev/random` (needs devfs, phase 9); retpoline decision; per-CPU stack
  guard.
- **Minor page fault is over budget** (2.0–2.9 µs against 2 µs,
  docs/BENCH.md). Profile it before changing anything.
- Frame pacing is tied to the 10 ms tick; a one-shot timer would help the
  desktop.

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
  (`run-lumen.cmd`), per-run VirtualBox serial logs (`logs/`), `make dist`.
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

---

## Spec phases remaining (in order — do not start N+1 before N passes)

Each ends with a bootable system and its acceptance test (see `docs/SPEC.md`
§5). The desktop preview grows into the real thing across phases 6, 11, 12, 13.

Spec v2 (2026-10-03) added security and performance rows to phases 4–13
(§5A; marked **+v2** below) and renumbered phases 14 onward. A phase is done
only when its §5 criteria *and* its §5A rows are demonstrated with real
output.

- [ ] **Phase 9 — Filesystem.** VFS, tmpfs, initramfs (tar), devfs, **lumfs**
  (on-disk, journal) + `mkfs.lumfs`. Dentry cache. Acceptance: format,
  mount, write 4 MB, remount, byte-compare; journal replay after a hard
  kill.
  **+v2:** permissions enforced; mount flags (`nosuid,nodev,noexec,ro`);
  `openat`/`O_NOFOLLOW`/`O_CLOEXEC`; lumfs range-checks every on-disk field,
  metadata checksums, fuzzed; one unified page cache (replaces the buffer
  cache) with read-ahead and `fsync`.
- [ ] **Phase 10 — Drivers.** PCI, AHCI/SATA (DMA), full PS/2 keyboard+mouse
  behind `/dev/input` (replace the early drivers), RTC, driver model.
  **+v2:** virtio transport + virtio-blk; device-supplied values
  bounds-checked; `/dev/input/*` and `/dev/fb0` restricted to the window
  server; ACPI shutdown/reboot.
- [ ] **Phase 11 — IPC and window-server foundation.** Ports, shared memory,
  signals, futex, userland threading. `port`/`shm` self-tests.
  **+v2:** port access control with kernel-supplied sender identity; bounded
  queues; event multiplexing (`event_create/ctl/wait`).
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
- [ ] **Phase 16 — Spec native backend** (was phase 14). x86-64 codegen, ELF
  emitter, stdlib, Lumen syscall bindings; rewrite ≥3 apps in Spec.
  (Spec = the spec's "Glint".)
- [ ] **Phase 17 — Software platform.** Dynamic linking with full RELRO,
  POSIX-subset libc (`docs/LIBC.md`), ptys and job control, full signals,
  signed package manager `pkg`; proof ports: zlib, Lua, tinycc, make.
- [ ] **Phase 18 — Installer, updates, storage encryption, recovery.**
  Live-ISO installer (GPT, lumfs + FAT32 EFI), optional full-disk
  encryption, signed updates with a fallback kernel, recovery boot entry
  and repairing `fsck.lumfs`, service manager with per-service sandbox.
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
  Hyper-V backend; `run-lumen.cmd` (QEMU+KVM) is the smooth path. Revisit
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
- [ ] About window's CPU line clips on long brand strings — add ellipsis to
  the value column.
- [ ] Terminal has no selection, scrollback, or escape sequences (Ember, the
  real terminal, is phase 13).
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
- Run for real: double-click `run-lumen.cmd` (QEMU window via WSLg), or boot
  `dist/lumen.iso` in a VirtualBox VM.
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
