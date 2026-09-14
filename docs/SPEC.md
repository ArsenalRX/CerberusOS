# Lumen OS + Glint — Full Build Specification

**This document is the prompt.** Paste it into Claude Code (or keep it at
`docs/SPEC.md` in the repo and reference it every session). It defines a
complete operating system with a graphical desktop and a systems programming
language that targets it.

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
- Everything must run in QEMU. Real hardware is a phase-14 stretch goal.
- No external runtime dependencies in the final image except Limine.

---

# 2. Repository layout

```
lumen/
├── Makefile                  # top-level: build, run, debug, test, iso
├── docs/
│   ├── SPEC.md               # this document
│   ├── DECISIONS.md          # dated design decisions + rationale
│   ├── STATUS.md             # current state, updated every session
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

## Phase 14 — Glint native backend and retarget

See §13. Deliverables: x86-64 code generation, ELF output, the Glint standard
library, Lumen syscall bindings, and at least three applications rewritten in
Glint.

**Accept:** `glintc hello.gl -o hello` produces a Lumen ELF binary that runs on
Lumen and prints correctly. A Glint-written GUI application runs on the
desktop.

---

## Phase 15 — Stretch goals (only after 14)

Networking (e1000 + a minimal TCP/IP stack), USB (XHCI), audio (AC'97 or
Intel HDA), real-hardware boot, self-hosting the Glint compiler, a Glint
package manager, ARM64 port.

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
```

Keep the numbers stable. Add new calls at the end of their block.

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

C++ first. Ported to Glint in phase 14.

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
Written in **Glint** once phase 14 lands — this is the flagship demonstration
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

---

# 17. Explicit non-goals

Do not build these, and do not spend time discussing them:

- POSIX compliance. We borrow ideas, not the standard.
- Binary compatibility with Linux or anything else.
- Security hardening beyond basic user/kernel separation and pointer
  validation. No KASLR, no SMEP/SMAP juggling, no seccomp.
- Multi-user accounts, permissions beyond a mode field, or authentication.
- A web browser.
- Backwards compatibility with anything.
- Performance optimisation before phase 14. Correctness first. The only
  exception is the compositor, which must not tear.

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
