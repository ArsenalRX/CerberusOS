// The dispatcher and the system calls themselves. See syscall.h for the
// rules every handler follows.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <fs/file.h>
#include <lib/csprng.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/usercopy.h>
#include <mm/vmm.h>
#include <proc/process.h>
#include <sched/sched.h>
#include <syscall/syscall.h>

extern "C" void syscall_entry();

namespace {

constexpr u64 EFER_SCE = 1 << 0;
// Flags a user program may have set when it returns to ring 3: the
// arithmetic flags and the direction flag. Interrupts are forced on; the
// trap, I/O privilege, nested-task, alignment-check (SMAP override) and
// other system flags are forced off.
constexpr u64 RFLAGS_USER_KEEP = 0x0CD5;        // CF PF AF ZF SF DF OF
constexpr u64 RFLAGS_IF = 1 << 9, RFLAGS_FIXED = 1 << 1;
// Flags cleared on entry: interrupts, direction, trap, nested task, alignment check.
constexpr u64 SFMASK_VALUE = (1 << 9) | (1 << 10) | (1 << 8) | (1 << 14) | (1 << 18);

constexpr usize IO_CHUNK = 512;
constexpr usize IO_MAX = 1 * MIB;               // per call; the caller loops for more
constexpr u64 SLEEP_MAX_MS = 1ull << 31;

constexpr u64 PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4;
constexpr u64 MAP_PRIVATE = 0x02, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20;
constexpr u64 O_ACCMODE = 3;
constexpr u64 WNOHANG = 1;

inline Process* self() { return thread_current()->process; }

// The File behind a descriptor of the calling process, or null.
File* fd_file(u64 fd) {
    if (fd >= PROCESS_MAX_FDS) return nullptr;
    return self()->files[fd];
}

// --------------------------------------------------------------- handlers --
// Every handler receives the six raw argument registers and the frame.

i64 sys_exit(u64 status, u64, u64, u64, u64, u64, InterruptFrame*) {
    process_exit(wait_status_exited((int)(status & 0xFF)));
}

i64 sys_write(u64 fd, u64 buf, u64 n, u64, u64, u64, InterruptFrame*) {
    File* f = fd_file(fd);
    if (!f) return -err::BADF;
    if (n > IO_MAX) n = IO_MAX;
    u8 chunk[IO_CHUNK];
    usize done = 0;
    while (done < n) {
        usize take = n - done < IO_CHUNK ? n - done : IO_CHUNK;
        if (!copy_from_user(chunk, buf + done, take).ok()) return done ? (i64)done : -err::FAULT;
        Result<usize> w = file_write(f, chunk, take);
        if (!w.ok()) return done ? (i64)done : errno_of(w.error());
        done += w.value();
    }
    return (i64)done;
}

i64 sys_read(u64 fd, u64 buf, u64 n, u64, u64, u64, InterruptFrame*) {
    File* f = fd_file(fd);
    if (!f) return -err::BADF;
    if (n > IO_MAX) n = IO_MAX;
    u8 chunk[IO_CHUNK];
    usize done = 0;
    while (done < n) {
        usize want = n - done < IO_CHUNK ? n - done : IO_CHUNK;
        Result<usize> r = file_read(f, chunk, want);
        if (!r.ok()) return done ? (i64)done : errno_of(r.error());
        if (r.value() == 0) break;
        if (!copy_to_user(buf + done, chunk, r.value()).ok()) return done ? (i64)done : -err::FAULT;
        done += r.value();
        if (r.value() < want) break;
    }
    return (i64)done;
}

i64 sys_open(u64 path, u64 flags, u64, u64, u64, u64, InterruptFrame*) {
    if (flags & ~O_ACCMODE) return -err::INVAL;
    if (flags & O_ACCMODE) return -err::PERM;       // the boot archive is read-only
    char kpath[PATH_MAX];
    Result<usize> len = strncpy_from_user(kpath, path, sizeof kpath);
    if (!len.ok()) return errno_of(len.error());
    Process* p = self();
    usize fd = 0;
    while (fd < PROCESS_MAX_FDS && p->files[fd]) fd++;
    if (fd == PROCESS_MAX_FDS) return -err::MFILE;
    Result<File*> f = file_open_archive(kpath);
    if (!f.ok()) return errno_of(f.error());
    p->files[fd] = f.value();
    return (i64)fd;
}

i64 sys_close(u64 fd, u64, u64, u64, u64, u64, InterruptFrame*) {
    File* f = fd_file(fd);
    if (!f) return -err::BADF;
    self()->files[fd] = nullptr;
    file_unref(f);
    return 0;
}

i64 sys_mmap(u64 hint, u64 len, u64 prot, u64 flags, u64 fd, u64 off, InterruptFrame*) {
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -err::INVAL;
    if (flags & ~(MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS)) return -err::INVAL;
    if (!(flags & MAP_PRIVATE) || !(flags & MAP_ANONYMOUS)) return -err::INVAL;     // no file mappings yet
    if ((i64)fd != -1 || off != 0 || len == 0) return -err::INVAL;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return -err::INVAL;             // W^X
    if (len > USER_MAX) return -err::NOMEM;
    u32 vm_prot = (prot & PROT_WRITE ? vm::WRITE : 0) | (prot & PROT_EXEC ? vm::EXEC : 0);
    Process* p = self();
    Result<vaddr_t> r = (flags & MAP_FIXED) ? p->space->mmap(hint, len, vm_prot, mmap_flag::FIXED)
                                            : p->space->mmap(p->mmap_hint, len, vm_prot, 0);
    if (!r.ok()) return errno_of(r.error() == Error::NoSpace ? Error::NoMemory : r.error());
    return (i64)r.value();
}

i64 sys_munmap(u64 addr, u64 len, u64, u64, u64, u64, InterruptFrame*) {
    Result<void> r = self()->space->munmap(addr, len);
    return r.ok() ? 0 : errno_of(r.error());
}

i64 sys_fork(u64, u64, u64, u64, u64, u64, InterruptFrame* f) {
    Result<i64> r = process_fork(f);
    return r.ok() ? r.value() : errno_of(r.error());
}

// Copies a NULL-terminated array of user string pointers into `args`.
Error copy_string_array(ExecArgs* args, vaddr_t array, bool env) {
    if (!array) return Error::None;
    char* scratch = (char*)kmalloc(EXEC_MAX_ARG_BYTES);
    if (!scratch) return Error::NoMemory;
    Error e = Error::None;
    for (usize i = 0;; i++) {
        if (i >= EXEC_MAX_STRINGS) {
            e = Error::TooBig;
            break;
        }
        u64 ptr = 0;
        e = copy_from_user(&ptr, array + i * sizeof(u64), sizeof ptr).error();
        if (e != Error::None || ptr == 0) break;
        Result<usize> len = strncpy_from_user(scratch, ptr, EXEC_MAX_ARG_BYTES);
        e = len.ok() ? exec_args_add(args, scratch, env).error() : len.error();
        if (e != Error::None) break;
    }
    kfree(scratch);
    return e;
}

i64 sys_execve(u64 path, u64 argv, u64 envp, u64, u64, u64, InterruptFrame* f) {
    char kpath[PATH_MAX];
    Result<usize> len = strncpy_from_user(kpath, path, sizeof kpath);
    if (!len.ok()) return errno_of(len.error());
    ExecArgs args;
    if (!exec_args_init(&args).ok()) return -err::NOMEM;
    Error e = copy_string_array(&args, argv, false);
    if (e == Error::None) e = copy_string_array(&args, envp, true);
    if (e == Error::None && args.argc == 0) e = exec_args_add(&args, kpath, false).error();
    if (e == Error::None) e = process_exec(f, kpath, args).error();
    exec_args_free(&args);
    return e == Error::None ? 0 : errno_of(e);
}

i64 sys_waitpid(u64 pid, u64 status, u64 flags, u64, u64, u64, InterruptFrame*) {
    if (flags & ~WNOHANG) return -err::INVAL;
    // Probe the status pointer before reaping, so a bad pointer cannot make
    // a child's exit status disappear.
    if (status && !clear_user(status, sizeof(int)).ok()) return -err::FAULT;
    int st = 0;
    Result<i64> r = process_waitpid((i64)(i32)pid, &st, flags & WNOHANG);
    if (!r.ok()) return errno_of(r.error());
    if (r.value() > 0 && status && !copy_to_user(status, &st, sizeof st).ok()) return -err::FAULT;
    return r.value();
}

i64 sys_getpid(u64, u64, u64, u64, u64, u64, InterruptFrame*) { return self()->pid; }

i64 sys_yield(u64, u64, u64, u64, u64, u64, InterruptFrame*) {
    thread_yield();
    return 0;
}

i64 sys_sleep_ms(u64 ms, u64, u64, u64, u64, u64, InterruptFrame*) {
    if (ms > SLEEP_MAX_MS) return -err::INVAL;
    thread_sleep_ms(ms);
    return 0;
}

i64 sys_getrandom(u64 buf, u64 n, u64 flags, u64, u64, u64, InterruptFrame*) {
    if (flags) return -err::INVAL;
    if (n > IO_MAX) n = IO_MAX;
    u8 chunk[256];
    usize done = 0;
    while (done < n) {
        usize take = n - done < sizeof chunk ? n - done : sizeof chunk;
        csprng_bytes(chunk, take);
        bool ok = copy_to_user(buf + done, chunk, take).ok();
        memset(chunk, 0, sizeof chunk);
        if (!ok) return done ? (i64)done : -err::FAULT;
        done += take;
    }
    return (i64)done;
}

// Generated from table.def: one case per implemented call. A switch keeps
// the dispatch in read-only code (no writable table of function pointers)
// and makes an out-of-range number fall through to ENOSYS.
constexpr u64 SYSCALL_NUMBERS[] = {
#define SYSCALL(number, name, proto, doc) number,
#include <syscall/table.def>
#undef SYSCALL
};
constexpr u64 syscall_limit() {
    u64 limit = 0;
    for (u64 n : SYSCALL_NUMBERS)
        if (n >= limit) limit = n + 1;
    return limit;
}

i64 dispatch(u64 nr, InterruptFrame* f) {
    // The number is masked without a branch (all ones when below the limit,
    // zero otherwise) before the range check, so that a mispredicted check
    // cannot steer the switch with an out-of-range value (SPEC §19.3,
    // speculation). The empty asm keeps the compiler from proving the mask
    // redundant and removing it.
    constexpr u64 LIMIT = syscall_limit();
    u64 mask = ~(u64)((i64)(nr | (LIMIT - 1 - nr)) >> 63);
    asm volatile("" : "+r"(mask));
    if (nr >= LIMIT) return -err::NOSYS;
    nr &= mask;
    switch (nr) {
#define SYSCALL(number, name, proto, doc) \
    case number: return sys_##name(f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9, f);
#include <syscall/table.def>
#undef SYSCALL
    default: return -err::NOSYS;
    }
}

} // namespace

i64 errno_of(Error e) {
    switch (e) {
    case Error::None: return 0;
    case Error::NoMemory: return -err::NOMEM;
    case Error::NotFound: return -err::NOENT;
    case Error::Exists: return -err::EXIST;
    case Error::Invalid: return -err::INVAL;
    case Error::Perm: return -err::PERM;
    case Error::Busy: return -err::BUSY;
    case Error::Fault: return -err::FAULT;
    case Error::NoSpace: return -err::NOSPC;
    case Error::IsDir: return -err::ISDIR;
    case Error::NotDir: return -err::NOTDIR;
    case Error::Again: return -err::AGAIN;
    case Error::Interrupted: return -err::INTR;
    case Error::IO: return -err::IO;
    case Error::NotSupported: return -err::NOSYS;
    case Error::TooBig: return -err::TOOBIG;
    case Error::Deadlock: return -err::DEADLK;
    case Error::Timeout: return -err::TIMEDOUT;
    case Error::BadFd: return -err::BADF;
    case Error::NoChild: return -err::CHILD;
    case Error::NoProcess: return -err::SRCH;
    case Error::TooManyFiles: return -err::MFILE;
    case Error::NotExecutable: return -err::NOEXEC;
    }
    return -err::INVAL;
}

void syscall_init() {
    wrmsr(msr::EFER, rdmsr(msr::EFER) | EFER_SCE);
    // syscall loads CS/SS from STAR[47:32]; sysret loads CS from STAR[63:48]
    // + 16 and SS from STAR[63:48] + 8 (see gdt.h for the GDT layout that
    // makes this work). The field must already carry ring 3 in its low bits:
    // Intel forces them on both selectors, AMD does not force them on SS,
    // and a user SS of 0x20 instead of 0x23 faults on the next iretq.
    wrmsr(msr::STAR, ((u64)(seg::UCODE32 | 3) << 48) | ((u64)seg::KCODE << 32));
    wrmsr(msr::LSTAR, (u64)syscall_entry);
    wrmsr(msr::SFMASK, SFMASK_VALUE);
}

extern "C" void syscall_dispatch(InterruptFrame* f) {
    f->rax = (u64)dispatch(f->rax, f);

    // Whatever the handler did to the frame (execve rewrites it), what goes
    // back to ring 3 must be a user context: sysret with a non-canonical rip
    // would fault in ring 0 on the user's stack.
    if (f->rip >= USER_MAX) {
        kprintf("process %s (pid %u) killed: return address %#lx is outside user space\n", self()->name,
                self()->pid, (unsigned long)f->rip);
        process_exit(WAIT_SIGSEGV);
    }
    f->rflags = (f->rflags & RFLAGS_USER_KEEP) | RFLAGS_IF | RFLAGS_FIXED;
    f->cs = seg::UCODE;
    f->ss = seg::UDATA;
}
