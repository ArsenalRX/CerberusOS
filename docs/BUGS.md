# Bug ledger

**What this file is for.** Every bug that was found and fixed, and every
bug that is known and still open, in one place: the symptom as the owner or
a test saw it, the real cause, what was changed, and how the fix is kept
from coming back. It is the project's memory of failure. A bug fixed
without an entry here will be fixed again.

**When to update it.** The moment a bug is understood (even before it is
fixed: add it as OPEN), and again when it is fixed and verified. Also when
an old bug comes back: add a new entry that links the old one rather than
editing it.

**How to use it.** Read the OPEN entries at the start of every session and
before touching the area they are in. When something strange happens,
search this file for the symptom first. Each entry has an id (`B-nnn`) so
that commits, docs/CHANGELOG.md and docs/DECISIONS.md can refer to it.

**How to write an entry.** `### B-nnn — one-line symptom` then the fields
below. Keep "Cause" to what was actually established, not what was
suspected. "Guard" names the test, check or rule that now catches the
class of bug; if there is none, say so.

| Field | Meaning |
|---|---|
| Found | date, and who or what found it (owner, a test, a review) |
| Where | files or subsystem |
| Symptom | what was seen |
| Cause | the actual defect |
| Fix | what changed (and the version it shipped in) |
| Guard | the test / check / rule that now covers it |
| Status | FIXED (date) or OPEN |

---

## Open

### B-010 — `eventtest` misses its 1 ms wake-up limit on VirtualBox
- Found: 2026-10-04, running the phase 11 programs on VirtualBox.
- Where: `kernel/ipc/event.cpp`, `userland/bin/eventtest.cpp`; the
  VirtualBox Hyper-V backend.
- Symptom: a waiting server wakes about 1.6 ms (median) after a message
  on VirtualBox; 53 us under KVM. The test's 1 ms limit fails there.
- Cause: inter-processor interrupts are slow on that backend; the kernel
  is not at fault. Not profiled further.
- Fix: none. The limit is right for KVM.
- Guard: none on VirtualBox.
- Status: OPEN.

### B-012 — The Settings window does not scroll when it is small
- Found: 2026-10-04.
- Where: `paint_settings` in `kernel/gui/desktop.cpp`.
- Symptom: below its minimum height the lower cards are cut off.
- Cause: content is laid out for the minimum size (620x440); there is no
  scrolling in the kernel desktop.
- Fix: partly, 0.0.5g: the card layout fits every tab inside the minimum
  size at 1280x800; at 800x600 the Display tab is still cut off.
- Guard: none (visual).
- Status: OPEN (minor; Pane's toolkit will scroll in phase 13).

---

## Fixed

### B-011 — Settings and reminders were lost at restart
- Found: 2026-10-04 (owner: "keep the desktop customisable").
- Where: `kernel/fs/fs.cpp`, `kernel/gui/desktop.cpp`.
- Symptom: every preference, the custom accent and every reminder were
  gone after a restart.
- Cause: nowhere to keep them: the root file system is read-only, `/tmp`
  is memory, and a cerfs disk was only mounted by hand.
- Fix (0.0.5h): the first disk holding a cerfs volume labelled `data` is
  mounted at `/data` at boot; Settings → Storage sets a blank disk up for
  it (format, two presses); the desktop writes `/data/desktop.conf` and
  `/data/reminders.txt` and reads them at start. The owner's "do all 1-8"
  was the go-ahead for mounting at boot.
- Guard: `desktop-storage-1` and `-2` (format, change, reboot, read back,
  disk checked on the host).
- Status: FIXED 2026-10-05.

### B-014 — A regenerated `fonts.bin` was not linked in (build)
- Found: 2026-10-05, when the lock screen's clock did not appear after
  digits were added to the display fonts.
- Where: `Makefile`, `kernel/boot/font.S`.
- Symptom: `make iso` finished but the kernel still had the old fonts.
- Cause: the object that `.incbin`s the blob depended only on its `.S`
  file, not on the blob.
- Fix: an explicit dependency on `kernel/gfx/fonts.bin` and the PSF files.
- Guard: the Makefile rule.
- Status: FIXED 2026-10-05.

### B-001 — VirtualBox: "CPU does not support 64-bit" at the Limine menu
- Found: 2026-10-03 and again 2026-10-05 (owner, every new VM).
- Where: VirtualBox VM settings, `tools/vbox-attach.sh`.
- Symptom: a VM made by dragging the ISO into VirtualBox stops at the
  boot menu with a 64-bit error.
- Cause: the wizard creates the VM as OS type "Other/Unknown", which is
  32-bit and hides long mode from the guest.
- Fix: `VBoxManage modifyvm <vm> --ostype Other_64 --x86-long-mode on`.
  `make dist` runs `tools/vbox-attach.sh`, which does this for every
  powered-off VM that boots an ISO from `dist/`. 2026-10-05: the owner's
  VM "6" fixed by hand.
- Guard: `tools/vbox-attach.sh` on every `make dist`; docs/STATUS.md "How
  to run it" explains it.
- Status: FIXED (recurs for every new VM; the guard handles it).

### B-002 — Limine's boot menu stays on screen on VirtualBox
- Found: 2026-10-04 (owner), confirmed 2026-10-05 with a headless boot.
- Where: `limine.conf`.
- Symptom: with `timeout: 1` the menu never counts down on VirtualBox;
  the user has to press Enter.
- Cause: not established in Limine itself; the one-second countdown does
  not run on that backend (likely its timer source).
- Fix: `timeout: 0` — the ISO boots the kernel at once (0.0.5g).
- Guard: every integration test boots through this path.
- Status: FIXED 2026-10-05.

### B-003 — At 1920x1080 on VirtualBox the mouse cannot reach the bottom
- Found: 2026-10-05 (owner, screenshots).
- Where: `kernel/drivers/ps2mouse.cpp`, new `kernel/drivers/vmmdev.cpp`.
- Symptom: the desktop pointer stops short of the bottom (and right) of
  the screen; worse at larger resolutions.
- Cause: VirtualBox only delivered relative PS/2 motion. When the VM
  window is scaled or the host accelerates the mouse, the guest pointer
  moves a different distance from the host pointer; when the host pointer
  hits the window edge no more motion arrives, so the guest pointer is
  left short of its edge.
- Fix: a driver for VirtualBox's guest device (PCI 80ee:cafe): on every
  PS/2 packet the kernel asks the host where its pointer is and the
  desktop puts the cursor there (0.0.5g). This also gives mouse
  integration (no capture).
- Guard: `vmmdev:` boot line on VirtualBox says whether the host sends
  positions. QEMU has no such device, so the tests cover the relative path
  only; the absolute path is checked by hand on VirtualBox.
- Status: FIXED 2026-10-05 (verify on the owner's ultrawide).

### B-004 — The About window clipped "Cerberus" when made narrow
- Found: 2026-10-04.
- Where: `paint_about`, `create_window` in `kernel/gui/desktop.cpp`.
- Symptom: the wordmark and the subtitle ran off the right edge.
- Cause: no minimum size for About; plain `draw_text`.
- Fix: About's minimum size is 430x340; the subtitle uses
  `draw_text_ellipsis` (0.0.5g).
- Guard: none (visual).
- Status: FIXED 2026-10-05.

### B-005 — Super+D brought windows back in the wrong stacking order
- Found: 2026-10-05, by `desktop-qol.expect` (a click meant for the
  terminal hit the Settings window).
- Where: `toggle_show_desktop` in `kernel/gui/desktop.cpp`.
- Symptom: after Super+D twice the window that had been on top was not.
- Cause: windows were restored in slot order, and each restore raises.
- Fix: restore in stacking order, bottom to top.
- Guard: `desktop-qol.expect` (types into the terminal after Super+D
  twice).
- Status: FIXED 2026-10-05.

### B-013 — Page fault at boot on VirtualBox after the new `vmmdev` driver
- Found: 2026-10-05, by the VirtualBox check (4 CPUs) of the 0.0.5g build;
  QEMU and all 42 tests had passed.
- Where: `kernel/drivers/vmmdev.cpp`, `kernel/drivers/driver.cpp`.
- Symptom: `#PF` with RIP=0 in `init_thread` right after the `vmmdev:`
  boot line; the system halted before the desktop.
- Cause: the driver described itself as a PCI driver (class 08/80) with no
  `attach` function, and `drivers_probe_all` called that null pointer when
  it offered the VMMDev PCI device to it. QEMU has no such device, so the
  tests never reached the call.
- Fix: the driver is a platform entry noted as attached by hand (as `bga`
  is); the registry also skips PCI drivers without `attach`.
- Guard: the registry's null check; the rule "always also verify on
  VirtualBox with 4 CPUs" (it caught this one too).
- Status: FIXED 2026-10-05.

### B-006 — `user-ipc` failed once in a full run (timing)
- Found: 2026-10-04, one failure in a `make test`.
- Where: `userland/bin/ipctest.cpp`.
- Symptom: a 50 ms wait measured as 49 ms.
- Cause: a wait counted in timer ticks and measured with the reference
  clock can read 1 ms short.
- Fix: the test allows 45 ms. Looped 16 times afterwards without failure.
- Guard: the rule in CLAUDE.md ("Timing checks in tests: leave a margin").
- Status: FIXED 2026-10-04.

### B-007 — Keyboard stalled on VirtualBox with several CPUs
- Found: 2026-10-03 (owner).
- Where: `kernel/arch/x86_64/smp.cpp`.
- Symptom: typing stopped working after a while on a 4-CPU VM.
- Cause: parked CPUs spinning starved the CPU that served the keyboard
  interrupt under the Hyper-V backend.
- Fix: parked CPUs `hlt` (release 0.5.1).
- Guard: the rule "always also verify on VirtualBox with 4 CPUs".
- Status: FIXED 2026-10-03.

### B-008 — PIT race in the reference clock
- Found: 2026-10-04, on VirtualBox.
- Where: `kernel/drivers/refclock.cpp`.
- Symptom: time jumped on VirtualBox, where the TSC is not trustworthy and
  the PIT is the clock.
- Cause: reading the PIT counter without latching while it wrapped.
- Fix: latched reads and a wrap check (0.0.5a).
- Guard: `test timer`; VirtualBox verification.
- Status: FIXED 2026-10-04.

### B-009 — Patch scripts lost backslashes (tooling)
- Found: 2026-10-05, three times in one session.
- Where: the Git Bash heredoc path into WSL's python3.
- Symptom: a Python patch written through a heredoc turned `\n` inside a
  C string into a real newline; the kernel then failed to compile with
  "missing terminating \" character".
- Cause: one level of backslash escaping is removed by the shell path.
- Fix: write patch scripts with the editor into `build/wip/` and run them
  from there; grep for a line that is only `");` afterwards.
- Guard: the rule in CLAUDE.md "Pitfalls".
- Status: FIXED (process).
