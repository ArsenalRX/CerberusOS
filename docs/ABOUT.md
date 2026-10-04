# About Cerberus and Spec

**What this file is for.** A plain-language description of the Cerberus
operating system and its programming language, Spec, that the owner can use
to explain the project to friends, family and technical people. It
summarises docs/STATUS.md (what is verified), docs/ROADMAP.md (what is
planned) and docs/SPEC.md (the full design, which wins if they disagree).
It claims nothing that STATUS.md does not verify; planned things are marked
as planned.

**When to update it.** At every release, and whenever a phase is finished,
re-ordered or re-scoped, so that "What it can do today" and "Where it's
going" stay true. Keep the language names from docs/DECISIONS.md (Spec,
`specc`, `.spec`), never the older name used in SPEC.md.

Last updated: **2026-10-04**, at release **0.0.5f**. (The system was called Lumen until that day.)

---

Cerberus is a new operating system for 64-bit PCs, written from scratch: its
own kernel, its own desktop, and eventually its own programming language,
Spec. The goal is a system that puts the privacy and security of the person
using it first, feels smooth, and uses the hardware efficiently. It is
early: today it boots and runs in virtual machines, not yet as your
everyday computer.

## Cerberus in one minute

- **What it is.** An operating system: the basic software that starts when
  a computer is switched on and lets every other program run. Windows,
  macOS and Linux are operating systems. Cerberus is a new one, for 64-bit
  x86 processors (the kind in most PCs and laptops).
- **Written from scratch.** It is not based on Linux, Windows or any other
  system's code. Only the bootloader (Limine) comes from elsewhere.
- **A hybrid kernel.** The core is a "hybrid kernel": a few speed-critical
  jobs run inside the core, and the rest run as separate, protected
  programs (explained below).
- **Its own desktop and language.** It has a graphical desktop with windows
  and a taskbar, and a planned programming language, Spec, designed
  together with the system.
- **Who makes it.** A personal project by one developer, the project owner,
  built with an AI coding assistant (Claude Code) working from a written
  design document.
- **Where it stands.** The current release is **0.0.5f** (2026-10-04). It
  runs in the VirtualBox and QEMU virtual machines from a single ISO file,
  on old BIOS or modern UEFI, and can keep files on a virtual hard disk. It
  cannot yet connect to the internet or be installed on a real PC.

## How Cerberus works

Think of a computer as a building. The **hardware** is the building itself:
rooms (memory), workers (processor cores), doors and windows (keyboard,
screen, disk). The **operating system** is the building management: it
decides who gets which room, which worker does which job, and who may use
which door. **Programs** are the tenants: they do the interesting work, but
they must ask management for everything.

```
 +-----------------------------------------------------------+
 |  Applications     Terminal, file manager, editor, ...     |  planned (phase 13)
 +-----------------------------------------------------------+
 |  Servers          Pane (windows), netd (network), ...     |  planned (12, 14)
 +-----------------------------------------------------------+
 |  Kernel           memory, scheduling, system calls,       |  working today
 |                   files core, drivers, messaging          |
 +-----------------------------------------------------------+
 |  Hardware         processor cores, memory, disk, screen   |
 +-----------------------------------------------------------+
```

Each layer only talks to the one below it through well-defined, checked
doors. Today the kernel layer works, user programs run on top of it, and a
preview desktop runs inside the kernel; the server and application layers
are what the coming phases build.

### Booting

When the (virtual) machine starts, a small program called the **bootloader**
(Limine) loads the Cerberus kernel into memory and hands over control. The
kernel then finds the processor, the memory, the timers, the interrupt
controllers and the real-time clock on its own, starts every processor
core, and brings up the desktop. This takes about a second.

### The kernel and "hybrid kernel"

The **kernel** is the part of the operating system with full control of
the machine. Everything else runs with limited rights and has to ask the
kernel for help.

Operating systems make a basic choice here. A *monolithic* kernel (like
Linux) puts nearly everything inside the kernel: fast, but a bug anywhere
can bring down the whole system. A *microkernel* keeps the kernel tiny and
runs almost everything as separate programs: safer, but often slower.
Cerberus is **hybrid**: the rule (SPEC §6.2) is that anything needing
sub-microsecond speed or special processor instructions stays in the kernel
(memory management, scheduling, system calls, the core of the file system,
disk and input drivers, messaging between programs), and everything else
becomes a separate protected program, a **server**, reached through
messages. The window server (Pane) and the network stack (netd) are planned
to be such servers. Keeping the trusted core small is also a security
choice.

### Memory

Every program gets its **own private memory space**. It cannot see or
touch the memory of other programs or of the kernel; if it tries, it is
stopped. Memory is handed out in 4 KiB **pages**, and a page is only really
given to a program when the program first touches it ("demand paging"), so
memory is not wasted on things never used.

When a program makes a copy of itself (`fork`), the copy does not duplicate
all of its memory straight away. Both share the same pages until one of
them writes, and only then is that one page copied. This is called
**copy-on-write** and makes starting programs cheap. Memory is wiped before
it is reused, so no program can read what another left behind.

### Running programs

A computer appears to do many things at once. In reality each processor
core switches between jobs many times a second. The **scheduler** decides
who runs next: Cerberus has four priority levels, time slices, and lets idle
cores take work from busy ones. A unit of work is a **thread**; a running
program (a **process**) has one or more threads. Cerberus uses every
processor core it finds (tested with four).

Programs run in **user mode**, a restricted processor mode. Only the
kernel runs in **kernel mode** with full rights. When a program needs
something only the kernel can do (open a file, get memory, start another
program), it makes a **system call**: a controlled request through a single
door, where every argument is checked. Cerberus has 14 system calls today and
a small C library for programs. A program that crashes is stopped and
reported; the rest of the system carries on.

### The desktop

The desktop is drawn by a **compositor**: each window draws its own picture,
and the compositor combines them into the final screen image, adding
shadows, rounded corners and the mouse cursor, and redrawing only what
changed.

Today's desktop is a **preview that runs inside the kernel**: a wallpaper,
a taskbar with a launcher menu, clock and memory use, and four built-in
windows (Terminal, System Monitor, Memory Map, About) that can be moved,
resized, minimised, maximised and closed. In **phase 12** it moves out of
the kernel into **Pane**, a separate window server program, so that no
graphics code runs with full rights. In **phase 13** comes **Facet**, a
toolkit of buttons, text boxes, menus and dialogs with dark and light
themes, and the applications built with it: terminal, file manager, text
editor, settings, calculator, image viewer, paint program and more.

### Files and disks

Think of a library: the **virtual file system** is the front desk that
every request goes through, whichever shelf the book is on. Behind it are
several "shelves":

- `/` holds the system's own programs, unpacked from an archive at boot and
  kept **read-only**;
- `/tmp` lives in memory and is gone at shutdown;
- `/dev` holds devices that look like files (`/dev/null`, `/dev/random`,
  each disk as `/dev/disk/sda`);
- a disk can be attached ("mounted") on any folder, usually `/mnt`.

Disks use **cerfs**, Cerberus's own format. cerfs keeps a **journal**: a
log of each change written before the change itself, so a crash or power
cut never leaves the disk in a broken state (after a restart, a finished
log entry is replayed and an unfinished one is ignored). Every piece of
bookkeeping on the disk carries a checksum, and the disk is treated as
untrusted: a damaged disk gives an error, not a crash. This is tested by
cutting the power mid-write and by feeding the system thousands of
deliberately damaged disks.

The front desk also enforces **permissions** (who may read, change or run
each file) and keeps recently used file contents and names in memory (the
**page cache** and **name cache**), so reading a file a second time does
not touch the disk. Cerberus has drivers for SATA disks (fast) and older
IDE disks. Opening files from a program's own memory map and a graphical
file manager come later (phases 11 and 13).

### Networking (planned)

Cerberus has no networking yet, so nothing can leave the machine. **Phase 14**
adds network card drivers and **netd**, a network server running as a
separate, unprivileged program: addresses (DHCP), name lookup (DNS), TCP
and UDP, and tools such as `ping` and `fetch`. A **firewall blocks all
incoming connections by default**, and each application needs permission
to use the network. Secure connections (HTTPS) come in phase 15.

Cerberus will have **its own web browser**, written from scratch (phase 15B,
decided 2026-10-04). It comes after networking and secure connections,
which it needs. The first version renders HTML and CSS without
JavaScript, runs in a sandbox, and treats every web page as hostile input.

### Security and privacy

The owner's first priority is that the person using the machine can trust
it. The principles (SPEC §19): give each program only what it needs, deny
by default, check every piece of data that crosses a boundary, refuse when
a check fails, and never send anything off the machine unless the user
asked. **No telemetry, ever.**

In place today, each shown by a test:

- Programs cannot read or write the kernel's memory or each other's.
- Every system-call argument is checked; bad pointers are refused safely.
- No memory is ever both writable and executable (W^X); memory is wiped
  before reuse.
- Randomised program layout (ASLR), so attackers cannot predict addresses.
- Guard pages under every stack and stack-smashing detection.
- A hardened memory allocator that refuses double frees and foreign
  pointers.
- Processor protections (SMEP, SMAP, UMIP) where the CPU offers them.
- A cryptographic random number generator (ChaCha20).
- Automatic fuzz testing (millions of random inputs) of the program loader,
  the archive reader and the system calls.

Planned: file permissions (phase 9); only the window server may read the
keyboard and screen (phase 10); programs cannot spy on each other's windows,
screenshots and clipboard history need permission (phase 12); firewall and
per-app network permission (phase 14); user accounts, Argon2id password
hashes, lock screen, app sandboxing, an encrypted keyring and HTTPS
(phase 15); signed packages (phase 17); optional full-disk encryption and
signed updates (phase 18); kernel address randomisation (phase 19).

Today the desktop and kernel shell still run with full rights, and there
are no users, permissions or files yet.

### Performance

Smoothness is treated as a requirement with measured budgets, not as a
later clean-up. Under QEMU with four cores, switching between threads takes
about 23 nanoseconds (budget 2,000), a system call about 34 nanoseconds
(budget 300), and the idle desktop uses 0% of the CPU. Two numbers are over
budget and on the fix list: the first touch of a memory page (2.0–2.9
microseconds against 2), and full-screen redraws on VirtualBox (about
23 ms, which is why window dragging feels laggy there). Goals for later:
boot to a usable desktop in under 3 seconds, small apps starting in under
200 ms, and a desktop frame drawn in under 8 ms.

## The Spec programming language

**Spec** is a systems programming language designed alongside Cerberus. It is
meant for writing Cerberus's applications, and eventually parts of Cerberus
itself. The idea behind building both: a language made together with its
operating system can express things a general-purpose language cannot,
such as system calls as typed parts of the language, kernel handles the
compiler checks, and hardware register layouts verified when the program is
compiled.

**Honest status: the Spec compiler has not been started.** The `spec/`
folder holds only empty folders. It is **phase 16** on the roadmap.

### Design goals and main features (SPEC §13.1)

- **Statically typed and compiled** to native x86-64 machine code for
  Cerberus; no garbage collector.
- **Regions** (the distinctive feature): memory allocated inside a
  `region` block is all freed at its closing brace, and the compiler stops
  any reference from escaping it. This gives predictable memory management
  without a garbage collector or full ownership tracking.
- **Traits and generics**, resolved at compile time.
- **Closures** (functions as values), **modules** with `pub` visibility,
  and **pattern matching** that the compiler checks covers every case.
- **Mutable references** with a simple rule: only one writable reference
  at a time.
- **Compile-time evaluation** (`const fn`, `static assert`).
- **System calls as typed constructs**, turned directly into the processor
  instruction, with no C layer in between.
- **Capability types**: kernel handles such as an open file (`Fd`) cannot
  be copied by accident or used after closing.
- **Hardware register layouts** (`mmio struct`): the compiler refuses a
  write to a read-only register.
- **Arrays and slices** with bounds checks (and an explicit escape hatch),
  and inline assembly.

The design document shows fragments of the planned syntax, for example:

```
trait Show { fn show(self) -> str }
impl Show for Point { ... }
fn dump[T: Show](v: T)

region r { let x = r.alloc(...); }

syscall write(fd: Fd, buf: &[u8]) -> Result[usize, Errno]

mmio struct AhciPort {
    0x00: clb: u32 rw,
    0x10: is: u32 rw1c,
}
```

The full syntax is not final and will be settled when the compiler is
built.

### How the compiler works (SPEC §13.2)

The compiler, `specc`, turns a `.spec` source file into a Cerberus program in
stages: it splits the text into words (lexer), builds a tree of the program
(parser), resolves names and modules, checks types, regions, capabilities,
pattern coverage and references, specialises generic code, translates it to
an internal form called SIR, optimises it (constant folding, dead-code
removal, inlining and more), assigns processor registers, picks x86-64
instructions, and finally writes an ELF program file Cerberus can run.

### Tooling (SPEC §13.4)

- `specc`, the compiler, with optimisation levels, debug info, and options
  to show each stage's output.
- `specfmt`, a formatter that gives all code one standard layout.
- A standard library (text, lists, maps, files, processes, threads, time,
  maths, JSON, GUI bindings, raw system calls).
- **Excellent error messages** as a requirement: file, line and column, the
  source line with the problem marked, an explanation of the rule, and a
  suggested fix where one is obvious.

Phase 16 is done when `specc hello.spec -o hello` makes a program that runs
on Cerberus and a Spec application with windows runs on the desktop.

## What it can do today (0.0.5f)

- Boots in VirtualBox and QEMU from one ISO, with BIOS or UEFI, in about a
  second, and uses every processor core.
- Manages memory with private spaces per program, demand paging and
  copy-on-write.
- Runs many threads and programs at once; runs real user programs in a
  protected mode and survives their crashes.
- Shows a desktop preview: wallpaper, taskbar, launcher menu, movable and
  resizable windows, keyboard shortcuts (Alt+Tab, Alt+F4, Super).
- Keeps files and folders, in memory and on disks formatted with cerfs
  (which survives a power cut), with permissions and the usual tools
  (`ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `mount`, `mkfs.cerfs` ...).
- Lets programs work together: threads inside a program, messages between
  programs through named ports (which can carry files and shared memory),
  signals, and servers that sleep until something happens.
- Has a built-in terminal shell that runs programs by name, with output
  redirection, system information and self-tests; the terminal scrolls back
  1,000 lines.
- Has the security protections listed above, checked by 40 automated tests
  and fuzzing.

It cannot yet use the network, play sound, use USB, have user accounts,
install to a disk, or run Windows or Linux software.

## Where it's going

- **Done so far:** phases 0–11 (boot, memory, scheduling, user programs,
  multi-core, files and disks, drivers, programs talking to each other) and
  desktop polish with smooth text.
- **Phase 12:** Pane, the window server as a separate program.
- **Phase 13:** the Facet toolkit, desktop shell and applications.
- **Phase 14:** networking and the internet.
- **Phase 15:** users, passwords, sandboxing, HTTPS.
- **Phase 16:** the Spec language.
- **Phase 17:** running ported C programs, a package manager.
- **Phase 18:** an installer like Windows Setup (erase a disk, or install
  alongside Windows with a boot menu to choose), disk encryption, updates,
  recovery.
- **Phase 19:** stretch goals: USB, sound, real PCs, IPv6, ARM64.
- **Phase 15B:** our own web browser (HTML and CSS first; JavaScript later).

## FAQ for explaining it

**Is it Linux?** No. Cerberus is written from scratch. It does not use Linux
or Windows code, and it cannot run their programs as they are.

**Can it run Windows or Linux apps?** Not now. Phase 17 aims to let
ordinary C programs (for example Lua and a small C compiler) be rebuilt
from their source code for Cerberus. Running Windows or Linux programs
directly is not a goal.

**Can I install it on my PC?** Not yet. It runs in virtual machines.
Installing to a disk, including next to Windows with a boot menu, is phase
18; real-hardware support is a phase 19 goal.

**Why build an operating system?** To understand and control every layer,
to build a system around privacy and security from the start instead of
adding them later, and to explore what a language designed together with
its operating system can do.

**Is it safe to use for real data?** No. There are no user accounts, no
file permissions and no saved files yet. Do not put real data in it before
phase 15.

**Does it collect data about me?** No, and it never will: no telemetry, no
analytics, no background connections you did not ask for. Today it has no
network at all.

## Glossary

- **Operating system (OS):** the software that manages the hardware and
  lets programs run.
- **Kernel:** the core of the OS, with full control of the machine.
- **Hybrid kernel:** a kernel that keeps speed-critical services inside and
  runs others as separate protected programs.
- **Bootloader:** the small program that loads the kernel at power-on
  (Cerberus uses Limine).
- **Process:** a running program with its own private memory.
- **Thread:** one line of work inside a process; a process can have many.
- **Scheduler:** the part of the kernel that decides which thread runs on
  which core, and when.
- **User mode / kernel mode:** the restricted processor mode programs run
  in, and the full-rights mode only the kernel uses.
- **System call:** a program's checked request to the kernel.
- **Page:** a 4 KiB block of memory, the unit the kernel hands out.
- **Compositor:** the part that combines all windows into the screen image.
- **Window server:** the program that owns the screen and input and manages
  windows (Pane, planned).
- **File system:** how files and folders are stored on a disk (cerfs,
  planned).
- **Journal:** a log of changes that keeps a disk consistent after a crash.
- **Driver:** code that operates a piece of hardware.
- **Firewall:** a filter that decides which network traffic is allowed.
- **ASLR:** address space layout randomisation; placing program parts at
  random addresses so attacks are harder.
- **W^X:** "write xor execute"; memory is never both writable and runnable.
- **Fuzzing:** testing by feeding huge amounts of random or malformed input.
- **Compiler:** a program that turns source code into a runnable program
  (`specc`, planned).
- **Virtual machine:** software (VirtualBox, QEMU) that imitates a PC, so
  Cerberus can run safely inside Windows.
