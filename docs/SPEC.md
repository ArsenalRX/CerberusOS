# Lumen OS + Glint — Full Build Specification

**This document is the prompt.** Paste it into Claude Code (or keep it at
`docs/SPEC.md` in the repo and reference it every session). It defines a
complete operating system with a graphical desktop and a systems programming
language that targets it.

**Version 2 (2026-10-03).** v1 treated security, privacy, networking and
performance as non-goals or stretch goals. v2 makes them requirements: the
owner's priorities are, in order, (1) the privacy and security of the person
using the machine, (2) a system that feels smooth and uses memory and hardware
efficiently, (3) real working programs and internet access. What changed and
why is recorded in `docs/DECISIONS.md` (entry dated 2026-10-03).

**Naming.** This document says "Glint" for the language. The owner named it
**Spec**; the translation table is the first entry in `docs/DECISIONS.md`.

**How this document is maintained.** §22 says when, how and why each file in
`docs/` is updated. Read §19 (security and privacy), §20 (performance) and §21
(implementation procedure) before implementing anything; they apply to every
phase.

---

# 0. Working agreement (read this first, every session)

You are building a real operating system. The failure mode is writing 10,000
lines that don't boot. Avoid it as follows:

1. **Work in phases.** Phases are numbered in §5. Do not start phase N+1 until
   phase N's acceptance criteria all pass. State which phase you're in at the
   start of every session.
2. **Every phase ends with a working, bootable system.** If the kernel doesn't
   boot in QEMU at the end of a phase, the phase isn't done.
3. **Verify, don't assume.** After every meaningful change, run `make run` and
   confirm the expected output. Paste the actual QEMU output into your reply.
   Never say "this should work."
4. **Small commits.** One logical change per commit. Message format:
   `phase3: add bitmap physical frame allocator`.
5. **Write the test as you go.** Every kernel subsystem gets a self-test
   function callable from the kernel shell (`test pmm`, `test vmm`, etc.).
6. **When stuck, reduce.** If something doesn't work, cut it down to the
   smallest reproducing case before adding more code. Use the GDB workflow in
   §15.
7. **Keep `docs/DECISIONS.md` current.** Every non-obvious design choice gets a
   dated entry with the reasoning and the alternatives rejected. Future
   sessions read this file.
8. **Keep `docs/STATUS.md` current.** At the end of every session, update: what
   phase, what works, what's half-done, what's next, any known bugs. This is
   how the next session picks up.
9. **Ask before deviating.** If a spec decision here turns out to be wrong or
   impractical, say so explicitly and propose the alternative. Do not silently
   substitute a different design.
10. **No placeholder code.** No `// TODO: implement`. If something isn't
    implemented yet, it isn't in the tree yet.
11. **Security and privacy come first.** When a design choice trades the
    user's privacy or security against convenience, speed, or less code, the
    user's privacy and security win. Every change is checked against the
    rules in §19 before it is committed. A feature that cannot be built
    safely yet is not built yet.
12. **Smoothness is a requirement, not a later optimisation.** Every
    subsystem has a budget in §20. A change that breaks a budget is a bug,
    found by measurement (`make bench`), not by feel.
13. **Follow the implementation procedure in §21** for every change: read,
    threat-check, design, test first, implement, verify, measure, document,
    commit.
14. **No new dependency without the owner's approval.** That includes
    vendored source. Propose it in `docs/DECISIONS.md` as an open question
    and wait.

---

# 1. What we are building

**Lumen** — a hybrid-kernel x86-64 operating system with a compositing
graphical desktop.

**Glint** — a statically typed systems language that compiles to native x86-64
and targets Lumen's syscall ABI. Userland applications are written in it.

The thesis that justifies doing both: *a language designed alongside its OS can
express things a portable language cannot* — syscalls as typed language
constructs, kernel capabilities as types the compiler checks, MMIO register
layouts verified at compile time, and drivers that cannot compile if they
violate the hardware's access rules.

## Non-negotiables

- Target: **x86-64 only**. No 32-bit, no ARM, until everything else is done.
- Boot: **UEFI via Limine**. We do not write a bootloader.
- Kernel language: **freestanding C++20**. No exceptions, no RTTI, no
  libstdc++, `-ffreestanding -fno-stack-protector -fno-pic -mno-red-zone
  -mcmodel=kernel -mno-sse -mno-mmx` (SSE re-enabled only in userland).
- Kernel is preemptive and SMP-aware from phase 8 onward.
- Everything must run in QEMU. Real hardware is a phase-19 stretch goal.
- No external runtime dependencies in the final image except Limine, and any
  the owner approves in `docs/DECISIONS.md` (see §19.9 on cryptography, the
  one place where porting audited code is safer than writing our own).
- The user/kernel boundary and the boundary between processes are security
  boundaries. Nothing crosses them without validation (§19).
- The OS never sends data off the machine unless the user asked for that
  specific connection. No telemetry, no analytics, no "phone home" (§19.8).

---

# 2. Repository layout

```
lumen/
├── Makefile                  # top-level: build, run, debug, test, iso
├── docs/
│   ├── SPEC.md               # this document
│   ├── DECISIONS.md          # dated design decisions + rationale
│   ├── STATUS.md             # current state, updated every session
│   ├── TO_FINISH.md          # checklist of remaining work, phase by phase
│   ├── BENCH.md              # benchmark results per phase (from phase 6)
│   ├── SYSCALLS.md           # generated from kernel/syscall/table.def
│   └── PROTOCOL.md           # Pane window protocol wire format
├── toolchain/
│   ├── build-cross.sh        # builds x86_64-elf-gcc + binutils
│   └── limine/               # vendored bootloader
├── kernel/
│   ├── arch/x86_64/          # gdt, idt, apic, tss, smp, context switch asm
│   ├── boot/                 # limine entry, early console, boot info
│   ├── mm/                   # pmm, vmm, kheap, slab, vma
│   ├── sched/                # threads, processes, scheduler, sync primitives
│   ├── syscall/              # dispatch, table.def, argument validation
│   ├── fs/                   # vfs, tmpfs, initramfs (tar), lumfs, devfs
│   ├── ipc/                  # ports, shared memory, signals
│   ├── drivers/              # pit, apic-timer, ps2, ahci, pci, framebuffer
│   ├── lib/                  # printf, string, list, hashmap, bitmap, spinlock
│   └── main.cpp
├── userland/
│   ├── libc/                 # minimal C runtime (crt0, syscall stubs, malloc)
│   ├── libglint/             # Glint runtime: allocator, panic, entry
│   ├── pane/                 # window server + compositor
│   ├── facet/                # GUI widget toolkit (C++ first, Glint later)
│   ├── shell/                # desktop shell: panel, launcher, wallpaper
│   ├── apps/                 # see §11
│   └── bin/                  # CLI utilities
├── glint/
│   ├── src/                  # compiler: lexer, parser, check, ir, codegen
│   ├── lib/                  # Glint standard library (.gl)
│   ├── examples/
│   └── tests/
├── tests/
│   ├── kernel/               # in-kernel unit tests
│   ├── integration/          # QEMU + serial harness, expected-output files
│   └── glint/                # compiler test suite
└── build/                    # artifacts, gitignored
```

---

# 3. Toolchain and build

## Cross compiler

`toolchain/build-cross.sh` builds `x86_64-elf-gcc` 13.x and matching binutils
into `toolchain/out/`. Script is idempotent; re-running with the toolchain
present is a no-op.

## Required host tools

`nasm`, `xorriso`, `qemu-system-x86_64`, `gdb`, `make`, `python3`.
The Makefile checks for each and prints a clear install hint if missing.

## Make targets

| Target | Behaviour |
|---|---|
| `make` | Build kernel + userland + Glint compiler |
| `make iso` | Produce `build/lumen.iso` with Limine + initramfs |
| `make run` | Build iso, boot QEMU, serial to stdout |
| `make debug` | Same, with `-s -S`, waits for GDB |
| `make gdb` | Attach GDB with `kernel.sym`, source dirs preloaded |
| `make test` | Run kernel unit tests + integration + Glint tests, exit nonzero on failure |
| `make bench` | Run the benchmarks in §20.9 and print results (from phase 6) |
| `make fuzz` | Run the fuzz harnesses in §19.11 with a time budget (from phase 7) |
| `make clean` | Remove `build/` |

## QEMU invocation (exact)

```
qemu-system-x86_64 \
  -machine q35 -cpu qemu64,+pdpe1gb -smp 4 -m 512M \
  -cdrom build/lumen.iso -boot d \
  -serial stdio -display gtk \
  -d guest_errors -no-reboot -no-shutdown
```

Serial port `0x3F8` is the kernel log. `-display none` variant for CI.

---

# 4. Coding standards

- 4 spaces, no tabs. 100-column soft limit.
- `snake_case` functions and variables, `PascalCase` types, `SCREAMING_CASE`
  constants and macros.
- Headers use `#pragma once`.
- No raw `new`/`delete` in kernel outside the heap implementation itself.
  Use `kmalloc`/`kfree` or the slab allocator.
- Every function that can fail returns a `Result<T, Error>` (kernel-local
  template, no exceptions). Never return a bare pointer that might be null
  without the type saying so.
- Kernel panics: `PANIC("message", args...)` — prints message, register dump,
  and a stack backtrace, then halts all CPUs.
- Assertions: `ASSERT(cond)` compiled in debug builds; `ASSERT_ALWAYS(cond)`
  always.
- Every file starts with a comment stating what the file is responsible for.
- Comments explain *why*, never *what*. No comment that restates the code.
- Public kernel APIs documented in the header with: what it does,
  preconditions, what it returns on failure, whether it can sleep, and whether
  it's safe to call from an interrupt context.

---

# 5. Phase plan

Each phase lists its **deliverables** and its **acceptance criteria**. The
acceptance criteria are literal: you must demonstrate each one.

**v2:** phases 4–13 each have additional security and performance
deliverables and acceptance criteria, listed in §5A directly after phase 13.
They are part of the phase: a phase is not done until its §5A rows pass too.
Phases 14 onward were renumbered in v2 (old 14 is now 16, old 15 is now 19).

---

## Phase 0 — Scaffolding

**Deliverables:** repo layout, cross-compiler script, Makefile with all targets
stubbed, Limine vendored, `docs/DECISIONS.md` and `docs/STATUS.md` created.

**Accept:** `make` succeeds and produces an empty kernel binary. `make run`
launches QEMU (which will fail to boot — that's expected).

---

## Phase 1 — Boot and output

**Deliverables:**
- Limine config, higher-half kernel at `0xFFFFFFFF80000000`.
- Parse Limine requests: memory map, framebuffer, HHDM offset, kernel address,
  RSDP, module list, SMP info. Store in a `BootInfo` struct copied to kernel
  memory before Limine's structures are reclaimed.
- Serial driver (COM1, 115200 8N1), polling first.
- PSF v2 bitmap font renderer to the framebuffer. Embed the font as a linked
  binary blob.
- `kprintf` supporting `%d %u %x %p %s %c %%`, width, zero-pad, `%lu`, `%llx`.
  It writes to both serial and framebuffer console.
- Framebuffer text console: scrolling, colour, cursor.
- A boot banner printing: kernel version, build date, memory map summary,
  framebuffer resolution, CPU vendor/brand string from CPUID.

**Accept:** QEMU shows the banner on screen *and* on serial. Memory map lines
match what QEMU reports.

---

## Phase 2 — CPU structures and interrupts

**Deliverables:**
- GDT with kernel code/data (ring 0), user code/data (ring 3), and a TSS per
  CPU. Correct ordering for `syscall`/`sysret` (STAR MSR layout).
- IDT with all 256 entries. Named handlers for all 32 CPU exceptions.
- Exception handler prints: exception name, error code decoded (for #PF: the
  faulting address from CR2 plus present/write/user/reserved/instruction-fetch
  flags), full register dump, RIP with symbol resolution, and a stack
  backtrace walking RBP.
- IST stacks for #DF, #NMI, #MC.
- PIC remapped and masked; Local APIC enabled; I/O APIC configured with
  identity-ish routing for legacy IRQs.
- APIC timer calibrated against the PIT, one-shot and periodic modes.
- Symbol table: build step that extracts `kernel.sym` from the ELF and embeds
  a compact `(address, name)` table so backtraces show names.

**Accept:**
- `test exceptions` in the kernel shell deliberately triggers #DE, #UD, #PF and
  each prints a complete, correct dump (then halts — that's fine for now).
- Timer interrupt fires at 100 Hz and a tick counter is visible.

---

## Phase 3 — Physical memory

**Deliverables:**
- Bitmap-based physical frame allocator over the Limine memory map.
  `pmm_alloc(count)`, `pmm_free(addr, count)`, `pmm_stats()`.
- Contiguous multi-frame allocation (first-fit over the bitmap).
- Reserve: kernel image, Limine structures, framebuffer, the bitmap itself.
- Statistics: total, used, free, reserved, largest free run.

**Accept:** `test pmm` allocates 10,000 frames in random order, frees them in a
different random order, and verifies the bitmap returns to its initial state
and stats match. Allocating all of memory then freeing it all works.

---

## Phase 4 — Virtual memory

**Deliverables:**
- 4-level paging. `AddressSpace` type wrapping a PML4.
- `map(virt, phys, size, flags)`, `unmap`, `translate`, `protect`.
  Flags: present, writable, user, no-execute, write-through, cache-disable,
  global.
- Automatic intermediate table allocation; correct TLB invalidation
  (`invlpg` for single pages, CR3 reload for bulk).
- Kernel half mapped into every address space; HHDM for physical access.
- `AddressSpace::clone()` for `fork`, with copy-on-write marking.
- Demand paging: the #PF handler resolves faults against the VMA list.
- VMA (virtual memory area) list per address space: `[start, end, flags,
  backing]` where backing is anonymous, file, or device.
- `mmap`/`munmap`/`mprotect` primitives at kernel level.
- Guard pages below every stack.

**Accept:** `test vmm` maps a page, writes, reads back, unmaps, and confirms
the next access faults with the right CR2. A COW clone is made, written to by
both sides, and each sees only its own writes. A stack overflow hits the guard
page and reports cleanly rather than corrupting memory.

---

## Phase 5 — Kernel heap

**Deliverables:**
- Slab allocator for fixed-size objects (16, 32, 64, 128, 256, 512, 1024,
  2048 bytes), backed by the VMM.
- Large allocations fall through to direct page allocation.
- `kmalloc`, `kzalloc`, `kfree`, `krealloc`.
- Debug mode: red zones around allocations, poison on free (`0xDE` on alloc,
  `0xEF` on free), allocation tracking with call-site recording, and a
  `heapstat` command listing outstanding allocations by call site.

**Accept:** `test heap` runs 100,000 random alloc/free pairs of random sizes
with no corruption, then reports zero leaks. Deliberate buffer overrun is
detected by the red zone check.

---

## Phase 6 — Threads and scheduling

**Deliverables:**
- `Thread` structure: kernel stack, saved context, state (ready/running/
  blocked/sleeping/zombie), priority, time slice, CPU affinity, parent process.
- `Process`: PID, address space, thread list, file descriptor table, working
  directory, parent/children, exit status, credentials.
- Context switch in assembly: save/restore callee-saved registers, swap stacks,
  swap CR3 when the address space differs.
- Round-robin scheduler with 4 priority levels and per-level run queues.
  Priority boost for threads that block on I/O, decay for CPU hogs.
- Preemption on the APIC timer.
- Idle thread per CPU that `hlt`s.
- Sleep/wake: `thread_sleep_ms`, wait queues, `wake_one`, `wake_all`.
- Sync primitives: spinlock (with interrupt-state save), mutex, semaphore,
  condition variable, RW lock.
- Kernel threads: `kthread_create(fn, arg, name)`.

**Accept:** Five kernel threads print interleaved counters and are visibly
preempted. `test sched` runs a producer/consumer with a bounded buffer across
threads for 10 seconds with correct totals and no deadlock.

---

## Phase 7 — Userland and syscalls

**Deliverables:**
- Ring 3 entry: build an IRET frame and jump into user code.
- `syscall`/`sysret` fast path: MSRs `STAR`, `LSTAR`, `SFMASK` configured;
  per-CPU kernel stack swapped via `swapgs` and the TSS.
- Syscall dispatch table generated from `kernel/syscall/table.def` (an X-macro
  file) so the number, name, signature, and doc comment live in one place and
  both the kernel table and `docs/SYSCALLS.md` are generated from it.
- **Argument validation is mandatory.** Every pointer from userland is checked:
  in user range, mapped, correct permission. `copy_from_user`/`copy_to_user`
  with fault-safe access. Never dereference a user pointer directly.
- ELF64 loader: parse program headers, map PT_LOAD segments with correct
  permissions, set up the stack with argv/envp/auxv, jump to the entry point.
- Initial syscalls: `exit`, `write`, `read`, `open`, `close`, `mmap`, `munmap`,
  `getpid`, `yield`, `sleep_ms`, `fork`, `execve`, `waitpid`.
- Minimal libc: `crt0.S`, syscall stubs, `malloc` (dlmalloc-style on top of
  `mmap`), `memcpy`/`memset`/`strlen` family, `printf`, `exit`.
- `init` as the first userland process, loaded from the initramfs.

**Accept:** A userland `hello` binary prints via `write(1, ...)` and exits with
a status the kernel reports. `fork` + `execve` + `waitpid` round-trips
correctly. A userland program passing a bad pointer to a syscall gets `-EFAULT`
rather than crashing the kernel.

---

## Phase 8 — SMP

**Deliverables:**
- Parse Limine's SMP response; start APs via Limine's `goto_address`.
- Per-CPU data via `GS` base (`swapgs` on kernel entry/exit).
- Per-CPU run queues, GDT, TSS, IDT (shared IDT is fine), APIC timer.
- Work stealing when a CPU's run queue empties.
- TLB shootdown via IPI: a CPU unmapping a shared page sends an IPI to all CPUs
  running threads of the same address space, and waits for acknowledgement.
- All previously written locks audited for SMP correctness.

**Accept:** 4 CPUs report in at boot. `test smp` runs a shared-counter
increment with a spinlock across all CPUs for 1,000,000 iterations and the
count is exact. TLB shootdown test: one CPU unmaps while another reads, and the
reader faults correctly.

---

## Phase 9 — Filesystem

**Deliverables:**
- VFS layer: `VNode` with an operations table
  (`open/close/read/write/seek/stat/readdir/mkdir/unlink/rename/truncate/
  ioctl/mmap`). Mount table, path resolution with `.`/`..`, symlink following
  with a loop limit.
- File descriptor table per process, `dup`/`dup2`, close-on-exec.
- **tmpfs**: in-memory, full read/write, the default for `/tmp`.
- **initramfs**: USTAR tar archive passed as a Limine module, mounted at `/`
  read-only at boot.
- **devfs** at `/dev`: `null`, `zero`, `random`, `console`, `fb0`, `tty0`,
  `input/kbd0`, `input/mouse0`, `disk/sda`.
- **lumfs**: our own on-disk filesystem.
  - Superblock at LBA 0: magic `LUMFS\0\0\0`, version, block size (4096),
    total blocks, free blocks, inode count, root inode, block bitmap offset,
    inode table offset, journal offset, UUID, label.
  - Inodes: 128 bytes — mode, uid, gid, size, atime/mtime/ctime, link count,
    12 direct blocks, 1 indirect, 1 double-indirect.
  - Directories: linked list of `(inode, name_len, type, name)` records.
  - Journal: write-ahead metadata journal, circular, with transaction commit
    records. Replay on mount.
  - `mkfs.lumfs` host tool (build it in `tools/`) so images can be created
    outside the OS.
- Path cache (dentry cache) with LRU eviction.
- Buffer cache for block devices with write-back and an explicit `sync`.

**Accept:** Format a QEMU virtual disk with `mkfs.lumfs`, mount it, create a
directory tree 5 levels deep, write a 4 MB file, unmount, remount, verify
contents byte-for-byte. Kill QEMU mid-write (`-no-shutdown`, hard kill) and
confirm journal replay leaves the FS consistent on next mount.

---

## Phase 10 — Drivers

**Deliverables:**
- **PCI**: enumerate bus/device/function, read config space, BAR decoding,
  MSI/MSI-X setup, a driver registration/match table.
- **AHCI/SATA**: identify device, read/write via command lists and FIS,
  NCQ optional, interrupt-driven with DMA.
- **PS/2 keyboard**: scancode set 2, full key map, modifier tracking, key
  repeat, translation to a `KeyEvent {keycode, unicode, modifiers, pressed}`.
- **PS/2 mouse**: streaming mode, 3-button + scroll wheel, movement deltas
  to absolute position clamped to the screen.
- Input event queue exposed via `/dev/input/*`, readable by the window server.
- **RTC**: read the current date and time at boot; maintain via timer ticks.
- Driver model: `Driver { name, probe(), attach(), detach() }` with a registry.

**Accept:** Typing in QEMU produces correct characters including shift, caps,
and symbols. Moving the mouse moves a cursor drawn on the framebuffer. A file
written to the SATA disk survives a reboot.

---

## Phase 11 — IPC and the window server foundation

**Deliverables:**
- **Ports**: named message endpoints. `port_create(name)`,
  `port_connect(name)`, `port_send(port, msg, len, fds)`,
  `port_recv(port, buf, len, timeout)`. Messages are bounded (64 KB), queued,
  and can carry file descriptors and shared-memory handles.
- **Shared memory**: `shm_create(size)` returns a handle; `shm_map(handle,
  flags)` maps it. Handles are transferable over ports. Reference counted.
- **Signals**: a minimal set (`SIGKILL`, `SIGTERM`, `SIGSEGV`, `SIGCHLD`,
  `SIGUSR1/2`) with user-registered handlers and a signal trampoline.
- **Futex**: `futex_wait(addr, val, timeout)` / `futex_wake(addr, count)` so
  userland can build mutexes without syscalls in the fast path.
- Userland threading: `thread_spawn`, `thread_join`, TLS via `FS` base; a
  pthread-ish API in libc.

**Accept:** Two userland processes exchange 100,000 messages through a port
with correct ordering and no loss. A shared memory region written by one
process is visible to the other. A futex-based mutex protects a shared counter
across 4 processes with an exact final count.

---

## Phase 12 — Pane: window server and compositor

See §9 for the full protocol. Deliverables:

- Compositor owning `/dev/fb0`, double-buffered, damage-tracked.
- Window management: create, destroy, move, resize, stack, focus, minimise,
  maximise, fullscreen.
- Per-window shared-memory buffers; clients draw into their buffer and send a
  damage rectangle.
- Alpha blending, per-window opacity, drop shadows, rounded corners.
- Input routing: keyboard to the focused window, mouse to the window under the
  cursor, with grab support for drags.
- Hardware cursor emulation (software cursor composited last).
- Client library `libpane` with a clean C++ API.

**Accept:** Three client windows can be dragged, resized, stacked, and focused.
Dragging one window over another produces no tearing and no flicker. Damage
tracking is proven: a client repainting a 10×10 region causes only that region
to be recomposited (verify with an instrumented counter).

---

## Phase 13 — Facet toolkit, desktop shell, and applications

See §10, §11, §12. Deliverables: the widget toolkit, the desktop shell, and all
the applications listed in §12.

**Accept:** Boot to a graphical desktop. Open the file manager, browse to a
text file, open it in the editor, edit, save, close, reopen, verify the change
persisted. Open the terminal and run commands. All of this with the mouse and
keyboard only.

---

## 5A. Security and performance additions to phases 4–13 (v2)

Each row is a deliverable with its own acceptance test, added to the phase
named. The reasoning for every item is in §19 (security) or §20
(performance). Build the item in the phase where its mechanism is built;
retrofitting protection later is how holes are left behind.

### Phase 4 — Virtual memory

- **W^X everywhere.** `map()` rejects any request that is both writable and
  executable (`Error::Invalid`). Enable `EFER.NXE`. The kernel image is
  remapped by section: `.text` read+execute, `.rodata` read-only+NX,
  `.data`/`.bss` read-write+NX. The HHDM and the kernel heap are NX.
- **Zero before handing out.** Every frame mapped into a user address space
  or returned by a kernel allocation that may reach userland is zeroed first,
  so one process never sees another's old data.
- **Null guard.** The first 64 KiB of every user address space is never
  mappable.
- **2 MiB pages** for the HHDM and for any aligned kernel mapping of 2 MiB or
  more; fewer TLB misses and fewer page-table frames.
- **Compiler hardening turned on now** (needs only small runtime support, so
  it does not wait for userland): `-ftrivial-auto-var-init=zero` so no stack
  variable is ever uninitialised, and the undefined-behaviour sanitizer
  (`-fsanitize=undefined` with in-kernel handlers that panic with the source
  location) in debug builds.
- **Accept:** `test vmm` additionally shows: a W+X map request fails; a write
  to `.rodata` and a write to `.text` each fault; a jump into a data page
  faults with the instruction-fetch bit set; a freshly mapped user page reads
  as all zeroes after the frame previously held a known pattern.

### Phase 5 — Kernel heap

- Freed objects are poisoned in debug builds (already specified) and the
  slab free list stores its next-pointers XORed with a per-boot secret so a
  heap overflow cannot trivially redirect an allocation.
- `kfree_sensitive(ptr, size)` zeroes before freeing; mandatory for keys,
  passwords, and buffers that held user file data.
- Allocation fast path takes one lock and touches one cache line; measured
  by `make bench` (§20.9).
- **Accept:** `test heap` additionally shows a corrupted free-list pointer is
  detected (panic with the slab name) rather than followed.

### Phase 6 — Threads and scheduling

- Every kernel stack has an unmapped guard page below it; overflow produces
  a clean #DF dump on the IST stack, never silent corruption.
- The compositor gets its own kernel thread at interactive priority; the
  idle thread halts again (removes the spin-idle stopgap).
- A thread that wakes from input or a timer runs within 1 ms on an idle CPU
  and within one time slice (10 ms) on a busy one.
- `make bench` exists from this phase: context-switch time and wake-up
  latency, recorded in `docs/BENCH.md`.
- **Accept:** the desktop stays responsive (cursor moves, windows drag) while
  `test idle` or any long shell command runs. A deliberately recursive kernel
  thread reports "kernel stack overflow" with a backtrace.

### Phase 7 — Userland and syscalls

- **SMEP, SMAP, UMIP** enabled when CPUID reports them. All user-memory
  access goes through `copy_from_user`/`copy_to_user`/`strncpy_from_user`,
  which are the only functions that execute `stac`/`clac`.
- **Syscall entry hardening:** number bounds-checked before the table lookup;
  every length checked for overflow (`ptr + len` must not wrap or leave the
  user half); every flag word rejects unknown bits; every struct copied to
  userland is fully initialised, padding included.
- **No kernel addresses to userland.** `%p` output, `sysinfo`, and error
  paths never reveal kernel pointers to an unprivileged process.
- **ASLR for userland:** randomised stack top, `mmap` base, and load base
  for position-independent executables. The libc and all shipped binaries
  are built as PIE.
- **Stack protector:** userland is built with `-fstack-protector-strong`
  from the first binary. The kernel switches from `-fno-stack-protector` to
  `-fstack-protector-strong` once the per-CPU canary is set up from the
  CSPRNG.
- **CSPRNG:** ChaCha20-based generator seeded from RDSEED/RDRAND when
  present, mixed with interrupt timing and the HPET counter; reseeded
  periodically. Exposed as `getrandom` (syscall 66) and `/dev/random`. It is
  the only source of randomness for ASLR, canaries, and (later) keys.
- **ELF loader is hostile-input code:** every offset, size and count is
  validated against the file size and against overflow; segments that are
  writable and executable are refused; fuzzed under `make fuzz`.
- **Accept:** a user program that passes a kernel address, a wrapping
  length, or an unmapped pointer to each syscall gets `-EFAULT`/`-EINVAL`
  (a test program walks the whole table). With SMAP on, a deliberate direct
  dereference of a user pointer in a test syscall faults. Two runs of the
  same binary print different stack and `mmap` addresses. `make fuzz` runs
  the ELF loader harness for 60 s with no crash.

### Phase 8 — SMP

- Lock ordering is documented in one header (`kernel/lib/lock_order.h`) and
  checked in debug builds (each lock has a rank; taking a lower rank while
  holding a higher one panics).
- Per-CPU slab caches and per-CPU run queues so the common path takes no
  shared lock.
- **Accept:** `test smp` additionally runs the heap and scheduler benchmarks
  on 4 CPUs with the lock-rank checker enabled and no violation.

### Phase 9 — Filesystem

- **Permissions enforced** in the VFS on every operation: owner/group/other
  mode bits, uid/gid from the process credentials (§19.4). Until phase 15
  everything runs as uid 0, but the checks exist and are tested with a
  synthetic non-root credential.
- **Mount flags** `nosuid`, `nodev`, `noexec`, `ro`. `/tmp` and removable
  media mount `nodev,nosuid`.
- **Race-free path handling:** `openat`-style resolution relative to a
  directory fd, `O_NOFOLLOW`, `O_CLOEXEC`; symlink loop limit 40.
- **lumfs is hostile-input code:** every on-disk field is range-checked at
  mount and on read; a corrupted image yields `Error::IO`, never a panic.
  Metadata blocks carry a checksum (CRC32C).
- **One page cache** shared by file reads, file-backed `mmap`, and block
  I/O (replaces v1's separate buffer cache); read-ahead for sequential
  access; write-back by a kernel thread; `fsync` flushes one file.
- **Accept:** a non-root credential cannot read a mode-0600 root file;
  `mount -o noexec` refuses `execve`; `make fuzz` mutates a lumfs image for
  60 s and the kernel never panics; reading a 64 MB file sequentially twice
  shows the second read served from cache (counter in `sysinfo`).

### Phase 10 — Drivers

- **virtio-blk** alongside AHCI (QEMU's fast path), and the **virtio**
  transport (PCI modern) written once for reuse by virtio-net in phase 14.
- Every value read from a device (lengths, indices, counts) is treated as
  untrusted and bounds-checked before use.
- `/dev/input/*` and `/dev/fb0` are openable only by the window server's
  credential; no other process can read keystrokes or the screen directly.
- ACPI shutdown and reboot (not the QEMU debug port).
- **Accept:** an unprivileged test process gets `-EACCES` opening
  `/dev/input/kbd0` and `/dev/fb0`; `poweroff` powers off QEMU via ACPI.

### Phase 11 — IPC

- **Port access control:** `port_create` takes a mode; `port_connect` is
  checked against it. The receiver learns the sender's pid/uid from the
  kernel (unforgeable), not from the message.
- Message sizes and fd counts are bounded and validated; a full queue blocks
  or returns `-EAGAIN`, never drops or grows without limit.
- **Event multiplexing** (`event_create`/`event_ctl`/`event_wait`, syscalls
  90–92) over fds, ports, timers and child exit, so servers sleep instead
  of polling.
- **Accept:** a process without permission cannot connect to a protected
  port; a server blocked in `event_wait` uses 0% CPU while idle and wakes
  within 1 ms of a message.

### Phase 12 — Pane

- **Client isolation.** A client can read only its own buffers, receives
  only input addressed to its own windows, and cannot synthesise input,
  move other clients' windows, or learn their titles or contents.
- **Mediated capture.** Screenshot, screen recording, global hotkey
  registration and clipboard history are privileged requests granted per
  application by the user (§19.5); denied by default.
- **Clipboard** content is delivered only to the focused window on an
  explicit paste, not broadcast.
- **Secure entry:** while a password field has focus, no other client
  receives key events and capture requests are refused.
- Every protocol message is length- and range-checked; a malformed message
  disconnects the client, never crashes the server. Fuzzed.
- Frame budget: composite in under 8 ms at 1920×1080 with 10 windows;
  no work when nothing is damaged; zero-copy shm buffers.
- **Accept:** a test client that requests another window's buffer, injects
  a key, or grabs the screen without permission is refused and logged;
  `make fuzz` on the protocol for 60 s leaves Pane running; the frame-time
  counter meets the budget under the phase-12 drag test.

### Phase 13 — Toolkit, shell, applications

- Applications declare the permissions they need in their `.desktop` entry
  (`Permissions=files:home,network,…`); the session starts each with
  exactly those (enforced by `restrict`, phase 15; declared from phase 13).
- File dialogs run in the trusted shell process and hand the application a
  file descriptor, so an application without filesystem permission can
  still open the one file the user picked.
- Every parser for external data (PNG, BMP, TGA, TTF, tar, `.desktop`,
  `.conf`) is bounds-checked and fuzzed.
- **Accept:** `make fuzz` covers every listed parser for 60 s each with no
  crash; a cold-boot to an interactive desktop takes under 3 s in QEMU/KVM.

---

## Phase 14 — Networking

Design in §19.7 (security) and §20.7 (performance).

**Deliverables:**
- **virtio-net** driver (primary) and **e1000** driver (VirtualBox and QEMU
  default), each exposed to userland as a packet device (`/dev/net/eth0`)
  with zero-copy receive rings where the device allows.
- **`netd`**, the userland network server (hybrid-kernel rule, §6.2):
  Ethernet framing; ARP with a bounded, expiring cache; IPv4 with fragment
  reassembly bounded by size and time; ICMP echo; UDP; TCP with the full
  state machine, retransmission with RTT estimation (RFC 6298), fast
  retransmit and recovery (NewReno), window scaling, randomised initial
  sequence numbers (RFC 6528) and randomised ephemeral ports; loopback.
- **DHCP client**, **DNS resolver** (randomised query IDs and source ports,
  bounded cache honouring TTLs).
- **Open owner decision:** write the stack in-tree (the default in this
  spec; full control, but other independent OSes report TCP staying
  incomplete for years) or port **lwIP** into `netd` (less work, a
  dependency needing approval under §0 rule 14). Either way the drivers,
  `netd`, the socket syscalls and the firewall are ours.
- **Socket API** (syscalls 70–83): `socket bind connect listen accept send
  recv sendto recvfrom shutdown getsockopt setsockopt getsockname
  getpeername`, usable with `event_wait` and `O_NONBLOCK`.
- **Firewall:** inbound connections denied by default; rules by
  port/protocol/direction in `/etc/lumen/firewall.conf`.
- **Per-application network permission** (§19.5): a process without it gets
  `-EACCES` from `socket`.
- Tools: `ping`, `ifconfig`, `nslookup`, `fetch` (HTTP/1.1 GET), `netstat`.
- Network page in Dial; panel indicator showing connection state.
- All packet parsers fuzzed.

**Accept:** under QEMU user networking, DHCP obtains 10.0.2.15; `ping
10.0.2.2` gets replies; `nslookup example.com` resolves; `fetch
http://example.com/` prints the page; a 100 MB TCP transfer to and from a
host listener matches by SHA-256; an inbound connection to a closed port is
dropped and logged; a process without network permission cannot open a
socket; `make fuzz` on the packet parsers runs 60 s with no crash; no packet
leaves the machine at boot other than DHCP and what the user started
(verified with `-object filter-dump`).

---

## Phase 15 — Security model, users, and encrypted transport

Design in §19.

**Deliverables:**
- **Users and groups:** `/etc/passwd`, `/etc/group`, `/etc/shadow`
  (mode 0600); passwords hashed with Argon2id (RFC 9106) with a per-user
  salt, verified against the RFC test vectors.
- **Credentials** on every process: real/effective uid and gid,
  supplementary groups (syscalls 100–111). **No setuid binaries.**
  Privilege elevation goes through `authd`, a small privileged service
  reached over a port, which authenticates the user and performs a
  named, audited operation.
- **Login screen and lock screen** (Super+L, idle timeout); auto-login is
  off by default.
- **`restrict(flags, paths)`** (syscall 110): a process irrevocably drops
  abilities — network, filesystem outside listed paths, spawning, raw
  devices. Inherited across `fork`/`execve`. The session applies the
  `.desktop` permission list with it.
- **Permission prompts** for capture, clipboard history, and network, shown
  by the shell (trusted path), remembered per application in
  `/etc/lumen/permissions.conf`.
- **Secrets store** (`keyring` service): per-user secrets encrypted with a
  key derived from the login password; unlocked at login, wiped at lock.
- **TLS client** (1.3 preferred, 1.2 the minimum) for `fetch` and the
  package manager, with a root certificate store. Implementation source is
  an open owner decision (§19.9).
- **Audit log:** logins, failed authentications, privilege elevation,
  permission grants and denials, to `/var/log/audit` (root-readable only).

**Accept:** a second user cannot read the first user's home; a wrong
password is rejected and logged, with a growing delay; Argon2id output
matches the RFC 9106 vectors; a sandboxed process that tries the network,
a file outside its paths, or `fork` gets `-EACCES`; the lock screen cannot
be bypassed by killing the client (Pane holds the lock state); `fetch
https://example.com/` succeeds and a connection to a host with a bad
certificate is refused.

---

## Phase 15B — Our own web browser (owner, 2026-10-04)

Added by the owner ("cool features ie internet our own web browser"). Built
on phase 13 (toolkit), 14 (sockets, DNS) and 15 (TLS, sandbox); the name is
the owner's to choose. Written in-tree, from scratch.

**Deliverables:**
- **Networking:** HTTP/1.1 client with keep-alive, redirects (limited),
  chunked transfer and gzip (zlib inflate, written or ported under §0
  rule 14); HTTPS through the phase-15 TLS client; a bounded memory and
  disk cache honouring `Cache-Control`; cookies scoped by site, kept per
  user, cleared on request.
- **HTML:** a tokenizer and tree builder following the WHATWG HTML
  parsing algorithm for the common cases (implicit tags, entities,
  malformed markup recovered, never crashed on); a DOM.
- **CSS:** parser for a documented subset (selectors: type, class, id,
  descendant, child, attribute, `:hover`; properties: box model, colours,
  backgrounds, borders, fonts, text alignment, `display`
  block/inline/inline-block/none/flex basics, `position` static/relative/
  absolute); cascade and inheritance; a user-agent stylesheet.
- **Layout and painting:** block and inline formatting contexts, line
  breaking, tables (basic), images (PNG, JPEG, GIF first frame); painting
  through libgfx with damage tracking; smooth scrolling.
- **Browser UI** (Facet): address bar, back/forward/reload, tabs,
  bookmarks, history (local only, clearable), downloads through the
  trusted file dialog, find in page, zoom.
- **No JavaScript in 15B.** A script engine (write one, or port QuickJS
  under §0 rule 14) is a later owner decision; pages render without it.

**Security and privacy:**
- Runs under `restrict`: network and its own profile folder only; files
  reach it only through the trusted dialogs; each tab's renderer is a
  separate sandboxed process from the start of multi-tab work.
- Every parser (HTTP, HTML, CSS, images, gzip, URLs) is bounds-checked and
  fuzzed (`make fuzz`); a hostile page can crash a tab, never the browser
  or the system.
- HTTPS by default with certificate checking; plain HTTP is labelled "not
  secure"; no telemetry; third-party cookies off by default.

**Accept:** loads and renders `http://example.com/` and
`https://example.com/` readably; a local test suite of HTML/CSS pages
matches reference screenshots within a tolerance; 60 s of fuzzing per
parser with no crash; a page cannot read files or other tabs' data.

---

## Phase 16 — Glint native backend and retarget

See §13. Deliverables: x86-64 code generation, ELF output, the Glint standard
library, Lumen syscall bindings, and at least three applications rewritten in
Glint.

**Accept:** `glintc hello.gl -o hello` produces a Lumen ELF binary that runs on
Lumen and prints correctly. A Glint-written GUI application runs on the
desktop.

---

## Phase 17 — Software platform: running real programs

**Deliverables:**
- **Dynamic linking:** shared libraries, `/lib/ld-lumen.so`, lazy binding
  off (full RELRO: the GOT is read-only after relocation), `dlopen`.
- **libc grown to a documented POSIX subset** (listed in
  `docs/LIBC.md`) sufficient to build unmodified third-party C software.
  Proof ports: `zlib`, `lua`, `tinycc`, `make`.
  **Open owner decision:** grow the in-tree libc, or port **mlibc**
  (a portable libc with a per-OS "sysdeps" layer, used by other
  independent OSes to run large software). Porting is far less work and
  more compatible; it is a dependency and needs approval (§0 rule 14).
- **Pseudo-terminals** with line discipline, job control, the full signal
  set with `sigaction` semantics.
- **Package manager `pkg`:** package = tar + manifest (name, version,
  dependencies, files, hashes). Every package and repository index is
  **signed (Ed25519)**; unsigned or mismatching packages are refused.
  Install scripts run sandboxed. `pkg install|remove|list|search|upgrade`.
- `make bench` extended with process-spawn and dynamic-link start-up time.

**Accept:** `lua` and `tcc` built from upstream source run on Lumen; `tcc`
compiles and runs a C hello world on Lumen; `pkg install` of a tampered
package fails with a signature error; a 50-library GUI application starts
in under 200 ms.

---

## Phase 18 — Installer, updates, storage encryption, recovery

**Deliverables:**
- **Installer:** boot the ISO to a live desktop; partition (GPT), format
  (lumfs + FAT32 EFI), copy the system, install Limine, create a user,
  reboot into the installed system. FAT32 read/write and GPT parsing are
  deliverables of this phase.
- **Full-disk encryption** as an install option: AES-256-XTS (or
  XChaCha20 per sector), key derived from a passphrase with Argon2id,
  unlocked at boot before the root filesystem mounts. Verified against
  published test vectors.
- **Updates:** `pkg upgrade` with signed indexes; kernel updates keep the
  previous kernel as a boot menu entry; updates are never applied without
  the user starting them.
- **Recovery:** a boot entry that reaches a root shell with the disk
  mounted read-only; `fsck.lumfs` that repairs.
- Service manager (`init` with unit files: dependencies, restart policy,
  per-service `restrict` profile and resource limits); `svc` CLI; log
  capture with a viewer.

**Accept:** install to a blank QEMU disk and boot from it without the ISO;
the encrypted install shows only ciphertext when the disk image is searched
on the host for a known file's contents; an interrupted update leaves a
bootable system; recovery mode repairs a deliberately damaged lumfs.

---

## Phase 19 — Stretch goals (only after 18)

USB (XHCI, HID, mass storage), audio (Intel HDA, mixing server), IPv6,
KASLR, real-hardware boot, self-hosting the Glint compiler, a Glint package
ecosystem, HiDPI scaling, accessibility features beyond §10, ARM64 port.

---

# 6. Kernel design details

## 6.1 Memory layout

```
0x0000_0000_0000_0000 – 0x0000_7FFF_FFFF_FFFF   userland (128 TB)
  0x0000_0000_0040_0000                          program base
  grows up                                       heap (brk / mmap)
  0x0000_7FFF_FFFF_0000                          stack top, grows down
0xFFFF_8000_0000_0000 – 0xFFFF_BFFF_FFFF_FFFF   HHDM (direct physical map)
0xFFFF_C000_0000_0000 – 0xFFFF_FFFF_7FFF_FFFF   kernel heap / vmalloc
0xFFFF_FFFF_8000_0000 – 0xFFFF_FFFF_FFFF_FFFF   kernel image
```

## 6.2 Hybrid kernel boundary

In-kernel: memory management, scheduling, syscalls, VFS core, block drivers,
input drivers, IPC.

Userland: window server, filesystem drivers beyond lumfs/tmpfs, network stack
(when it exists), all applications.

The rule: anything that needs sub-microsecond latency or privileged
instructions is in the kernel; everything else is a userland server reached
through ports.

## 6.3 Error handling

```cpp
template <typename T> class Result {
    // holds either T or Error; no exceptions, no allocation
    bool ok() const; T& value(); Error error();
};
enum class Error { None, NoMemory, NotFound, Exists, Invalid, Perm, Busy,
                   Fault, NoSpace, IsDir, NotDir, Again, Interrupted, IO,
                   NotSupported, TooBig, Deadlock, Timeout };
```

Every fallible kernel function returns `Result<T>`. Errors map to negative
errno values at the syscall boundary.

---

# 7. Syscall ABI

## Convention

`syscall` instruction. Number in `RAX`. Arguments in `RDI, RSI, RDX, R10, R8,
R9`. Return value in `RAX` (negative = `-errno`). `RCX` and `R11` clobbered by
the CPU. All other registers preserved by the kernel.

## Table

Defined in `kernel/syscall/table.def`:

```
SYSCALL(0,   exit,        (int status))
SYSCALL(1,   write,       (int fd, const void* buf, size_t n))
SYSCALL(2,   read,        (int fd, void* buf, size_t n))
SYSCALL(3,   open,        (const char* path, int flags, int mode))
SYSCALL(4,   close,       (int fd))
SYSCALL(5,   seek,        (int fd, int64_t off, int whence))
SYSCALL(6,   stat,        (const char* path, struct stat* out))
SYSCALL(7,   fstat,       (int fd, struct stat* out))
SYSCALL(8,   mkdir,       (const char* path, int mode))
SYSCALL(9,   unlink,      (const char* path))
SYSCALL(10,  rename,      (const char* from, const char* to))
SYSCALL(11,  readdir,     (int fd, struct dirent* out, size_t n))
SYSCALL(12,  chdir,       (const char* path))
SYSCALL(13,  getcwd,      (char* buf, size_t n))
SYSCALL(14,  dup,         (int fd))
SYSCALL(15,  dup2,        (int old, int neu))
SYSCALL(16,  pipe,        (int fds[2]))
SYSCALL(17,  ioctl,       (int fd, unsigned req, void* arg))
SYSCALL(18,  truncate,    (int fd, int64_t size))
SYSCALL(19,  sync,        (void))

SYSCALL(20,  mmap,        (void* hint, size_t len, int prot, int flags, int fd, int64_t off))
SYSCALL(21,  munmap,      (void* addr, size_t len))
SYSCALL(22,  mprotect,    (void* addr, size_t len, int prot))
SYSCALL(23,  brk,         (void* addr))

SYSCALL(30,  fork,        (void))
SYSCALL(31,  execve,      (const char* path, char* const argv[], char* const envp[]))
SYSCALL(32,  waitpid,     (int pid, int* status, int flags))
SYSCALL(33,  getpid,      (void))
SYSCALL(34,  getppid,     (void))
SYSCALL(35,  kill,        (int pid, int sig))
SYSCALL(36,  signal,      (int sig, void* handler))
SYSCALL(37,  sigreturn,   (void))
SYSCALL(38,  yield,       (void))
SYSCALL(39,  sleep_ms,    (uint64_t ms))
SYSCALL(40,  thread_spawn,(void* entry, void* arg, void* stack))
SYSCALL(41,  thread_exit, (int code))
SYSCALL(42,  thread_join, (int tid, int* code))
SYSCALL(43,  futex_wait,  (uint32_t* addr, uint32_t val, uint64_t timeout_ms))
SYSCALL(44,  futex_wake,  (uint32_t* addr, int count))

SYSCALL(50,  port_create, (const char* name))
SYSCALL(51,  port_connect,(const char* name))
SYSCALL(52,  port_send,   (int port, const void* msg, size_t len, const int* fds, int nfds))
SYSCALL(53,  port_recv,   (int port, void* buf, size_t len, int* fds, int* nfds, uint64_t timeout_ms))
SYSCALL(54,  port_close,  (int port))
SYSCALL(55,  shm_create,  (size_t size))
SYSCALL(56,  shm_map,     (int handle, int prot))
SYSCALL(57,  shm_unmap,   (void* addr))

SYSCALL(60,  time_ms,     (void))
SYSCALL(61,  time_date,   (struct datetime* out))
SYSCALL(62,  uname,       (struct utsname* out))
SYSCALL(63,  sysinfo,     (struct sysinfo* out))
SYSCALL(64,  reboot,      (int cmd))
SYSCALL(65,  log,         (int level, const char* msg))
SYSCALL(66,  getrandom,   (void* buf, size_t n, unsigned flags))

// v2: networking (phase 14)
SYSCALL(70,  socket,      (int domain, int type, int protocol))
SYSCALL(71,  bind,        (int fd, const struct sockaddr* addr, size_t len))
SYSCALL(72,  connect,     (int fd, const struct sockaddr* addr, size_t len))
SYSCALL(73,  listen,      (int fd, int backlog))
SYSCALL(74,  accept,      (int fd, struct sockaddr* addr, size_t* len, int flags))
SYSCALL(75,  send,        (int fd, const void* buf, size_t n, int flags))
SYSCALL(76,  recv,        (int fd, void* buf, size_t n, int flags))
SYSCALL(77,  sendto,      (int fd, const void* buf, size_t n, int flags, const struct sockaddr* addr, size_t len))
SYSCALL(78,  recvfrom,    (int fd, void* buf, size_t n, int flags, struct sockaddr* addr, size_t* len))
SYSCALL(79,  shutdown,    (int fd, int how))
SYSCALL(80,  getsockopt,  (int fd, int level, int opt, void* val, size_t* len))
SYSCALL(81,  setsockopt,  (int fd, int level, int opt, const void* val, size_t len))
SYSCALL(82,  getsockname, (int fd, struct sockaddr* addr, size_t* len))
SYSCALL(83,  getpeername, (int fd, struct sockaddr* addr, size_t* len))

// v2: event multiplexing (phase 11)
SYSCALL(90,  event_create,(int flags))
SYSCALL(91,  event_ctl,   (int ev, int op, int fd, const struct event* e))
SYSCALL(92,  event_wait,  (int ev, struct event* out, int max, uint64_t timeout_ms))

// v2: credentials and sandboxing (phases 9 and 15)
SYSCALL(100, getuid,      (void))
SYSCALL(101, geteuid,     (void))
SYSCALL(102, getgid,      (void))
SYSCALL(103, getegid,     (void))
SYSCALL(104, setuid,      (int uid))
SYSCALL(105, setgid,      (int gid))
SYSCALL(106, getgroups,   (int* list, int n))
SYSCALL(107, setgroups,   (const int* list, int n))
SYSCALL(108, chmod,       (const char* path, int mode))
SYSCALL(109, chown,       (const char* path, int uid, int gid))
SYSCALL(110, restrict,    (uint64_t flags, const char* const paths[]))
SYSCALL(111, umask,       (int mask))

// v2: files, extended (phase 9)
SYSCALL(120, fsync,       (int fd))
SYSCALL(121, openat,      (int dirfd, const char* path, int flags, int mode))
SYSCALL(122, symlink,     (const char* target, const char* path))
SYSCALL(123, readlink,    (const char* path, char* buf, size_t n))
SYSCALL(124, link,        (const char* from, const char* to))
SYSCALL(125, fcntl,       (int fd, int cmd, uint64_t arg))
```

Keep the numbers stable. Add new calls at the end of their block. `setuid`,
`setgid` and `setgroups` succeed only for uid 0 and only ever drop privilege
for the calling process; there is no way to gain privilege by executing a
file (§19.4).

---

# 8. Graphics stack

## 8.1 Pixel format

32-bit BGRA, 8 bits per channel, premultiplied alpha internally. The
framebuffer from Limine is assumed to be 32bpp; if it isn't, fail loudly at
boot rather than half-working.

## 8.2 Software renderer (`libgfx`, used by Pane and Facet)

Required primitives, all clipped to a rect:

- `fill_rect`, `fill_rect_rounded(radius)`, `stroke_rect`
- `blit` (copy), `blit_alpha` (source-over blend), `blit_scaled` (nearest and
  bilinear)
- `draw_line` (Bresenham + a Wu antialiased variant), `draw_circle`,
  `fill_circle`, `fill_triangle`
- `fill_gradient_linear`, `fill_gradient_radial`
- `blur_box(radius)` for shadows and translucency
- Text: `draw_text(font, x, y, str, colour)` with kerning-free advance,
  `measure_text`, multi-line layout with wrapping, and ellipsis truncation
- Clip stack: `push_clip(rect)` / `pop_clip`
- Alpha compositing is done once per frame in the compositor, not per widget

## 8.3 Fonts

- Phase 12: bitmap PSF fonts at 8×16 and 16×32.
- Phase 13: a minimal TrueType rasteriser — parse `glyf`, `loca`, `cmap`,
  `head`, `hhea`, `hmtx`; render quadratic Béziers with a scanline fill and
  4×4 supersampling; cache rendered glyphs in an LRU atlas keyed by
  `(font, size, codepoint)`.
- Ship three fonts: a UI sans, a serif, and a monospace.

---

# 9. Pane — window server protocol

Transport: a port named `pane`. Requests are fixed-size structs with a variable
tail; every message begins with `{ uint32 type; uint32 length; uint32 serial; }`.

## Client → server

| Message | Payload |
|---|---|
| `Connect` | client name, protocol version |
| `CreateWindow` | width, height, flags (decorated/resizable/modal/tooltip/popup), parent id, title |
| `DestroyWindow` | window id |
| `AttachBuffer` | window id, shm handle, stride, format |
| `Damage` | window id, rect |
| `Commit` | window id (present the attached buffer) |
| `SetTitle` | window id, utf8 string |
| `SetGeometry` | window id, x, y, w, h |
| `SetMinSize` / `SetMaxSize` | window id, w, h |
| `Raise` / `Lower` | window id |
| `Minimise` / `Maximise` / `Restore` / `Fullscreen` | window id |
| `SetCursor` | cursor id or custom shm bitmap + hotspot |
| `GrabPointer` / `ReleaseGrab` | window id |
| `SetOpacity` | window id, 0–255 |
| `RequestClipboard` / `SetClipboard` | mime type, data |
| `StartDrag` | window id, payload mime + data |

## Server → client

| Message | Payload |
|---|---|
| `Connected` | client id, screen w/h, dpi scale |
| `WindowCreated` | window id |
| `Configure` | window id, new x/y/w/h, state flags (client must redraw) |
| `KeyEvent` | window id, keycode, unicode codepoint, modifiers, pressed/released, repeat |
| `PointerMotion` | window id, x, y (window-relative), global x, y |
| `PointerButton` | window id, button, pressed, x, y, click count |
| `PointerScroll` | window id, dx, dy |
| `PointerEnter` / `PointerLeave` | window id |
| `FocusIn` / `FocusOut` | window id |
| `CloseRequest` | window id |
| `FrameCallback` | window id (safe to draw the next frame — vsync-ish pacing) |
| `ClipboardData` | mime type, data |
| `DropEvent` | window id, x, y, mime, data |

## Compositor internals

- Window list sorted by stacking order; a separate always-on-top layer for the
  panel, menus, and tooltips.
- Damage accumulation per frame: union of all client damage plus any region
  exposed by window movement. Only the union is recomposited.
- Frame pacing at 60 Hz driven by a timer; if no damage, no work.
- Server-side decorations: title bar with title text, close/minimise/maximise
  buttons, 1px border, 8px resize grip regions on all edges and corners.
- Drop shadow: a blurred black rounded rect offset (0, 4) with radius 12,
  rendered under each window, cached and reused unless the window resizes.
- Alt+Tab window switcher rendered by the compositor itself.
- Workspaces: 4 virtual desktops, switched with Ctrl+Alt+1..4, with a slide
  transition.

---

# 10. Facet — GUI toolkit

C++ first. Ported to Glint in phase 16.

## Architecture

- Retained-mode widget tree. A `Widget` has: parent, children, bounds, layout
  constraints, visibility, enabled state, and an event handler table.
- Layout is constraint-based and computed in two passes: `measure(available)`
  returns a preferred size; `arrange(final_rect)` assigns positions.
- Repaint is damage-driven: a widget calls `invalidate()`, the toolkit unions
  dirty rects and issues one `Damage` + `Commit` per frame.
- Event dispatch: hit-test from the root down, then bubble up. Handlers return
  whether they consumed the event.

## Layout containers

- `VBox` / `HBox` — with per-child weight, spacing, and alignment
- `Grid` — row/column with span, per-track sizing (fixed, auto, fraction)
- `Stack` — z-ordered overlay
- `ScrollView` — vertical/horizontal, with scrollbars that auto-hide
- `Splitter` — draggable divider, two panes, min sizes
- `TabView` — tab strip plus content area
- `Flow` — wrapping horizontal layout
- `Padding`, `Center`, `Spacer`, `Separator`

## Widgets

**Input:** `Button` (normal/toggle/icon/flat), `CheckBox`, `RadioButton` with
groups, `Slider` (h/v, ticks, snap), `SpinBox`, `TextField` (single line, with
selection, cursor, undo/redo, clipboard, placeholder, password mode, input
validation), `TextArea` (multi-line, word wrap, line numbers optional),
`ComboBox` (dropdown, editable variant), `ColorPicker`, `DatePicker`.

**Display:** `Label` (with alignment, wrapping, ellipsis), `Image`,
`ProgressBar` (determinate + indeterminate), `Spinner`, `Icon`, `Badge`,
`Tooltip` (hover-delayed, auto-positioned).

**Collections:** `ListView` (virtualised, single/multi select, custom item
renderer), `TreeView` (expand/collapse, indent guides, lazy children),
`TableView` (sortable columns, resizable headers, row selection, alternating
row colours).

**Containers/chrome:** `Window`, `Dialog` (modal, with a standard button row),
`MenuBar`, `Menu` / `MenuItem` (with submenus, separators, checkable items,
keyboard accelerators, icons), `ContextMenu`, `Toolbar`, `StatusBar`,
`Notification` (toast, auto-dismiss).

**Standard dialogs:** `FileOpenDialog`, `FileSaveDialog`, `MessageBox`
(info/warning/error/question), `InputDialog`, `ColorDialog`, `AboutDialog`,
`ProgressDialog`.

## Theming

A single `Theme` struct loaded from `/etc/facet/theme.conf` (INI-style):
colours (background, surface, surface-variant, primary, on-primary, text,
text-muted, border, focus ring, error, warning, success), corner radius,
spacing unit, font families and sizes, shadow parameters, animation durations.

Ship two themes: **Lumen Dark** (default) and **Lumen Light**. Changing the
theme at runtime repaints every window.

## Accessibility and polish requirements

- Full keyboard navigation: Tab/Shift-Tab focus order, Enter/Space activation,
  arrow keys within lists and menus, Escape to close.
- A visible focus ring on the focused widget.
- Every interactive widget has hover, active, focused, and disabled visuals.
- Animations: 150 ms ease-out for hover and press, 200 ms for menu and dialog
  open. Implemented as an animation driver on the frame callback.

---

# 11. Desktop shell

`userland/shell/` — the first GUI process started by init.

## Components

**Panel** — a 32px bar. Default position bottom, configurable to top.
Contains:
- Launcher button (opens the application menu)
- Task list: one button per open window, showing icon + truncated title,
  highlighted when focused, click to focus/minimise, right-click for a window
  menu
- Workspace switcher: 4 indicators, click to switch
- System tray area
- Clock: time and date, click for a calendar popup
- Resource indicator: CPU and RAM percentage, click opens System Monitor
- Power button: menu with Shutdown / Reboot / Log out

**Application menu** — categorised list (Accessories, Graphics, System,
Development), with a search field that filters by name as you type. Entries
come from `.desktop`-style files in `/usr/share/applications/`
(`Name`, `Exec`, `Icon`, `Category`, `Comment`).

**Desktop** — wallpaper (solid colour, gradient, or image with
centre/tile/stretch/fit modes), desktop icons for files in `~/Desktop`, a
right-click context menu (New Folder, New File, Change Wallpaper, Open
Terminal Here, Arrange Icons).

**Session manager** — starts Pane, then the panel, then autostart entries;
restarts crashed components; handles the shutdown sequence (notify apps, wait,
kill, sync, reboot syscall).

**Global hotkeys:** Alt+Tab (switch), Alt+F4 (close), Super (open menu),
Super+E (file manager), Super+T (terminal), Super+L (lock), Ctrl+Alt+1..4
(workspace), PrintScreen (screenshot).

---

# 12. Applications

Each application lives in `userland/apps/<name>/`, links Facet, and ships a
`.desktop` entry and an icon.

## 12.1 Ember — terminal emulator

- Full VT100/xterm subset: cursor movement, erase, scroll regions, SGR colours
  (16, 256, and 24-bit truecolour), alternate screen buffer, bracketed paste.
- Scrollback buffer (10,000 lines default, configurable), scrollbar, mouse
  wheel and Shift+PgUp/PgDn.
- Selection with the mouse (character, word on double-click, line on
  triple-click), copy/paste with Ctrl+Shift+C/V.
- Configurable font, size, colour scheme, cursor style (block/bar/underline,
  blinking or not), transparency.
- Tabs, with Ctrl+Shift+T to open and Ctrl+PgUp/PgDn to switch.
- Runs `lsh` (§12.11) as its child over a pty.

## 12.2 Crate — file manager

- Dual-pane and single-pane modes.
- Views: icons (small/medium/large), list, detail (name, size, type, modified).
- Sidebar with bookmarks (Home, Desktop, Documents, Downloads, Root, mounted
  volumes) and drag-to-bookmark.
- Navigation: back/forward/up, breadcrumb path bar that becomes editable on
  click, address entry, history.
- Operations: copy, cut, paste, delete (with a confirmation), rename (inline
  edit), new folder, new file, duplicate. All long operations run on a worker
  thread with a progress dialog and a cancel button.
- Drag and drop within and between panes, and to/from the desktop.
- Search: recursive filename filter with live results.
- Properties dialog: size (recursive for folders, computed in the background),
  permissions, timestamps, type.
- Open-with: double-click dispatches by extension via `/etc/mime.conf`;
  right-click offers a chooser.
- Hidden file toggle (Ctrl+H), sort by name/size/type/date, thumbnail previews
  for images.

## 12.3 Slate — text editor

- Multiple documents in tabs, with an unsaved-changes indicator.
- Line numbers, current-line highlight, configurable tab width, spaces-vs-tabs.
- Syntax highlighting driven by declarative rule files in
  `/usr/share/slate/syntax/*.conf` (keyword lists, string/comment/number
  regex-lite patterns). Ship definitions for Glint, C/C++, shell, INI,
  Markdown.
- Find and replace: literal and simple wildcard, case sensitivity, whole word,
  find-all with a results list, replace-all with an undo point.
- Undo/redo with grouped edits, unlimited depth.
- Multi-cursor: Ctrl+click to add a cursor, Ctrl+D to add the next occurrence.
- Auto-indent, bracket matching, bracket auto-close, comment toggle (Ctrl+/).
- Go to line (Ctrl+G), word wrap toggle, zoom in/out.
- Session restore: reopen the files that were open at last exit.

## 12.4 Gauge — system monitor

- Overview tab: CPU usage graph per core (scrolling line chart, 60 s window),
  memory usage (used/cached/free stacked area), disk I/O throughput.
- Processes tab: table of PID, name, state, CPU %, memory, threads, uptime;
  sortable, filterable, with kill/terminate from a context menu and a
  confirmation dialog.
- Filesystem tab: mounted volumes, total/used/free, a usage bar.
- Devices tab: PCI device list with vendor/device names resolved from a small
  embedded ID table.
- Auto-refresh at 1 Hz, pausable.

## 12.5 Dial — settings

Category sidebar plus a content pane. Categories:
- **Appearance**: theme (dark/light), accent colour, wallpaper, font and size,
  corner radius, animation on/off.
- **Display**: resolution (if multiple are available), scale factor.
- **Keyboard**: layout, key repeat delay and rate, hotkey list with rebinding.
- **Mouse**: pointer speed, double-click interval, left-handed swap, scroll
  direction.
- **Date & Time**: set the clock, 12/24-hour format, date format.
- **Startup**: manage autostart entries.
- **About**: OS version, kernel build, CPU, total RAM, uptime, a logo.

All settings persist to `/etc/lumen/*.conf` and take effect immediately via a
broadcast on a `settings` port.

## 12.6 Tally — calculator

Basic and scientific modes. Keyboard entry, full history list (click an entry
to recall it), memory keys (MS/MR/M+/M-), unit conversion tab
(length/mass/temperature/data), programmer tab (hex/dec/oct/bin with bitwise
operations and a bit-toggle display).

## 12.7 Frame — image viewer

Open PNG (write the decoder: zlib inflate + all five filter types + interlace
handling), BMP, TGA, and PPM. Zoom (fit, 1:1, arbitrary with Ctrl+scroll),
pan by dragging, rotate 90°, flip, next/previous within the folder, slideshow,
thumbnail strip, basic info overlay (dimensions, format, file size).

## 12.8 Daub — paint

Canvas with a size chooser. Tools: pencil, brush (size and hardness), eraser,
line, rectangle, ellipse, filled variants, flood fill, colour picker
(eyedropper), text, rectangular select with move/cut/copy/paste. Colour palette
plus a custom colour dialog. Undo/redo. Layers (add, delete, reorder, toggle
visibility, opacity). Save as PNG (write the encoder too) and BMP.

## 12.9 Chronos — clock

Clock face (analogue and digital), world clocks for several zones, stopwatch
with laps, countdown timer with a notification on completion, alarms.

## 12.10 Warren — text adventure / demo game

A small game proving the toolkit can drive something non-trivial: a tile-based
map, keyboard movement, an inventory panel, dialogue boxes, save/load to disk.
Written in **Glint** once phase 16 lands — this is the flagship demonstration
that the language works.

## 12.11 lsh — the shell (CLI)

- Command parsing with quoting, escapes, and comments.
- Pipes `|`, redirection `> >> < 2>`, background `&`, sequencing `; && ||`.
- Globbing `* ? [abc]`.
- Variables, `export`, environment inheritance, `$VAR` and `${VAR}` expansion,
  command substitution `$(...)`.
- Builtins: `cd pwd echo export unset alias exit jobs fg bg kill history
  source test true false type`.
- Line editing: history with up/down and Ctrl+R search, Tab completion for
  commands, paths, and variables, Ctrl+A/E/K/U/W.
- Scripts: `if/then/elif/else/fi`, `for/in/do/done`, `while`, `case`,
  functions, `$1..$9`, `$?`, `$#`.
- `~/.lshrc` sourced at startup.

## 12.12 Core CLI utilities

`ls cat cp mv rm mkdir rmdir touch ln stat pwd echo head tail wc grep find
sort uniq cut tr sed-lite du df mount umount ps kill top free uname date
sleep clear hexdump diff tar sync reboot poweroff mkfs.lumfs fsck.lumfs`

Each: proper argument parsing, `--help`, sensible exit codes, errors to stderr.

---

# 13. Glint language

A working tree-walking implementation may already exist (lexer, parser,
unification-based checker, interpreter, `Option`/`Result`, exhaustive match,
generics, `List[T]`). If it does, extend it. If not, build it first to that
level, then continue.

## 13.1 Language features to add

**Closures and first-class functions.** Function type `fn(A, B) -> C` as a
value. Lambda syntax `|x, y| expr`. Capture by value; capture analysis at
compile time. Requires environment capture in the runtime representation.

**Let-generalisation.** `let f = identity;` generalises so `f` is usable at
multiple types.

**Nested-pattern exhaustiveness.** Implement Maranget's usefulness algorithm so
`match t { Num(0) => .., Word(w) => .. }` is correctly rejected.

**Traits (bounded polymorphism).** `trait Show { fn show(self) -> str }`,
`impl Show for Point { ... }`, `fn dump[T: Show](v: T)`. Monomorphised at
compile time — no vtables unless explicitly `dyn`.

**Modules.** `mod foo;` maps to `foo.gl`. `use foo::bar;`. `pub` visibility.

**Struct field assignment and mutable references.** `p.x = 1;`, `&mut T`, with
a simple aliasing rule: only one `&mut` live at a time, checked by a
lightweight borrow pass (not full Rust NLL — scope-based is enough).

**Regions/arenas as a language feature.** `region r { let x = r.alloc(...); }`
— everything allocated in `r` is freed at the closing brace, and the checker
prevents a region-allocated reference from escaping its region. *This is the
distinctive feature.* It gives deterministic memory management without a GC and
without full ownership tracking, which suits kernel and driver code.

**Compile-time evaluation.** `const fn` evaluated at compile time; `static
assert`.

**Syscalls as typed constructs.** `syscall write(fd: Fd, buf: &[u8]) -> Result[usize, Errno]`
declared in the standard library and lowered directly to the `syscall`
instruction with the correct register assignment — no C shim.

**Capability types.** Kernel handles (`Fd`, `ShmHandle`, `Port`, `WindowId`)
are distinct non-copyable types. A `Fd` cannot be duplicated implicitly or used
after `close`. The checker enforces linear use.

**MMIO layout types.** 
```
mmio struct AhciPort {
    0x00: clb: u32 rw,
    0x04: clbu: u32 rw,
    0x10: is: u32 rw1c,
    0x14: ie: u32 rw,
}
```
The compiler generates volatile accessors with the right width and rejects a
write to a read-only register at compile time.

**Inline assembly.** `asm("...", in(reg) x, out(reg) y, clobbers("memory"))`.

**Arrays and slices.** `[T; N]` fixed arrays, `&[T]` slices with a length,
bounds-checked indexing (with an `unchecked` escape hatch).

## 13.2 Compiler pipeline

```
source → lexer → parser → AST
       → name resolution (modules, imports, scopes)
       → type check (unification, traits, regions, capabilities)
       → exhaustiveness + borrow checks
       → monomorphisation (generics and trait impls)
       → lowering to GIR (SSA-form mid-level IR)
       → optimisation passes
       → register allocation
       → x86-64 instruction selection
       → ELF64 object emission
       → link
```

**GIR** — a simple SSA IR: basic blocks, phi nodes, typed values, instructions
(`add sub mul div load store gep call br cond_br ret cmp cast alloca
extract_value insert_value`).

**Optimisation passes** (in order, each independently testable):
constant folding, dead code elimination, common subexpression elimination,
copy propagation, mem2reg (promote `alloca` to SSA values), inlining of small
functions, tail call elimination, basic loop-invariant code motion.

**Register allocation**: linear scan over SSA live intervals with spilling.
Callee-saved: `RBX RBP R12-R15`. Caller-saved: `RAX RCX RDX RSI RDI R8-R11`.
System V argument order for compatibility with C where needed.

**ELF emission**: write the object file yourself — ELF header, `.text`,
`.rodata`, `.data`, `.bss`, symbol table, relocations. Then a minimal linker,
or shell out to `ld` initially and replace it later.

## 13.3 Glint standard library (`glint/lib/`)

`core` (Option, Result, panic, assert), `mem` (alloc, free, copy, set,
regions), `str` (UTF-8 aware string operations, formatting, parsing),
`list` (growable vector), `map` (hash map), `set`, `io` (file handles, stdin/
stdout/stderr, buffered readers and writers), `fs` (path manipulation,
directory iteration, metadata), `proc` (spawn, wait, exit, args, env),
`sync` (Mutex, Channel on top of futex), `time`, `math`, `sort` (introsort +
stable merge), `json` (parse and serialise), `gui` (Facet bindings),
`sys` (raw Lumen syscalls).

## 13.4 Tooling

- `glintc` — the compiler. Flags: `-o`, `-O0..2`, `-g`, `--emit=ast|ir|asm|obj`,
  `--target=lumen|linux`, `-W` warnings, `--explain <error-code>`.
- `glintfmt` — canonical formatter.
- Error messages must be excellent: file:line:col, the source line with a caret
  span, the primary message, a secondary note explaining the rule, and a
  suggested fix where one is obvious.

---

# 14. Testing

## Kernel unit tests
`tests/kernel/` compiled into the kernel in test builds. A kernel shell command
`test <name>` or `test all` runs them and prints pass/fail counts. Required
suites: `pmm vmm heap sched sync vfs lumfs elf syscall ipc smp`.

## Integration tests
`tests/integration/` — each test is a `.expect` file plus a scenario. The
harness boots QEMU with `-display none -serial stdio`, feeds scripted input,
captures output, compares to expected, and enforces a 60-second timeout.
Must cover: boot to shell, file round-trip, process lifecycle, GUI startup,
window creation and destruction.

## Glint tests
- `tests/glint/run/*.gl` — must compile, run, exit 0, match `.expect` output.
- `tests/glint/fail/*.gl` — must fail to compile with an error containing the
  string in the matching `.expect`.
- Property test: random valid programs generated by a small fuzzer must not
  crash the compiler.

## CI
`make test` must exit nonzero on any failure. Run it before every commit that
touches the kernel or compiler.

---

# 15. Debugging playbook

Use this rather than guessing.

**Triple fault (QEMU resets in a loop):** run with `-d int,cpu_reset
-no-reboot`. The last exception before the reset is the culprit. Usually a bad
IDT entry, a bad GDT, or a stack that isn't mapped.

**Page fault:** the handler already prints CR2 and the flags. Cross-reference
CR2 against the memory map in §6.1 to find which region was touched.

**It hangs:** attach GDB (`make debug` then `make gdb`), `Ctrl+C`, `bt`, `info
registers`. If it's in a spinlock, you have a deadlock — check lock ordering.

**Wrong values after a context switch:** you're not saving or restoring a
register. Diff your assembly against the System V callee-saved list.

**Works with 1 CPU, breaks with 4:** missing lock, or a lock that doesn't
disable interrupts where it must. Run with `-smp 1` to confirm, then audit
every shared structure touched on that path.

**Userland crashes immediately:** check the ELF program headers actually got
mapped with the right permissions, that the stack is mapped and 16-byte
aligned at entry, and that `argv`/`envp`/`auxv` are laid out correctly.

**Compositor tearing:** you're drawing to the front buffer. Confirm the swap
happens only after a full composite.

Standard tools: `-d guest_errors`, `-d int`, `-monitor stdio` then `info mem`,
`info registers`, `info tlb`, `x/20i $rip`.

---

# 16. Definition of done

The project is complete when, in QEMU, from a cold boot:

1. Limine loads the kernel; the kernel initialises and prints a clean boot log.
2. The system reaches a graphical desktop with a wallpaper and a panel.
3. The launcher opens; applications start from it.
4. The file manager browses a real lumfs filesystem on a virtual disk.
5. A file is created in the editor, saved, and survives a reboot.
6. The terminal runs the shell, which runs the CLI utilities, including pipes
   and redirection.
7. Windows can be moved, resized, stacked, minimised, and closed with the mouse.
8. `glintc` compiles a Glint source file on Lumen into a Lumen binary that runs.
9. At least one GUI application on the desktop is written in Glint.
10. `make test` passes.
11. `docs/STATUS.md` reflects reality and `docs/DECISIONS.md` explains why the
    system looks the way it does.
12. The system is installed to a virtual disk by its own installer and boots
    from that disk, with a login screen and at least two user accounts that
    cannot read each other's files.
13. The machine gets an address by DHCP, resolves names, and fetches a page
    over HTTPS; a capture of the virtual NIC shows no traffic the user did
    not initiate.
14. A third-party C program built from unmodified upstream source (Lua) runs.
15. Every item in the §19.12 security checklist is demonstrated, `make fuzz`
    passes, and every budget in §20.1 is met in `docs/BENCH.md`.

---

# 17. Explicit non-goals

Do not build these, and do not spend time discussing them:

- Full POSIX compliance or certification. v2 does require a documented
  POSIX *subset* large enough to port real C software (phase 17).
- Binary compatibility with Linux or anything else.
- (Removed 2026-10-04: the owner now wants our own web browser; see
  phase 15B.)
- Backwards compatibility with anything.
- Inventing cryptography. Only published, standard algorithms, verified
  against official test vectors (§19.9).
- Telemetry, analytics, advertising identifiers, or any background
  connection the user did not ask for. Permanently.
- Tools for attacking other people's machines or networks (password
  cracking against others' accounts, Wi-Fi intrusion). Diagnostics for the
  user's own machines and networks are fine.
- Secure Boot signing, TPM-based attestation, GPU acceleration, Bluetooth,
  printing, webcams: out of scope until after phase 19.
- Micro-optimisation without a measurement. v1 said "no performance work
  before phase 14"; v2 replaces that with §20: design each subsystem to its
  budget from the start, and change code for speed only when `make bench`
  shows a budget is missed. Correctness and security still come first.

*Removed in v2 (now requirements):* security hardening (was "no KASLR, no
SMEP/SMAP, no seccomp" — see §19; KASLR itself is phase 19), and multi-user
accounts, permissions and authentication (phases 9 and 15).

---

# 18. Session start checklist

At the beginning of every session, before writing code:

1. Read `docs/STATUS.md`.
2. State the current phase and what the last session finished.
3. Run `make test` and `make run`; confirm the system still boots and report
   the actual output.
4. State the specific goal for this session and its acceptance criterion.
5. Work. Commit in small pieces.
6. Before ending: run `make test`, update `docs/STATUS.md` and, if any design
   decision was made, `docs/DECISIONS.md`.

v2 additions (details in §21 and §22):

- At step 1, also read `docs/TO_FINISH.md` and the newest entries of
  `docs/DECISIONS.md`.
- At step 4, name the §5A rows and the §19/§20 rules the session's work
  touches.
- At step 6, also run `make bench` (from phase 6) and `make fuzz` (from
  phase 7) when the session touched a measured or hostile-input path, update
  `docs/TO_FINISH.md`, and update `docs/BENCH.md` if numbers changed.

---

# 19. Security and privacy

This section overrides convenience everywhere else in the document. The
person at the keyboard must be able to trust that the OS keeps their data
theirs.

## 19.1 Principles

1. **Least privilege.** Every process, service and driver gets only the
   access it needs. The default answer to "may this code do X" is no.
2. **Deny by default.** Inbound network connections, device access, screen
   capture, and access to other users' files are refused unless explicitly
   granted.
3. **Validate at every boundary.** Userland → kernel (syscalls), device →
   driver, network → stack, disk → filesystem, client → server (ports),
   file → parser. Data from the far side is hostile until checked.
4. **Fail closed.** On any error in a security check, deny. Never fall back
   to "allow" because a lookup failed.
5. **Defence in depth.** No single mechanism is trusted alone: isolation,
   plus W^X, plus ASLR, plus canaries, plus sandboxing.
6. **Small trusted base.** Code that runs in ring 0 or as root is kept as
   small as possible; that is the reason for the hybrid-kernel rule (§6.2)
   and for moving the compositor out of the kernel at phase 12.
7. **Privacy by default.** Nothing leaves the machine, and no application
   observes the user (keys, screen, clipboard, files, location on the
   network), without the user asking for it.
8. **Honest state.** The UI always shows the truth: whether the disk is
   encrypted, whether a connection is encrypted, which application holds
   which permission.

## 19.2 Threat model

In scope — the OS must defend against:
- A malicious or buggy **unprivileged program** trying to read other
  processes' memory, other users' files, the keyboard, the screen, or to
  crash or take over the kernel.
- **Hostile data**: a crafted file, disk image, font, image, archive, or
  network packet.
- A **network attacker** on the same LAN or on the path: spoofing,
  injection, scanning, malformed packets, a forged server.
- **Another local user** of the same machine.
- A **stolen or copied disk** (when the user chose encryption at install).

Out of scope for now (stated honestly, not ignored): physical attacks on a
running machine, malicious hardware and firmware, side channels beyond the
basic Spectre/Meltdown measures in §19.3, and a compromised build host.

## 19.3 Kernel protections

| Mechanism | Rule | Phase |
|---|---|---|
| Ring separation | Only the kernel runs in ring 0; from phase 12 no GUI code does | 7, 12 |
| Address-space isolation | One PML4 per process; user pages never shared unless via shm | 4, 7 |
| NX + W^X | No page is ever writable and executable, kernel or user | 4 |
| Kernel image permissions | text RX, rodata R, data RW+NX | 4 |
| Guard pages | Below every kernel and user stack | 4, 6 |
| Page zeroing | Frames are zeroed before reuse across a trust boundary | 4 |
| SMEP / SMAP / UMIP | Enabled when present; user access only via usercopy | 7 |
| User-pointer validation | Range, overflow, mapping, permission; fault-safe copy | 7 |
| Stack canaries | Userland always; kernel after the CSPRNG is up | 7 |
| Compiler hardening, kernel | Zero-initialised locals, UBSAN in debug builds; later `-fzero-call-used-regs` | 4 |
| Compiler/linker hardening, userland | PIE, full RELRO, `-fstack-clash-protection`, `-z separate-code`, `-z noexecstack` | 7 |
| ASLR | Stack, mmap, PIE base, from the CSPRNG | 7 |
| Heap hardening | Encoded free-list pointers, red zones, poison | 5 |
| CSPRNG | Single kernel generator; no other randomness source is used | 7 |
| Info-leak hygiene | No kernel pointers or uninitialised bytes cross to userland | 7 |
| Speculation | `lfence`/masking after the syscall-number bounds check and in usercopy bounds checks; kernel compiled with retpolines if the CPU needs them; Meltdown-affected CPUs detected and reported (KPTI is phase 19) | 7 |
| Lock-rank checking | Debug builds panic on lock-order inversion | 8 |
| KASLR | Later: requires a relocatable kernel | 19 |

Rules for kernel code:
- Never dereference a user pointer. Use `copy_from_user`, `copy_to_user`,
  `strncpy_from_user`. Copy once, then validate the copy (no double fetch).
- Every size calculation that involves an untrusted number uses checked
  arithmetic (`checked_add`, `checked_mul` in `kernel/lib/checked.h`).
- Every array index from an untrusted source is bounds-checked at the
  point of use.
- Every struct returned to userland is `memset` to zero before filling.
- Every `switch` over an untrusted value has a `default` that fails.
- No function pointers in writable memory when a `const` table will do.
- Interrupt handlers do the minimum and defer; they never block and never
  touch user memory.

## 19.4 Identity, permissions, and privilege

- Each process has real and effective uid/gid and supplementary groups,
  inherited across `fork`, preserved across `execve`.
- The VFS checks mode bits on every open, exec, directory search, create,
  unlink and rename. uid 0 bypasses mode checks but not `restrict`.
- **There are no setuid or setgid executables.** A file's mode can never
  raise the privilege of the process that runs it. Elevation is a request
  to `authd`, which authenticates the user through the trusted login UI
  and performs a specific named operation on their behalf. Reason: setuid
  programs are the classic source of local privilege escalation; a narrow
  broker is far easier to get right.
- Passwords are stored only as Argon2id hashes with per-user salts, in a
  file readable only by root. Failed attempts are rate-limited with a
  growing delay and logged.
- Home directories are mode 0700 by default.

## 19.5 Application sandbox and permissions

- `restrict(flags, paths)` drops abilities for the calling process and all
  descendants, permanently. Flags: `NET`, `FS_WRITE`, `FS_READ` (outside
  the listed paths), `SPAWN`, `DEVICES`, `IPC_CONNECT` (to ports other than
  Pane and those listed).
- Applications declare needed permissions in their `.desktop` file. The
  session launches each application already restricted to that list.
- Sensitive abilities are granted by the user at first use, through a
  prompt drawn by the shell that applications cannot draw over or click
  programmatically: screen capture, clipboard history, global hotkeys,
  network access, access to files outside the home directory.
- File open/save dialogs are part of the trusted shell and return a file
  descriptor, so an application needs no broad filesystem permission to
  open what the user chose.
- Grants are stored per user and are viewable and revocable in Dial
  (Privacy page).

## 19.6 Window server privacy

The window server sees every keystroke and every pixel, so its protocol is
a privacy boundary (§5A phase 12): no client can read another's pixels,
receive another's input, inject input, or enumerate other windows; the
clipboard is delivered only on paste to the focused window; password fields
switch the server into secure-entry mode; the lock screen is enforced by
the server itself, not by a client that could be killed.

## 19.7 Network security

- Inbound is default-deny. No service listens on a non-loopback address
  unless the user enabled it.
- The stack randomises TCP initial sequence numbers, ephemeral ports and
  DNS query IDs from the CSPRNG.
- All reassembly queues, ARP/DNS caches, connection tables and backlog
  queues are bounded, with eviction, so a flood cannot exhaust memory.
- Every header length, option length, and offset is validated against the
  received packet length before use.
- ICMP redirects and source-routed packets are ignored. ARP replies that
  were not requested do not overwrite live entries.
- The DHCP client sends no hostname or other identifying option by default.
- Anything that carries credentials or installs software uses TLS (1.3
  preferred, 1.2 the minimum) with certificate verification. Plain HTTP is shown as "not private" in UIs.
- `netd` runs unprivileged, restricted to its packet device and its ports.

## 19.8 Privacy rules

1. **No telemetry.** The OS and the shipped applications never contact any
   server on their own. The only automatic traffic is DHCP (and NTP, if the
   user enables network time).
2. **No hidden identifiers.** No machine ID, advertising ID or hardware
   serial is exposed to applications or sent over the network.
3. **Logs hold no user content.** The kernel log and service logs record
   events, never keystrokes, file contents, clipboard contents, passwords,
   or full URLs with query strings. Logs are readable only by root and the
   owning user.
4. **Crash dumps stay local**, are readable only by the owning user, and
   are deleted after 30 days. Nothing is uploaded.
5. **Recent-files lists, search history and clipboard history** are per
   user, stored in the user's home, off for the clipboard by default, and
   clearable from Dial.
6. **Secrets** (saved passwords, keys) live only in the keyring, encrypted
   at rest, and are zeroed in memory after use (`kfree_sensitive`, and a
   userland equivalent).
7. **Deleted means gone from view immediately**; with disk encryption on,
   discarding the key makes the whole disk unrecoverable.
8. **The time zone, locale and installed-font list** are not exposed to
   network peers by any system component.

## 19.9 Cryptography

- Never design an algorithm or protocol. Use: ChaCha20 (CSPRNG),
  SHA-256/SHA-512, HMAC, HKDF, Argon2id, Ed25519, X25519,
  ChaCha20-Poly1305 and AES-256-GCM (TLS 1.3), AES-256-XTS (disk).
- Every implementation is verified against the official test vectors (RFC
  or NIST) by a test that runs under `make test`.
- Comparisons of secrets and MACs are constant-time. Key material is
  zeroed after use.
- TLS depends on three things the OS must provide correctly first: the
  CSPRNG (phase 7), a correct wall-clock time (certificate validity dates
  are always checked; never build with date checks disabled), and a root
  certificate store.
- **Open owner decision (needs approval under §0 rule 14 before phase
  15):** where TLS and the primitives come from.
  (a) Port **Mbed TLS**: supports TLS 1.3, needs a modest libc, takes
  send/receive callbacks instead of sockets, and refuses to run until the
  OS registers a strong entropy source. Recommended.
  (b) Port **BearSSL**: smallest, no heap, pure state machine, but TLS 1.2
  at most and lightly maintained, so some modern servers will refuse it.
  (c) Write TLS and the primitives in-tree: not recommended; hand-written
  TLS and elliptic-curve code is a well-known source of subtle,
  catastrophic bugs.
  Until this is decided, nothing in the tree performs encryption that
  users rely on.

## 19.10 Software supply chain

- Packages and repository indexes are signed with Ed25519; the public key
  ships in the image. Unsigned or altered packages do not install.
- Install scripts run under `restrict`.
- Updates happen only when the user starts them; the previous kernel stays
  bootable.
- The build is reproducible: the same commit produces the same ISO hash.

## 19.11 Fuzzing and review

- `make fuzz` runs a harness for every hostile-input surface: syscall
  arguments, the ELF loader, lumfs images, tar, PNG/BMP/TGA, TTF, the Pane
  protocol, port messages, and every network packet parser. Harnesses are
  built for the host with AddressSanitizer where the code is portable, and
  as an in-kernel random-syscall test where it is not.
- A crash found by fuzzing becomes a regression test before it is fixed.
- Every commit that touches a boundary listed in §19.1(3) is reviewed
  against §19.3's rules before it is committed (§21 step 2 and 7).

## 19.12 Security checklist (definition of done, item 15)

Each line must be demonstrated by a test with real output:

1. A user process cannot read or write kernel memory or another process's
   memory.
2. No W+X mapping can be created; kernel text and rodata are read-only.
3. Bad pointers, wrapping lengths and unknown flags to every syscall are
   rejected without a kernel fault.
4. User stack, mmap and PIE addresses differ between runs.
5. One user cannot read another user's files.
6. A restricted process cannot exceed its restrictions.
7. A Pane client cannot read other windows, capture the screen, or log
   keys without a user grant.
8. No unsolicited network traffic at boot or idle.
9. Inbound connections are refused by default.
10. A bad TLS certificate is refused; a tampered package is refused.
11. All cryptographic test vectors pass.
12. Every fuzz harness runs its time budget with no crash.

---

# 20. Performance and resource efficiency

The goal is a system that feels instant on modest hardware and never
wastes the user's memory or battery. The method is: set budgets, design to
them, measure, and only then optimise.

## 20.1 Budgets

Measured in QEMU with KVM, 4 CPUs, 512 MB, 1920×1080 unless stated.

| Metric | Budget |
|---|---|
| Cold boot to interactive desktop | under 3 s |
| Input event to pixels on screen | under 20 ms |
| Composite one frame, 10 windows | under 8 ms |
| Idle desktop CPU use | under 1% of one CPU |
| Idle desktop memory after boot | under 96 MB |
| Wake-up latency of an interactive thread | under 1 ms (idle CPU) |
| Context switch | under 2 µs |
| Null syscall round trip | under 300 ns |
| Minor page fault | under 2 µs |
| `kmalloc`/`kfree` pair, uncontended | under 100 ns |
| Application start (small GUI app) | under 200 ms |
| Sequential read from the page cache | over 1 GB/s |
| TCP throughput to the host (virtio-net) | over 500 Mbit/s |

Budgets are revised only by a dated entry in `docs/DECISIONS.md`.

## 20.2 General rules

1. **Measure before changing code for speed.** A performance commit quotes
   the before and after numbers from `make bench`.
2. **Sleep, never poll.** Nothing spins or wakes on a timer to check for
   work. Use wait queues, `event_wait`, and interrupts. Idle CPUs `hlt`.
3. **Do no work when nothing changed.** No damage, no composite; no dirty
   pages, no write-back; no runnable threads, no tick work.
4. **Copy at most once.** Pixels move by shared memory; file data is mapped
   from the page cache; packets are handed over by reference where the
   device allows.
5. **Bound everything.** Every cache and queue has a size limit and an
   eviction policy; unbounded growth is a memory leak with extra steps.
6. **Keep the fast path lock-free or single-lock.** Per-CPU data first,
   then fine-grained locks; never hold a spinlock across anything that can
   block or take long.
7. **Algorithmic cost first.** Choose the right data structure (hash, tree,
   bitmap with summary levels) before tuning constants.
8. **Security checks are not optional costs.** A budget is never met by
   removing validation; find the time elsewhere.

## 20.3 Memory

- Demand paging and copy-on-write everywhere: memory is committed when
  touched, shared until written.
- A shared zero page backs untouched anonymous memory.
- Read-only segments of the same executable or library are shared between
  processes through the page cache.
- Slab caches sized to objects, with per-CPU magazines (phase 8); empty
  slabs are returned to the PMM under pressure.
- The page cache uses otherwise-free memory and gives it back first:
  watermarks (low/min), a reclaim thread with clock eviction, and only
  then an out-of-memory policy that kills the largest non-essential
  process with a notification — never a silent hang.
- 2 MiB mappings for the HHDM and large kernel regions.
- The PMM gets a summary bitmap or per-order free lists when `make bench`
  shows first-fit scanning in the profile.
- Per-process accounting (resident, shared, virtual) is exposed through
  `sysinfo` so Gauge shows where memory goes.
- Userland `malloc` returns freed pages to the kernel (`munmap`/
  `madvise`-style) instead of holding them forever.

## 20.4 CPU and scheduling

- Interactive first: threads that sleep on input or IPC get a priority
  boost and short latency; CPU-bound threads get longer slices at lower
  priority. The compositor and the focused application's threads are
  favoured.
- Timer: 100 Hz periodic now; move to one-shot/tickless-idle when SMP
  lands so idle CPUs take no interrupts.
- Per-CPU run queues with work stealing; keep a thread on the CPU whose
  cache it warmed unless the imbalance is significant.
- TLB: `invlpg` for small ranges, PCID to avoid flushes on context switch
  when the CPU supports it, batched shootdowns.
- FPU/SSE state: save and restore with `xsave`/`xrstor` only for threads
  that used it; the kernel stays `-mno-sse`.
- Use `rep movsb`/`stosb` (ERMS) for kernel copies; SSE2/AVX2 blit and
  blend routines in userland libgfx selected by CPUID at start-up.

## 20.5 Storage and filesystem

- One page cache (phase 9) with read-ahead for sequential access and
  clustered write-back.
- The journal batches metadata transactions; `fsync` forces only what the
  file needs.
- Dentry cache with LRU and negative entries.
- Interrupt-driven DMA with request queueing (NCQ / virtqueue); never
  polling I/O after boot.
- Directory lookups in lumfs move from linked records to hashed or
  tree-indexed directories once directories of 10,000 entries are
  benchmarked.

## 20.6 Graphics and the desktop

- Never read from the framebuffer (it is write-combined; reads are
  extremely slow). Compose in a back buffer in normal RAM, write damaged
  rectangles out once.
- Damage tracking end to end: widget → window → compositor → framebuffer.
- Cache expensive results: blurred shadows, rasterised glyphs (LRU atlas),
  scaled wallpapers, decoded icons.
- Opaque-region tracking so windows fully hidden behind others are not
  composited.
- Frame pacing from a timer at the display rate; clients throttle to the
  frame callback and never draw faster than they can be shown.
- Animations are driven by time, not frame count, and stop scheduling
  frames when they finish.
- Long work never runs on a UI thread; applications use worker threads and
  stay responsive (the toolkit warns in debug builds when an event handler
  runs longer than 50 ms).

## 20.7 Networking

- virtio-net with multi-buffer receive rings; interrupt mitigation under
  load; checksum computed once.
- Packet buffers come from a pool, are reference-counted, and are passed
  between the driver and `netd` by shared memory, not copied per packet.
- Socket buffers are bounded with back-pressure; TCP uses window scaling
  and delayed ACKs.

## 20.8 Adapting to the hardware

- Detect features with CPUID once at boot and record them in a
  `CpuFeatures` struct; choose code paths from it (ERMS, SSE2/AVX2 in
  userland, PCID, invariant TSC, x2APIC, RDRAND/RDSEED, SMEP/SMAP/UMIP).
- Size caches and pools from the amount of RAM actually present, not from
  constants (e.g. page-cache limits and slab magazine sizes as fractions).
- Use every CPU the firmware reports; never assume four.
- Prefer the fastest clock source available (invariant TSC calibrated
  against the HPET, falling back to the HPET).
- Prefer paravirtual devices (virtio) when present, emulated hardware
  (AHCI, e1000) otherwise.
- Read the framebuffer's real geometry and pixel format; never assume a
  resolution.

## 20.9 Measuring

- `make bench` boots a benchmark build headless and prints one line per
  metric in §20.1 that exists at the current phase.
- `docs/BENCH.md` records the numbers at the end of every phase, with the
  commit hash and the QEMU configuration. A regression of more than 10%
  on any line must be explained in `docs/DECISIONS.md` or fixed.
- The kernel keeps cheap counters (context switches, page faults,
  syscalls, interrupts, compositor frame time, cache hits) exposed through
  `sysinfo` and shown in Gauge.
- A sampling profiler (timer interrupt records RIP, dumped by a shell
  command and symbolised with the embedded table) is the first tool to
  reach for; guess-driven optimisation is not allowed.

---

# 21. Implementation procedure

Follow these steps, in order, for every feature, fix, or change.

1. **Read.** `docs/STATUS.md`, `docs/TO_FINISH.md`, the phase text in §5
   and its §5A rows, the design section for the subsystem, and any
   `docs/DECISIONS.md` entries that mention it. Read the code you are about
   to change and its callers.
2. **Threat-check.** Write down (in the session, not a file) which trust
   boundaries the change touches (§19.1 item 3), what input an attacker
   controls there, and what the worst outcome is. If the change exposes
   user data — keys, screen, clipboard, files, network — name which §19.8
   rule covers it. If no rule covers it, stop and ask the owner.
3. **Budget-check.** Name the §20.1 budgets the change can affect and how
   it will be measured.
4. **Design the smallest version** that meets the phase's acceptance
   criteria. State the data structures, the locking (which lock, what
   rank, can it sleep, interrupt-safe or not), the failure paths, and how
   memory is bounded. If this deviates from the spec, propose it first
   (§0 rule 9).
5. **Write the test first** or alongside: a kernel self-test (`test
   <name>`), an integration `.expect` file, a fuzz harness for any parser
   or boundary, and a benchmark line for any budgeted path. Include the
   negative tests: the bad pointer, the oversized length, the denied
   permission.
6. **Implement** to the coding standards in §4 and the kernel rules in
   §19.3. Handle every error; release everything on every failure path;
   no placeholders.
7. **Self-review against the checklists** before building:
   - *Memory safety:* every index bounded, every size checked for
     overflow, every allocation checked and freed exactly once, no use
     after free, no uninitialised data returned.
   - *Boundaries:* nothing untrusted used before validation; copied once.
   - *Concurrency:* which lock protects each shared field; lock order
     respected; nothing sleeps under a spinlock; interrupt-safe where
     called from interrupts.
   - *Privacy:* nothing sensitive logged; secrets zeroed; no new data
     leaves the process or machine.
   - *Performance:* no polling, no unbounded structure, no extra copy on
     a hot path.
8. **Build and verify.** `make iso`, run it, and paste the real output.
   Run the new test, then `make test`. From phase 6 run `make bench` if a
   budgeted path changed; from phase 7 run `make fuzz` if a boundary
   changed. When something fails twice, reduce it to the smallest
   reproducing case and use §15 before a third attempt.
9. **Document.** Update the files in `docs/` per §22.
10. **Commit** one logical change with the message format in §0 rule 4.
    Never commit with a failing test, an unmet acceptance criterion
    claimed as met, or secrets in the tree.

**When fixing a bug:** reproduce it first, add a test that fails because of
it, fix the cause rather than the symptom, confirm the test passes, and
check whether the same mistake exists elsewhere (search for the pattern).
If the bug crossed a trust boundary, treat it as a security bug: record it
in `docs/DECISIONS.md` with what class of bug it was and what rule or check
now prevents the class.

**When removing or replacing something:** search for every user, migrate
them in the same commit or a preceding one, delete the old code completely,
and note the replacement in `docs/DECISIONS.md` if the design changed.

---

# 22. Document maintenance

Each file in `docs/` has one job. Read a file completely before editing it.
Keep facts in exactly one file and link to it from the others.

| File | Job | Update when | How | Why |
|---|---|---|---|---|
| `SPEC.md` | What is being built and the rules for building it | The owner changes a requirement, or approves a proposed change | Edit the affected sections, keep section and syscall numbers stable, bump the version line, add a `DECISIONS.md` entry describing old, new and why. Never edit silently | It is the contract; a spec that drifts from the owner's intent makes every later session wrong |
| `DECISIONS.md` | Why the system is the way it is | Any non-obvious design choice, any deviation from the spec, any security bug class found, any budget change, any open question for the owner | Append a dated entry at the bottom: the choice, the reasoning, the alternatives rejected. Never rewrite old entries; supersede them with a new entry that names the old one | Future sessions have no memory; this file is how they avoid re-deciding or undoing a decision |
| `STATUS.md` | What is true right now | The end of every session, without exception | Rewrite the sections in place so the file describes the present: phase, what works, half-done, next, known bugs. Only claim what was verified this session or is unchanged since it was verified | The next session starts here; a stale status file causes wasted or destructive work |
| `TO_FINISH.md` | What remains, in order | A phase or checklist item is finished, added, re-scoped or re-ordered | Tick or move items; keep the phase list matching §5 and §5A; update the "right now" section and the date | It is the pick-up list; it must match the spec's phases and the real state |
| `BENCH.md` | Measured performance over time | The end of each phase from phase 6, and whenever a budgeted path changes | Append a dated block: commit hash, QEMU configuration, one line per metric | Regressions are only visible against recorded numbers |
| `SYSCALLS.md` | The syscall reference | Never by hand | Generated from `kernel/syscall/table.def` by the build | One source of truth for numbers and signatures |
| `PROTOCOL.md` | Pane wire format | The protocol in §9 changes | Edit together with the code and bump the protocol version | Clients and server must agree byte for byte |

Rules for all of them:
- Dates are absolute (`2026-10-03`), never "yesterday" or "last session".
- State facts that were verified, and say when something was not verified.
- When two files disagree, the order of authority is: `SPEC.md` for
  requirements, `DECISIONS.md` for recorded deviations from it, the code
  and test output for what actually exists. Fix the disagreement in the
  same session it is found.
- A change to the code that makes any sentence in `docs/` untrue is not
  finished until that sentence is fixed.

| File | Job | Update when | How | Why |
|---|---|---|---|---|
| `CHANGELOG.md` | What changed in each released version | Every commit that changes behaviour adds a line under "Unreleased"; a release renames that block | §23 | Users and later sessions must be able to see what a version contains, security fixes first |

---

# 23. Versioning and releases

## 23.1 The version number

**Before 1.0: `0.MINOR.PATCH` followed by one letter, `a` to `j`** (owner,
2026-10-04). Versions climb slowly and do not follow the phase number.
- Every release takes the next letter: `0.0.5a`, `0.0.5b`, … `0.0.5j`.
- After `j`, `PATCH` goes up by one and the letter starts again at `a`:
  `0.0.5j` is followed by `0.0.6a`.
- After `0.0.9j` comes `0.1.0a` (`PATCH` carries into `MINOR` the way a
  car's odometer does).
- The first version under this scheme is `0.0.5a` (the phase 8 release);
  the earlier releases `0.3.0` to `0.7.0` keep their old numbers and tags.
- Which phase a release completes is written in its changelog entry, not
  in its number.
- `1.0.0` (no letter) is released when every item of §16 is demonstrated.

**From 1.0:**
- `MAJOR` increases when existing programs can break: an incompatible
  change to the syscall ABI, the Pane protocol, an on-disk format without
  an automatic upgrade, or a libc interface.
- `MINOR` increases for new features that keep compatibility.
- `PATCH` increases for bug, performance and security fixes only.

## 23.2 Where the version lives

- The file `VERSION` at the repository root holds the number and nothing
  else. It is the only place the number is written by hand. The build reads
  it for the boot banner, `uname`, the About window, and the ISO name
  (`lumen-<version>.iso` in the name of a development ISO; the release file is always `dist/lumen.iso`).
- `make RELEASE=1`, run on the tagged release commit with a clean working
  tree, produces a **release build** that shows the plain number. Every
  other build is a **development build** and shows
  `<VERSION>-dev+<short commit hash>`, so a test build can never be mistaken
  for a release. Between releases `VERSION` holds the next version to be
  released.
- Only `kernel/lib/version.cpp` is compiled with the version and build date;
  everything else calls `lumen_version()` / `lumen_build_date()`.
- Every release is a git tag `v<VERSION>` on the release commit.

## 23.3 The changelog

`docs/CHANGELOG.md`, newest version first. Each version has a date and
entries under these headings, in this order, omitting empty ones:

- **Security** — anything that fixes or hardens a trust boundary. A version
  with a Security entry is marked `(security release)` in its title.
- **Fixed** — bug fixes.
- **Performance** — with the before and after numbers from `make bench`
  once it exists.
- **Added**, **Changed**, **Removed**.

Entries are written for the person using the OS: what changed for them, in
one line. The top block is always `## Unreleased`; a commit that changes
behaviour adds its line there in the same commit.

## 23.4 Release procedure

1. `make test` passes; from phase 6 `make bench` is recorded in
   `docs/BENCH.md`; from phase 7 `make fuzz` passes.
2. Set `VERSION` (per §23.1) and rename `## Unreleased` in the changelog to
   `## <version> — <date>`, adding a fresh empty `## Unreleased` above it.
3. Update `docs/STATUS.md`. Commit as `release: <version>`.
4. Tag the commit `v<version>`. Build from the tag with
   `make RELEASE=1 dist`, which overwrites `dist/lumen.iso` (the folder
   holds exactly one ISO, always under that name), and points any
   VirtualBox VM that boots it at the file.
5. Bump `VERSION` to the next version (§23.1: the next letter) in a following commit
   (`release: begin <next>`), so development builds are labelled correctly.

A security fix is released on its own as soon as it is verified; it does not
wait for other work.

## 23.5 Updates on an installed system (phase 18)

`pkg upgrade` compares installed versions against the signed repository
index, shows the changelog entries for what is new (Security first),
installs only when the user confirms, and keeps the previous kernel as a
boot entry. The OS never downloads or installs an update on its own.
