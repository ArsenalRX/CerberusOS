// The dispatcher and the system calls themselves. See syscall.h for the
// rules every handler follows.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <drivers/refclock.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <lib/csprng.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/usercopy.h>
#include <mm/vmm.h>
#include <proc/process.h>
#include <sched/sched.h>
#include <syscall/abi.h>
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

constexpr usize IO_CHUNK = 4096;            // one page per step through the file layer
constexpr usize IO_MAX = 1 * MIB;               // per call; the caller loops for more
constexpr u64 SLEEP_MAX_MS = 1ull << 31;

constexpr u64 PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4;
constexpr u64 MAP_PRIVATE = 0x02, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20;
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

// ------------------------------------------------------------ file system --
// Paths come from user memory; descriptors index the process's table.

// Copies a user path. NameTooLong if it does not fit.
Error user_path(u64 uptr, char* out) {
    Result<usize> len = strncpy_from_user(out, uptr, PATH_MAX);
    if (!len.ok()) return len.error() == Error::TooBig ? Error::NameTooLong : len.error();
    return Error::None;
}

// The directory paths are resolved from: a directory descriptor for the
// *at calls, else the working directory (null = the root).
Result<Vnode*> start_dir(i64 dirfd) {
    if (dirfd == abi::AT_FDCWD) return self()->cwd;
    File* f = fd_file((u64)dirfd);
    if (!f) return Error::BadFd;
    if (f->vnode->type != VType::Dir) return Error::NotDir;
    return f->vnode;
}

// Installs `f` at the lowest free descriptor.
i64 install_fd(File* f, bool cloexec) {
    Process* p = self();
    for (usize fd = 0; fd < PROCESS_MAX_FDS; fd++) {
        if (p->files[fd]) continue;
        p->files[fd] = f;
        if (cloexec) p->fd_cloexec |= 1u << fd;
        else p->fd_cloexec &= ~(1u << fd);
        return (i64)fd;
    }
    file_unref(f);
    return -err::MFILE;
}

i64 do_open(i64 dirfd, u64 path, u64 flags, u64 mode) {
    char kpath[PATH_MAX];
    Error e = user_path(path, kpath);
    if (e != Error::None) return errno_of(e);
    Result<Vnode*> start = start_dir(dirfd);
    if (!start.ok()) return errno_of(start.error());
    Process* p = self();
    Result<File*> f = file_open(start.value(), kpath, (u32)flags, (u32)mode & ~p->umask & 07777, p->cred);
    if (!f.ok()) return errno_of(f.error());
    return install_fd(f.value(), flags & abi::O_CLOEXEC);
}

i64 sys_open(u64 path, u64 flags, u64 mode, u64, u64, u64, InterruptFrame*) {
    return do_open(abi::AT_FDCWD, path, flags, mode);
}

i64 sys_openat(u64 dirfd, u64 path, u64 flags, u64 mode, u64, u64, InterruptFrame*) {
    return do_open((i64)(i32)dirfd, path, flags, mode);
}

i64 sys_close(u64 fd, u64, u64, u64, u64, u64, InterruptFrame*) {
    File* f = fd_file(fd);
    if (!f) return -err::BADF;
    Process* p = self();
    p->files[fd] = nullptr;
    p->fd_cloexec &= ~(1u << fd);
    file_unref(f);
    return 0;
}

i64 sys_seek(u64 fd, u64 off, u64 whence, u64, u64, u64, InterruptFrame*) {
    File* f = fd_file(fd);
    if (!f) return -err::BADF;
    if (f->vnode->type == VType::CharDev) return -err::SPIPE;
    Result<u64> r = file_seek(f, (i64)off, (u32)whence);
    return r.ok() ? (i64)r.value() : errno_of(r.error());
}

i64 stat_path(u64 path, u64 out, bool follow) {
    char kpath[PATH_MAX];
    Error e = user_path(path, kpath);
    if (e != Error::None) return errno_of(e);
    LookupFlags lf;
    lf.follow_last = follow;
    Result<Vnode*> v = vfs_resolve(self()->cwd, kpath, self()->cred, lf);
    if (!v.ok()) return errno_of(v.error());
    abi::Stat st;
    file_stat(v.value(), &st);
    vnode_unref(v.value());
    return copy_to_user(out, &st, sizeof st).ok() ? 0 : -err::FAULT;
}

i64 sys_stat(u64 path, u64 out, u64, u64, u64, u64, InterruptFrame*) { return stat_path(path, out, true); }
i64 sys_lstat(u64 path, u64 out, u64, u64, u64, u64, InterruptFrame*) { return stat_path(path, out, false); }

i64 sys_fstat(u64 fd, u64 out, u64, u64, u64, u64, InterruptFrame*) {
    File* f = fd_file(fd);
    if (!f) return -err::BADF;
    abi::Stat st;
    file_stat(f->vnode, &st);
    return copy_to_user(out, &st, sizeof st).ok() ? 0 : -err::FAULT;
}

i64 make_object(u64 path, VType type, u32 mode, const char* target) {
    char kpath[PATH_MAX];
    Error e = user_path(path, kpath);
    if (e != Error::None) return errno_of(e);
    Process* p = self();
    Result<Vnode*> v = vfs_create(p->cwd, kpath, p->cred, type, mode & ~p->umask & 07777, true, target);
    if (!v.ok()) return errno_of(v.error());
    vnode_unref(v.value());
    return 0;
}

i64 sys_mkdir(u64 path, u64 mode, u64, u64, u64, u64, InterruptFrame*) {
    return make_object(path, VType::Dir, (u32)mode, nullptr);
}

i64 sys_symlink(u64 target, u64 path, u64, u64, u64, u64, InterruptFrame*) {
    char ktarget[PATH_MAX];
    Error e = user_path(target, ktarget);
    if (e != Error::None) return errno_of(e);
    if (!ktarget[0]) return -err::NOENT;
    return make_object(path, VType::Symlink, 0777, ktarget);
}

i64 remove_name(u64 path, bool dir) {
    char kpath[PATH_MAX];
    Error e = user_path(path, kpath);
    if (e != Error::None) return errno_of(e);
    Result<void> r = vfs_unlink(self()->cwd, kpath, self()->cred, dir);
    return r.ok() ? 0 : errno_of(r.error());
}

i64 sys_unlink(u64 path, u64, u64, u64, u64, u64, InterruptFrame*) { return remove_name(path, false); }
i64 sys_rmdir(u64 path, u64, u64, u64, u64, u64, InterruptFrame*) { return remove_name(path, true); }

i64 sys_rename(u64 from, u64 to, u64, u64, u64, u64, InterruptFrame*) {
    char kfrom[PATH_MAX], kto[PATH_MAX];
    Error e = user_path(from, kfrom);
    if (e == Error::None) e = user_path(to, kto);
    if (e != Error::None) return errno_of(e);
    Result<void> r = vfs_rename(self()->cwd, kfrom, kto, self()->cred);
    return r.ok() ? 0 : errno_of(r.error());
}

i64 sys_readdir(u64 fd, u64 out, u64 n, u64, u64, u64, InterruptFrame*) {
    File* f = fd_file(fd);
    if (!f) return -err::BADF;
    if (f->vnode->type != VType::Dir) return -err::NOTDIR;
    u64 done = 0;
    while (done < n) {
        abi::Dirent d;
        Result<bool> r = file_readdir(f, &d);
        if (!r.ok()) return done ? (i64)done : errno_of(r.error());
        if (!r.value()) break;
        if (!copy_to_user(out + done * sizeof d, &d, sizeof d).ok()) return done ? (i64)done : -err::FAULT;
        done++;
    }
    return (i64)done;
}

i64 sys_chdir(u64 path, u64, u64, u64, u64, u64, InterruptFrame*) {
    char kpath[PATH_MAX];
    Error e = user_path(path, kpath);
    if (e != Error::None) return errno_of(e);
    Process* p = self();
    Result<Vnode*> v = vfs_resolve(p->cwd, kpath, p->cred, LookupFlags{});
    if (!v.ok()) return errno_of(v.error());
    Result<void> ok = v.value()->type == VType::Dir ? vfs_access(v.value(), p->cred, vfs::X_OK)
                                                    : Result<void>(Error::NotDir);
    if (!ok.ok()) {
        vnode_unref(v.value());
        return errno_of(ok.error());
    }
    if (p->cwd) vnode_unref(p->cwd);
    p->cwd = v.value();
    return 0;
}

i64 sys_getcwd(u64 buf, u64 n, u64, u64, u64, u64, InterruptFrame*) {
    char kpath[PATH_MAX];
    Process* p = self();
    Result<void> r = vfs_path_of(p->cwd ? p->cwd : vfs_root(), kpath, sizeof kpath);
    if (!r.ok()) return errno_of(r.error());
    usize len = strlen(kpath) + 1;
    if (len > n) return -err::NAMETOOLONG;
    return copy_to_user(buf, kpath, len).ok() ? (i64)(len - 1) : -err::FAULT;
}

i64 sys_dup(u64 fd, u64, u64, u64, u64, u64, InterruptFrame*) {
    File* f = fd_file(fd);
    if (!f) return -err::BADF;
    return install_fd(file_ref(f), false);
}

i64 sys_dup2(u64 oldfd, u64 newfd, u64, u64, u64, u64, InterruptFrame*) {
    File* f = fd_file(oldfd);
    if (!f) return -err::BADF;
    if (newfd >= PROCESS_MAX_FDS) return -err::BADF;
    if (oldfd == newfd) return (i64)newfd;
    Process* p = self();
    File* old = p->files[newfd];
    p->files[newfd] = file_ref(f);
    p->fd_cloexec &= ~(1u << newfd);
    if (old) file_unref(old);
    return (i64)newfd;
}

i64 sys_ioctl(u64 fd, u64 request, u64 arg, u64, u64, u64, InterruptFrame*) {
    File* f = fd_file(fd);
    if (!f) return -err::BADF;
    Result<i64> r = vfs_ioctl(f->vnode, (u32)request, arg);
    return r.ok() ? r.value() : errno_of(r.error() == Error::NotSupported ? Error::NoDevice : r.error());
}

i64 sys_truncate(u64 fd, u64 size, u64, u64, u64, u64, InterruptFrame*) {
    File* f = fd_file(fd);
    if (!f) return -err::BADF;
    if (!file_writable(f)) return -err::BADF;
    if ((i64)size < 0) return -err::INVAL;
    Result<void> r = vfs_truncate(f->vnode, size);
    return r.ok() ? 0 : errno_of(r.error());
}

i64 sys_sync(u64, u64, u64, u64, u64, u64, InterruptFrame*) {
    vfs_sync();
    return 0;
}

i64 sys_fsync(u64 fd, u64, u64, u64, u64, u64, InterruptFrame*) {
    File* f = fd_file(fd);
    if (!f) return -err::BADF;
    Result<void> r = vfs_fsync(f->vnode);
    return r.ok() ? 0 : errno_of(r.error());
}

i64 sys_readlink(u64 path, u64 buf, u64 n, u64, u64, u64, InterruptFrame*) {
    char kpath[PATH_MAX];
    Error e = user_path(path, kpath);
    if (e != Error::None) return errno_of(e);
    LookupFlags lf;
    lf.follow_last = false;
    Result<Vnode*> v = vfs_resolve(self()->cwd, kpath, self()->cred, lf);
    if (!v.ok()) return errno_of(v.error());
    char target[PATH_MAX];
    Result<usize> r = vfs_readlink(v.value(), target, sizeof target);
    vnode_unref(v.value());
    if (!r.ok()) return errno_of(r.error());
    usize len = r.value() < n ? r.value() : n;
    return copy_to_user(buf, target, len).ok() ? (i64)len : -err::FAULT;
}

i64 attr_path(u64 path, bool follow, Vnode** out) {
    char kpath[PATH_MAX];
    Error e = user_path(path, kpath);
    if (e != Error::None) return errno_of(e);
    LookupFlags lf;
    lf.follow_last = follow;
    Result<Vnode*> v = vfs_resolve(self()->cwd, kpath, self()->cred, lf);
    if (!v.ok()) return errno_of(v.error());
    *out = v.value();
    return 0;
}

i64 sys_chmod(u64 path, u64 mode, u64, u64, u64, u64, InterruptFrame*) {
    Vnode* v;
    i64 r = attr_path(path, true, &v);
    if (r) return r;
    Result<void> c = vfs_chmod(v, self()->cred, (u32)mode);
    vnode_unref(v);
    return c.ok() ? 0 : errno_of(c.error());
}

i64 sys_chown(u64 path, u64 uid, u64 gid, u64, u64, u64, InterruptFrame*) {
    Vnode* v;
    i64 r = attr_path(path, true, &v);
    if (r) return r;
    Result<void> c = vfs_chown(v, self()->cred, (u32)uid, (u32)gid);
    vnode_unref(v);
    return c.ok() ? 0 : errno_of(c.error());
}

i64 sys_mount(u64 source, u64 target, u64 type, u64 flags, u64, u64, InterruptFrame*) {
    char ksource[PATH_MAX], ktarget[PATH_MAX], ktype[16];
    Error e = user_path(source, ksource);
    if (e == Error::None) e = user_path(target, ktarget);
    if (e != Error::None) return errno_of(e);
    Result<usize> tl = strncpy_from_user(ktype, type, sizeof ktype);
    if (!tl.ok()) return errno_of(tl.error() == Error::TooBig ? Error::NoDevice : tl.error());
    if (flags & ~(u64)(abi::MS_RDONLY | abi::MS_NOEXEC | abi::MS_NOSUID | abi::MS_NODEV)) return -err::INVAL;
    if (self()->cred.uid != 0) return -err::PERM;
    Result<Mount*> m = vfs_make_mount(ktype, ksource, (u32)flags);
    if (!m.ok()) return errno_of(m.error());
    Result<void> r = vfs_mount(m.value(), ktarget, self()->cred);
    if (!r.ok()) {
        // Not attached anywhere: hand it straight back to its file system.
        vnode_unref(m.value()->root);
        if (m.value()->unmount) (void)m.value()->unmount(m.value());
        return errno_of(r.error());
    }
    return 0;
}

i64 sys_umount(u64 target, u64, u64, u64, u64, u64, InterruptFrame*) {
    char ktarget[PATH_MAX];
    Error e = user_path(target, ktarget);
    if (e != Error::None) return errno_of(e);
    Result<void> r = vfs_unmount(ktarget, self()->cred);
    return r.ok() ? 0 : errno_of(r.error());
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

i64 sys_time_ms(u64, u64, u64, u64, u64, u64, InterruptFrame*) { return (i64)(refclock_now_us() / 1000); }

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
    case Error::Access: return -err::ACCES;
    case Error::ReadOnly: return -err::ROFS;
    case Error::NotEmpty: return -err::NOTEMPTY;
    case Error::NameTooLong: return -err::NAMETOOLONG;
    case Error::Loop: return -err::LOOP;
    case Error::CrossDevice: return -err::XDEV;
    case Error::NoDevice: return -err::NODEV;
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
