# Benchmarks

> **Name:** the operating system was called **Lumen** until 2026-10-04,
> when the owner renamed it **Cerberus** (its file system lumfs became
> **cerfs**). Entries written before that date keep the old names.

Measured performance over time, against the budgets in docs/SPEC.md §20.1.

**What this file is for.** A regression is only visible against recorded
numbers. This is the record.

**When to update it.** At the end of every phase (from phase 6), and whenever
a change touches a path that has a budget.

**How to update it.** Run `make bench` on an otherwise idle host (twice; take
the second run) and append a dated block: version, commit, where it ran, one
row per metric with the budget next to it. Never edit an old block. If a
number is worse than the previous block by more than 10%, or over budget,
say why in the block and, if it is being accepted, add a docs/DECISIONS.md
entry.

**Why the environment matters.** These are virtual machines on a desktop
that is doing other things. Nested virtualisation (QEMU/KVM inside WSL2
inside Hyper-V) makes anything that traps to the hypervisor — clock reads,
page-table changes, port I/O — far slower than on real hardware, and other
load on the host shows up as outliers. Compare like with like: same
environment, same build type.

---

## 0.6.0 — 2026-10-03 (phase 6: threads and scheduling)

Commit: the `v0.6.0` tag. Debug build (red zones, poisoning, UBSAN subset).
The host was also running VirtualBox during these runs.

### QEMU 8.2 + KVM in WSL2, 4 CPUs (1 active), 512 MiB — `make bench`

| Metric | Measured | Budget | Verdict |
|---|---|---|---|
| Context switch (two threads yielding) | 13 ns | 2,000 ns | within |
| Wake-up latency, interactive thread, average | 6 µs | 1,000 µs | within |
| Wake-up latency, worst of 500 | 13 µs | 1,000 µs | within |
| `kmalloc(64)` + `kfree` pair | 85 ns | 100 ns | within (13–19 ns with `DEBUG=0`, measured for 0.5.0) |
| Minor page fault (first touch of an anonymous page) | 4,107 ns | 2,000 ns | **over** |
| Idle desktop CPU use (3 s sample) | 0 % of ticks | under 1 % | within |
| Last composite (partial frame; not the 10-window budget case) | 147 µs | 8,000 µs | not comparable yet |

Notes:
- **Minor page fault is over budget.** The path is: fault, VMA lookup,
  frame allocation, zeroing 4 KiB, page-table walk with up to three table
  allocations, return. Not yet profiled; the debug build's checks and the
  bitmap allocator's scan are the first suspects. To be examined with the
  sampling profiler SPEC §20.9 calls for; no code was changed for it.
- One earlier run of `test sched` saw a single wake-up sample above 1 ms out
  of 200 (average still single-digit microseconds). The test checks the
  average and reports the worst.
- The context-switch figure divides elapsed time by switches that actually
  happened, with no address-space change. It is plausible for a switch that
  saves six registers on a 5 GHz core, but it has not been cross-checked
  against a cycle counter.

### VirtualBox 7.2.6 (Hyper-V backend), VM "Lumen", 4 CPUs, 2 GiB — `bench` typed in the shell

| Metric | Measured | Budget | Verdict |
|---|---|---|---|
| Context switch | 13 ns | 2,000 ns | within |
| Wake-up latency, average | 28 µs | 1,000 µs | within |
| Wake-up latency, worst of 500 | 810 µs | 1,000 µs | within |
| `kmalloc(64)` + `kfree` pair | 99 ns | 100 ns | within, barely |
| Minor page fault | 19,315 ns | 2,000 ns | **over** (the budgets are defined for QEMU/KVM) |
| Idle desktop CPU use | 0 % of ticks | under 1 % | within |

Note: on VirtualBox `test sched` printed a context-switch time of 2 ns for a
shorter run, which is not believable; the guest's reference clock (HPET
there) appears to lag while the guest is busy. Treat sub-millisecond timings
from VirtualBox as rough.

### Not measured yet

Cold boot to desktop, input-to-pixels latency, a 10-window composite, null
system call, application start, page-cache read rate and TCP throughput have
budgets but no benchmark, because the features they measure do not exist or
the benchmark has not been written. First-frame time at boot varies between
21 and 39 ms from run to run and is not used as a benchmark.

---

## 0.7.0 — 2026-10-03 (phase 7: userland and system calls)

Commit: the `v0.7.0` tag. Debug build. The kernel is now compiled with the
stack protector, and SMEP/SMAP/UMIP are on under QEMU/KVM.

### QEMU 8.2 + KVM in WSL2, 4 CPUs (1 active), 512 MiB — `make bench`

Five runs over the evening; the range is given where they differed.

| Metric | Measured | Budget | Verdict |
|---|---|---|---|
| Context switch (two threads yielding) | 14–16 ns | 2,000 ns | within |
| Wake-up latency, interactive thread, average | 6–7 µs | 1,000 µs | within |
| Wake-up latency, worst of 500 | 23–127 µs | 1,000 µs | within |
| `kmalloc(64)` + `kfree` pair | 80–101 ns | 100 ns | at the limit (one run of five was 1 ns over) |
| Minor page fault (first touch, through the user-copy path) | 2,247–3,278 ns | 2,000 ns | **over** |
| System-call round trip (`getpid` from ring 3) | 35–37 ns | 300 ns | within |
| Idle desktop CPU use (3 s sample) | 0 % of ticks | under 1 % | within |
| Last composite (partial frame) | 161–258 µs | 8,000 µs | not comparable yet |

Notes:
- **Minor page fault is still over budget**, though lower than 0.6.0's
  4,107 ns. The benchmark changed: it now touches each page through
  `copy_to_user` (a direct kernel write to a user page is forbidden under
  SMAP), so the two blocks are not strictly like for like. The spread
  between runs is large; not profiled yet.
- **`kmalloc`/`kfree` is at its limit in the debug build.** The stack
  protector added a few instructions to every function with a local buffer.
  Accepted for the debug build; a `DEBUG=0` build was not measured this
  time.
- The system-call figure is the difference between running `/bin/sysbench`
  with 2,000,000 `getpid` calls and with none, divided by the count; it
  includes the user-side loop.
- The context-switch benchmark was corrected in this release: its two
  threads now run at the lowest priority level, so they cannot start
  before both exist and a timer tick cannot separate them. Earlier
  implausible readings on VirtualBox (2 ns, 0 ns) came from that, not from
  the guest clock as the 0.6.0 block guessed.

### VirtualBox 7.2.6 (Hyper-V backend), temporary VM, 4 CPUs, 2 GiB — `bench` typed in the shell

No SMEP/SMAP/UMIP offered to the guest there.

| Metric | Measured | Budget | Verdict |
|---|---|---|---|
| Context switch | 15 ns | 2,000 ns | within |
| Wake-up latency, average | 30 µs | 1,000 µs | within |
| Wake-up latency, worst of 500 | 480 µs | 1,000 µs | within |
| `kmalloc(64)` + `kfree` pair | 104 ns | 100 ns | **over** (debug build) |
| Minor page fault | 19,356 ns | 2,000 ns | **over** (the budgets are defined for QEMU/KVM) |
| System-call round trip | 37 ns | 300 ns | within |
| Idle desktop CPU use | 0 % of ticks | under 1 % | within |

## 0.0.5a — 2026-10-04 (phase 8: SMP)

First release under the new version scheme; it follows 0.7.0. The kernel's
reference clock is now the CPU's time-stamp counter where it is usable
(QEMU/KVM: 3,869 MHz, calibrated against the HPET), which makes every
timestamp in the benchmarks cheaper than the HPET read it replaces.

### QEMU 8.2 + KVM in WSL2, 4 CPUs (all active), 512 MiB — `make bench`, six runs

| Metric | Measured | Budget | Verdict |
|---|---|---|---|
| Context switch | 22–24 ns | 2,000 ns | within |
| Wake-up latency, interactive thread, average | 7–9 µs | 1,000 µs | within |
| Wake-up latency, worst of 500 | 76–101 µs | 1,000 µs | within |
| `kmalloc(64)` + `kfree` pair | 76–84 ns | 100 ns | within |
| Minor page fault (first touch, through the user-copy path) | 1,993–2,922 ns | 2,000 ns | **over** in five runs of six |
| System-call round trip (`getpid` from ring 3) | 33–34 ns | 300 ns | within |
| Idle desktop CPU use (3 s sample) | 0 % of ticks | under 1 % | within (one run read 16 % while a VirtualBox VM was busy on the same host; 0 % in the five others) |
| Last composite (partial frame) | 130–194 µs | 8,000 µs | not comparable yet |

Notes:
- **Wake-up latency** is back at the 0.7.0 level (6–7 µs). With the HPET as
  the clock, phase 8 builds measured 18–19 µs; part of that was the cost of
  reading the HPET itself, now gone.
- **`kmalloc`/`kfree` moved away from its limit** (80–101 ns before): the
  per-CPU slabs take no lock on the common path.
- **Minor page fault is still over budget**, by a little. Not profiled yet.

### VirtualBox 7.2.6 (Hyper-V backend), temporary VM, 4 CPUs, 2 GiB — `bench` typed in the shell

No HPET, so the reference clock is the PIT (this VM's time-stamp counter
measured 6.9 MHz and is not used). No SMEP/SMAP/UMIP offered.

| Metric | Measured | Budget | Verdict |
|---|---|---|---|
| Context switch | 32 ns | 2,000 ns | within |
| Wake-up latency, average | 238 µs | 1,000 µs | within |
| Wake-up latency, worst of 500 | 8,061 µs | 1,000 µs | **over** (the budgets are defined for QEMU/KVM) |
| `kmalloc(64)` + `kfree` pair | 67 ns | 100 ns | within |
| Minor page fault | 17,221 ns | 2,000 ns | **over** (QEMU/KVM budget) |
| System-call round trip | 37 ns | 300 ns | within |
| Idle desktop CPU use | 0 % of ticks | under 1 % | within |
| Full-screen composite (maximised window, from the System Monitor) | 22,959 µs | 8,000 µs | **over**: about 130 MB/s into video memory, the likely cause of laggy dragging there |

## 0.0.5b — 2026-10-04 (desktop polish)

### QEMU 8.2 + KVM in WSL2, 4 CPUs, 512 MiB — `make bench`, one run

Context switch 23 ns, wake-up 10 µs average / 72 µs worst, `kmalloc`+`kfree`
83 ns, minor page fault 2,309 ns (**over**, unchanged), system call 31 ns,
idle desktop 0 %, last composite 203 µs. No change from 0.0.5a outside run
to run variation.

### Desktop presentation (new measurements)

| Case | Pixels in the damaged area | Pixels written | Frame time |
|---|---|---|---|
| QEMU/KVM, dragging the terminal 4 px per step (worst frame) | about 424,000 | 82,162 | 2,979 µs |
| QEMU/KVM, first full frame at 1280×800 | 1,024,000 | 1,024,114 | 19,406 µs |
| VirtualBox, 4 CPUs, first full frame at 1024×768 | 786,432 | 786,546 | 30,246 µs |
| VirtualBox, maximising the terminal | — | 441,388 | 35,522 µs |
| VirtualBox, restoring it | — | 445,210 | 12,887 µs |

Writing only changed pixels cuts screen writes during a drag about
fivefold. VirtualBox writes roughly 25–35 million pixels per second to the
screen on this host (its Hyper-V backend), so a full-screen change still
costs 13–35 ms there.
