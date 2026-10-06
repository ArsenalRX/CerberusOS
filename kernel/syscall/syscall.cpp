// The dispatcher and the system calls themselves. See syscall.h for the
// rules every handler follows.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/power.h>
#include <drivers/refclock.h>
#include <fs/file.h>
#include <fs/pagecache.h>
#include <fs/vfs.h>
#include <ipc/event.h>
#include <ipc/futex.h>
#include <ipc/object.h>
#include <ipc/port.h>
#include <ipc/shm.h>
#include <lib/csprng.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/usercopy.h>
#include <mm/vmm.h>
#include <proc/process.h>
#include <proc/signal.h>
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

// The File behind a descriptor of the calling process (null if there is
// none), referenced for as long as this object lives: the process's other
// threads may close the descriptor at any moment.
struct FdRef {
    File* f;
    explicit FdRef(u64 fd) : f(fd_get(fd)) {}
    ~FdRef() {
        if (f) file_unref(f);
    }
    FdRef(const FdRef&) = delete;
    FdRef& operator=(const FdRef&) = delete;
    operator File*() const { return f; }
    File* operator->() const { return f; }
};

// The working directory, likewise (null = the root).
struct CwdRef {
    Vnode* v;
    CwdRef() : v(cwd_get()) {}
    ~CwdRef() {
        if (v) vnode_unref(v);
    }
    CwdRef(const CwdRef&) = delete;
    CwdRef& operator=(const CwdRef&) = delete;
    operator Vnode*() const { return v; }
};

// Installs `f` (taking the reference) at the lowest free descriptor.
i64 install_fd(File* f, bool cloexec) {
    Result<int> fd = fd_install(f, cloexec);
    return fd.ok() ? fd.value() : errno_of(fd.error());
}

// Installs a new object's descriptor, or passes its error on.
i64 install_new(Result<File*> f, bool cloexec = false) {
    return f.ok() ? install_fd(f.value(), cloexec) : errno_of(f.error());
}

// --------------------------------------------------------------- handlers --
// Every handler receives the six raw argument registers and the frame.

i64 sys_exit(u64 status, u64, u64, u64, u64, u64, InterruptFrame*) {
    process_exit(wait_status_exited((int)(status & 0xFF)));
}

i64 sys_write(u64 fd, u64 buf, u64 n, u64, u64, u64, InterruptFrame*) {
    FdRef f(fd);
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
    FdRef f(fd);
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

// The directory paths are resolved from, with a reference (null = the
// root): a directory descriptor for the *at calls, else the working
// directory.
Result<Vnode*> start_dir(i64 dirfd) {
    if (dirfd == abi::AT_FDCWD) return cwd_get();
    FdRef f((u64)dirfd);
    if (!f) return Error::BadFd;
    if (f->vnode->type != VType::Dir) return Error::NotDir;
    return vnode_ref(f->vnode);
}

i64 do_open(i64 dirfd, u64 path, u64 flags, u64 mode) {
    char kpath[PATH_MAX];
    Error e = user_path(path, kpath);
    if (e != Error::None) return errno_of(e);
    Result<Vnode*> start = start_dir(dirfd);
    if (!start.ok()) return errno_of(start.error());
    Process* p = self();
    Result<File*> f = file_open(start.value(), kpath, (u32)flags, (u32)mode & ~p->umask & 07777, p->cred);
    if (start.value()) vnode_unref(start.value());
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
    File* f = fd_take(fd);
    if (!f) return -err::BADF;
    file_unref(f);
    return 0;
}

i64 sys_seek(u64 fd, u64 off, u64 whence, u64, u64, u64, InterruptFrame*) {
    FdRef f(fd);
    if (!f) return -err::BADF;
    Result<u64> r = file_seek(f, (i64)off, (u32)whence);
    return r.ok() ? (i64)r.value() : errno_of(r.error());
}

i64 stat_path(u64 path, u64 out, bool follow) {
    char kpath[PATH_MAX];
    Error e = user_path(path, kpath);
    if (e != Error::None) return errno_of(e);
    LookupFlags lf;
    lf.follow_last = follow;
    Result<Vnode*> v = vfs_resolve(CwdRef(), kpath, self()->cred, lf);
    if (!v.ok()) return errno_of(v.error());
    abi::Stat st;
    file_stat(v.value(), &st);
    vnode_unref(v.value());
    return copy_to_user(out, &st, sizeof st).ok() ? 0 : -err::FAULT;
}

i64 sys_stat(u64 path, u64 out, u64, u64, u64, u64, InterruptFrame*) { return stat_path(path, out, true); }
i64 sys_lstat(u64 path, u64 out, u64, u64, u64, u64, InterruptFrame*) { return stat_path(path, out, false); }

i64 sys_fstat(u64 fd, u64 out, u64, u64, u64, u64, InterruptFrame*) {
    FdRef f(fd);
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
    Result<Vnode*> v = vfs_create(CwdRef(), kpath, p->cred, type, mode & ~p->umask & 07777, true, target);
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
    Result<void> r = vfs_unlink(CwdRef(), kpath, self()->cred, dir);
    return r.ok() ? 0 : errno_of(r.error());
}

i64 sys_unlink(u64 path, u64, u64, u64, u64, u64, InterruptFrame*) { return remove_name(path, false); }
i64 sys_rmdir(u64 path, u64, u64, u64, u64, u64, InterruptFrame*) { return remove_name(path, true); }

i64 sys_rename(u64 from, u64 to, u64, u64, u64, u64, InterruptFrame*) {
    char kfrom[PATH_MAX], kto[PATH_MAX];
    Error e = user_path(from, kfrom);
    if (e == Error::None) e = user_path(to, kto);
    if (e != Error::None) return errno_of(e);
    Result<void> r = vfs_rename(CwdRef(), kfrom, kto, self()->cred);
    return r.ok() ? 0 : errno_of(r.error());
}

i64 sys_readdir(u64 fd, u64 out, u64 n, u64, u64, u64, InterruptFrame*) {
    FdRef f(fd);
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
    Result<Vnode*> v = vfs_resolve(CwdRef(), kpath, p->cred, LookupFlags{});
    if (!v.ok()) return errno_of(v.error());
    Result<void> ok = v.value()->type == VType::Dir ? vfs_access(v.value(), p->cred, vfs::X_OK)
                                                    : Result<void>(Error::NotDir);
    if (!ok.ok()) {
        vnode_unref(v.value());
        return errno_of(ok.error());
    }
    cwd_set(v.value());
    return 0;
}

i64 sys_getcwd(u64 buf, u64 n, u64, u64, u64, u64, InterruptFrame*) {
    char kpath[PATH_MAX];
    CwdRef cwd;
    Result<void> r = vfs_path_of(cwd.v ? cwd.v : vfs_root(), kpath, sizeof kpath);
    if (!r.ok()) return errno_of(r.error());
    usize len = strlen(kpath) + 1;
    if (len > n) return -err::NAMETOOLONG;
    return copy_to_user(buf, kpath, len).ok() ? (i64)(len - 1) : -err::FAULT;
}

i64 sys_dup(u64 fd, u64, u64, u64, u64, u64, InterruptFrame*) {
    FdRef f(fd);
    if (!f) return -err::BADF;
    return install_fd(file_ref(f), false);
}

i64 sys_dup2(u64 oldfd, u64 newfd, u64, u64, u64, u64, InterruptFrame*) {
    FdRef f(oldfd);
    if (!f) return -err::BADF;
    if (newfd >= PROCESS_MAX_FDS) return -err::BADF;
    if (oldfd == newfd) return (i64)newfd;
    (void)fd_replace(newfd, file_ref(f));
    return (i64)newfd;
}

i64 sys_ioctl(u64 fd, u64 request, u64 arg, u64, u64, u64, InterruptFrame*) {
    FdRef f(fd);
    if (!f) return -err::BADF;
    Result<i64> r = vfs_ioctl(f->vnode, (u32)request, arg);
    return r.ok() ? r.value() : errno_of(r.error() == Error::NotSupported ? Error::NoDevice : r.error());
}

i64 sys_truncate(u64 fd, u64 size, u64, u64, u64, u64, InterruptFrame*) {
    FdRef f(fd);
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
    FdRef f(fd);
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
    Result<Vnode*> v = vfs_resolve(CwdRef(), kpath, self()->cred, lf);
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
    Result<Vnode*> v = vfs_resolve(CwdRef(), kpath, self()->cred, lf);
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
    if (!(flags & MAP_PRIVATE) || len == 0) return -err::INVAL;        // shared mappings are shm_map's job
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return -err::INVAL;             // W^X
    if (len > USER_MAX) return -err::NOMEM;
    u32 vm_prot = (prot & PROT_WRITE ? vm::WRITE : 0) | (prot & PROT_EXEC ? vm::EXEC : 0);
    Process* p = self();
    if (flags & MAP_ANONYMOUS) {
        if ((i64)fd != -1 || off != 0) return -err::INVAL;
        Result<vaddr_t> r = (flags & MAP_FIXED) ? p->space->mmap(hint, len, vm_prot, mmap_flag::FIXED)
                                                : p->space->mmap(p->mmap_hint, len, vm_prot, 0);
        if (!r.ok()) return errno_of(r.error() == Error::NoSpace ? Error::NoMemory : r.error());
        return (i64)r.value();
    }

    // A private mapping of a file: the caller's own copy of that part of
    // the file, read in now. Bytes past the end of the file are zero.
    FdRef f(fd);
    if (!f) return -err::BADF;
    Vnode* v = f->vnode;
    if (v->type != VType::File) return -err::NODEV;
    if (!file_readable(f)) return -err::ACCES;
    if ((off & (PAGE_SIZE - 1)) || (i64)off < 0 || off + len < off) return -err::INVAL;
    if ((prot & PROT_EXEC) && !vfs_access(v, p->cred, vfs::X_OK).ok()) return -err::ACCES;
    // Mapped writable to fill it, then dropped to what was asked for.
    Result<vaddr_t> made = (flags & MAP_FIXED) ? p->space->mmap(hint, len, vm::WRITE, mmap_flag::FIXED)
                                               : p->space->mmap(p->mmap_hint, len, vm::WRITE, 0);
    if (!made.ok()) return errno_of(made.error() == Error::NoSpace ? Error::NoMemory : made.error());
    vaddr_t at = made.value();
    constexpr usize CHUNK = 64 * KIB;
    u8* chunk = (u8*)kmalloc(CHUNK);
    Error e = chunk ? Error::None : Error::NoMemory;
    for (u64 done = 0; e == Error::None && done < len;) {
        usize want = len - done < CHUNK ? len - done : CHUNK;
        Result<usize> got = vfs_read(v, off + done, chunk, want);
        if (!got.ok()) e = got.error();
        else if (got.value() == 0) break;           // the end of the file
        else if (!copy_to_user(at + done, chunk, got.value()).ok()) e = Error::NoMemory;
        else done += got.value();
    }
    kfree(chunk);
    if (e == Error::None && vm_prot != vm::WRITE) e = p->space->mprotect(at, align_up(len, PAGE_SIZE), vm_prot).error();
    if (e != Error::None) {
        (void)p->space->munmap(at, align_up(len, PAGE_SIZE));
        return errno_of(e);
    }
    return (i64)at;
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
    // Whole ticks, plus one because the current tick is partly over.
    WaitResult r = thread_sleep_interruptible((ms + SCHED_TICK_MS - 1) / SCHED_TICK_MS + 1);
    return r == WaitResult::Interrupted ? -err::INTR : 0;
}

i64 sys_time_ms(u64, u64, u64, u64, u64, u64, InterruptFrame*) { return (i64)(refclock_now_us() / 1000); }
i64 sys_time_us(u64, u64, u64, u64, u64, u64, InterruptFrame*) { return (i64)refclock_now_us(); }

i64 sys_sysinfo(u64 out, u64, u64, u64, u64, u64, InterruptFrame*) {
    abi::SysInfo info{};
    PmmStats mem = pmm_stats();
    PageCacheStats cache = page_cache_stats();
    SchedStats sched = sched_stats();
    info.uptime_ms = refclock_now_us() / 1000;
    info.mem_total = mem.usable_frames * PAGE_SIZE;
    info.mem_free = mem.free_frames * PAGE_SIZE;
    info.cache_pages = cache.pages;
    info.cache_hits = cache.hits;
    info.cache_misses = cache.misses;
    info.ticks = sched.ticks;
    info.idle_ticks = sched.idle_ticks;
    info.context_switches = sched.context_switches;
    info.cpus = sched.cpus;
    info.threads = sched.threads;
    info.processes = sched.processes;
    info.page_size = PAGE_SIZE;
    return copy_to_user(out, &info, sizeof info).ok() ? 0 : -err::FAULT;
}

// ------------------------------------------------- memory, part 2 (phase 11) --

i64 sys_mprotect(u64 addr, u64 len, u64 prot, u64, u64, u64, InterruptFrame*) {
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -err::INVAL;
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) return -err::INVAL;             // W^X
    u32 vm_prot = (prot & PROT_WRITE ? vm::WRITE : 0) | (prot & PROT_EXEC ? vm::EXEC : 0);
    Result<void> r = self()->space->mprotect(addr, len, vm_prot);
    return r.ok() ? 0 : errno_of(r.error());
}

// ------------------------------------------------------ processes, part 2 --

i64 sys_getppid(u64, u64, u64, u64, u64, u64, InterruptFrame*) {
    u64 irq = sched_lock();
    i64 pid = self()->parent->pid;
    sched_unlock(irq);
    return pid;
}

i64 sys_kill(u64 pid, u64 signo, u64, u64, u64, u64, InterruptFrame*) {
    if ((i64)(i32)pid <= 0 || signo >= (u64)sig::MAX) return -err::INVAL;
    Result<void> r = signal_send((u32)pid, (int)signo, self()->cred);
    return r.ok() ? 0 : errno_of(r.error());
}

i64 sys_signal(u64 signo, u64 handler, u64, u64, u64, u64, InterruptFrame*) {
    if (signo >= (u64)sig::MAX) return -err::INVAL;
    Result<u64> r = signal_set_handler((int)signo, handler);
    return r.ok() ? (i64)r.value() : errno_of(r.error());
}

i64 sys_sigreturn(u64, u64, u64, u64, u64, u64, InterruptFrame* f) { return signal_return(f); }

i64 sys_thread_spawn(u64 entry, u64 arg, u64 stack, u64 tls, u64, u64, InterruptFrame*) {
    // The FS base is loaded with a privileged instruction that faults on a
    // non-canonical value; the others would only fault in the new thread.
    if (entry >= USER_MAX || stack >= USER_MAX || tls >= USER_MAX) return -err::INVAL;
    Result<u32> r = process_thread_spawn(entry, arg, stack, tls);
    return r.ok() ? (i64)r.value() : errno_of(r.error());
}

i64 sys_thread_exit(u64 code, u64, u64, u64, u64, u64, InterruptFrame*) { process_thread_exit((int)(code & 0xFF)); }

i64 sys_thread_join(u64 tid, u64 code, u64, u64, u64, u64, InterruptFrame*) {
    if (code && !clear_user(code, sizeof(int)).ok()) return -err::FAULT;
    Result<int> r = thread_join_user((u32)tid);
    if (!r.ok()) return errno_of(r.error() == Error::NoProcess ? Error::NoProcess : r.error());
    int value = r.value();
    if (code && !copy_to_user(code, &value, sizeof value).ok()) return -err::FAULT;
    return 0;
}

i64 sys_set_tls(u64 base, u64, u64, u64, u64, u64, InterruptFrame*) {
    if (base >= USER_MAX) return -err::INVAL;
    thread_set_fs_base(base);
    return 0;
}

i64 sys_futex_wait(u64 addr, u64 val, u64 timeout_ms, u64, u64, u64, InterruptFrame*) {
    Result<void> r = futex_wait(addr, (u32)val, timeout_ms);
    return r.ok() ? 0 : errno_of(r.error());
}

i64 sys_futex_wake(u64 addr, u64 count, u64, u64, u64, u64, InterruptFrame*) {
    Result<u32> r = futex_wake(addr, (u32)count);
    return r.ok() ? (i64)r.value() : errno_of(r.error());
}

// ---------------------------------------------------------------- identity --

i64 sys_getuid(u64, u64, u64, u64, u64, u64, InterruptFrame*) { return self()->cred.uid; }
i64 sys_getgid(u64, u64, u64, u64, u64, u64, InterruptFrame*) { return self()->cred.gid; }
// There is one identity per process: the effective one is the real one.
i64 sys_geteuid(u64, u64, u64, u64, u64, u64, InterruptFrame*) { return self()->cred.uid; }
i64 sys_getegid(u64, u64, u64, u64, u64, u64, InterruptFrame*) { return self()->cred.gid; }

i64 sys_setuid(u64 uid, u64, u64, u64, u64, u64, InterruptFrame*) {
    Process* p = self();
    if (uid > 0xFFFFFFFEull) return -err::INVAL;
    if (p->cred.uid != 0 && p->cred.uid != (u32)uid) return -err::PERM;
    p->cred.uid = (u32)uid;         // one way: a process that gives root up cannot take it back
    return 0;
}

i64 sys_setgid(u64 gid, u64, u64, u64, u64, u64, InterruptFrame*) {
    Process* p = self();
    if (gid > 0xFFFFFFFEull) return -err::INVAL;
    if (p->cred.uid != 0 && p->cred.gid != (u32)gid) return -err::PERM;
    p->cred.gid = (u32)gid;
    return 0;
}

i64 sys_umask(u64 mask, u64, u64, u64, u64, u64, InterruptFrame*) {
    Process* p = self();
    return (i64)__atomic_exchange_n(&p->umask, (u32)mask & 0777, __ATOMIC_RELAXED);
}

// ------------------------------------------------------------------- ports --

Error user_port_name(u64 uptr, char* out) {
    Result<usize> len = strncpy_from_user(out, uptr, port::NAME_MAX + 1);
    if (!len.ok()) return len.error() == Error::TooBig ? Error::NameTooLong : len.error();
    return Error::None;
}

i64 sys_port_create(u64 name, u64 mode, u64, u64, u64, u64, InterruptFrame*) {
    char kname[port::NAME_MAX + 1];
    Error e = user_port_name(name, kname);
    if (e != Error::None) return errno_of(e);
    if (mode & ~0777ull) return -err::INVAL;
    Process* p = self();
    return install_new(port_create(kname, (u32)mode, p->cred, p->pid));
}

i64 sys_port_connect(u64 name, u64, u64, u64, u64, u64, InterruptFrame*) {
    char kname[port::NAME_MAX + 1];
    Error e = user_port_name(name, kname);
    if (e != Error::None) return errno_of(e == Error::NameTooLong ? Error::NotFound : e);
    Process* p = self();
    return install_new(port_connect(kname, p->cred, p->pid));
}

i64 port_send_common(u64 fd, u64 msg, u64 len, u64 fds, u64 nfds, bool nonblock) {
    if (len > port::MESSAGE_MAX) return -err::MSGSIZE;
    if (nfds > port::FDS_MAX) return -err::INVAL;
    FdRef f(fd);
    if (!f) return -err::BADF;
    i32 numbers[port::FDS_MAX];
    if (nfds && !copy_from_user(numbers, fds, nfds * sizeof(i32)).ok()) return -err::FAULT;
    u8* data = (u8*)kmalloc(len ? len : 1);
    if (!data) return -err::NOMEM;
    if (len && !copy_from_user(data, msg, len).ok()) {
        kfree(data);
        return -err::FAULT;
    }
    File* files[port::FDS_MAX];
    for (u32 i = 0; i < nfds; i++) {
        files[i] = numbers[i] >= 0 ? fd_get((u64)numbers[i]) : nullptr;
        if (files[i]) continue;
        while (i--) file_unref(files[i]);
        kfree(data);
        return -err::BADF;
    }
    Result<void> r = port_send(f, data, len, files, (u32)nfds, nonblock);     // takes the buffer and the references
    return r.ok() ? 0 : errno_of(r.error());
}

i64 sys_port_send(u64 fd, u64 msg, u64 len, u64 fds, u64 nfds, u64, InterruptFrame*) {
    return port_send_common(fd, msg, len, fds, nfds, false);
}

i64 sys_sigprocmask(u64 how, u64 set_ptr, u64 old_ptr, u64, u64, u64, InterruptFrame*) {
    u64 set = 0;
    if (set_ptr && !copy_from_user(&set, set_ptr, sizeof set).ok()) return -err::FAULT;
    Result<u64> old = signal_set_mask((int)how, set, set_ptr != 0);
    if (!old.ok()) return errno_of(old.error());
    if (old_ptr) {
        u64 o = old.value();
        if (!copy_to_user(old_ptr, &o, sizeof o).ok()) return -err::FAULT;
    }
    return 0;
}

i64 sys_port_try_send(u64 fd, u64 msg, u64 len, u64 fds, u64 nfds, u64, InterruptFrame*) {
    return port_send_common(fd, msg, len, fds, nfds, true);
}

i64 sys_port_recv(u64 fd, u64 buf, u64 len, u64 fds, u64 nfds_ptr, u64 timeout_ms, InterruptFrame*) {
    FdRef f(fd);
    if (!f) return -err::BADF;
    // How many descriptors the caller has room for; the count that arrived
    // is written back to the same place.
    i32 room = 0;
    if (nfds_ptr && !copy_from_user(&room, nfds_ptr, sizeof room).ok()) return -err::FAULT;
    if (room < 0 || !fds) room = 0;
    if (room > (i32)port::FDS_MAX) room = port::FDS_MAX;
    if (len > port::MESSAGE_MAX) len = port::MESSAGE_MAX;
    // Probe the buffers first: a message taken off the queue cannot be put back.
    if ((len && !clear_user(buf, len).ok()) || (room && !clear_user(fds, (u64)room * sizeof(i32)).ok()))
        return -err::FAULT;

    PortMessage m;
    Result<void> r = port_recv(f, len, (u32)room, timeout_ms, &m);
    if (!r.ok()) return errno_of(r.error() == Error::TooBig ? Error::NoSpace : r.error());
    i64 result = (i64)m.len;
    if (m.len && !copy_to_user(buf, m.data, m.len).ok()) result = -err::FAULT;
    kfree(m.data);
    i32 numbers[port::FDS_MAX];
    u32 installed = 0;
    for (u32 i = 0; i < m.nfds; i++) {
        if (result < 0) {
            file_unref(m.fds[i]);
            continue;
        }
        Result<int> slot = fd_install(m.fds[i], false);     // drops the reference if it fails
        if (slot.ok()) numbers[installed++] = slot.value();
        else result = -err::MFILE;
    }
    i32 count = (i32)installed;
    if (result >= 0 && installed && !copy_to_user(fds, numbers, installed * sizeof(i32)).ok()) result = -err::FAULT;
    if (result >= 0 && nfds_ptr && !copy_to_user(nfds_ptr, &count, sizeof count).ok()) result = -err::FAULT;
    if (result < 0)
        for (u32 i = 0; i < installed; i++)
            if (File* dropped = fd_take((u64)numbers[i])) file_unref(dropped);
    return result;
}

i64 sys_port_peer(u64 fd, u64 out, u64, u64, u64, u64, InterruptFrame*) {
    FdRef f(fd);
    if (!f) return -err::BADF;
    Result<PortPeer> r = port_peer(f);
    if (!r.ok()) return errno_of(r.error());
    PortPeer peer = r.value();
    return copy_to_user(out, &peer, sizeof peer).ok() ? 0 : -err::FAULT;
}

// ----------------------------------------------------------- shared memory --

i64 sys_shm_create(u64 size, u64, u64, u64, u64, u64, InterruptFrame*) { return install_new(shm_create(size)); }

i64 sys_shm_map(u64 fd, u64 prot, u64, u64, u64, u64, InterruptFrame*) {
    if (prot & ~(PROT_READ | PROT_WRITE)) return -err::INVAL;
    FdRef f(fd);
    if (!f) return -err::BADF;
    Result<vaddr_t> r = shm_map(f, prot & PROT_WRITE);
    if (!r.ok()) return errno_of(r.error() == Error::NoSpace ? Error::NoMemory : r.error());
    return (i64)r.value();
}

i64 sys_shm_unmap(u64 addr, u64, u64, u64, u64, u64, InterruptFrame*) {
    Result<void> r = shm_unmap(addr);
    return r.ok() ? 0 : errno_of(r.error());
}

// ------------------------------------------------------------ event queues --

i64 sys_event_create(u64 flags, u64, u64, u64, u64, u64, InterruptFrame*) {
    if (flags & ~(u64)abi::O_CLOEXEC) return -err::INVAL;
    return install_new(event_create(), flags & abi::O_CLOEXEC);
}

i64 sys_event_ctl(u64 ev, u64 op, u64 fd, u64 e, u64, u64, InterruptFrame*) {
    FdRef q(ev);
    if (!q) return -err::BADF;
    event::Event ke{};
    if (op != event::OP_DEL && !copy_from_user(&ke, e, sizeof ke).ok()) return -err::FAULT;
    if (ke.events & ~(poll::READ | poll::WRITE | poll::HUP | poll::TIMER | poll::CHILD)) return -err::INVAL;
    if (ke.timeout_ms > SLEEP_MAX_MS) return -err::INVAL;
    Result<void> r = event_ctl(q, (u32)op, (i32)fd, ke);
    return r.ok() ? 0 : errno_of(r.error());
}

i64 sys_event_wait(u64 ev, u64 out, u64 max, u64 timeout_ms, u64, u64, InterruptFrame*) {
    FdRef q(ev);
    if (!q) return -err::BADF;
    if (max == 0 || (i64)max < 0) return -err::INVAL;
    if (max > event::MAX_WATCHES) max = event::MAX_WATCHES;
    if (timeout_ms != event::FOREVER && timeout_ms > SLEEP_MAX_MS) return -err::INVAL;
    if (!clear_user(out, max * sizeof(event::Event)).ok()) return -err::FAULT;
    event::Event* ready = (event::Event*)kzalloc(max * sizeof(event::Event));
    if (!ready) return -err::NOMEM;
    Result<u32> r = event_wait(q, ready, (u32)max, timeout_ms);
    i64 result = r.ok() ? (i64)r.value() : errno_of(r.error());
    if (result > 0 && !copy_to_user(out, ready, (u64)result * sizeof(event::Event)).ok()) result = -err::FAULT;
    kfree(ready);
    return result;
}

i64 sys_reboot(u64 how, u64, u64, u64, u64, u64, InterruptFrame*) {
    if (self()->cred.uid != 0) return -err::PERM;
    if (how != 1 && how != 2) return -err::INVAL;
    if (how == 1 && !power_can_power_off()) return -err::NOSYS;
    vfs_sync();
    if (how == 2) power_reboot();
    power_off();
    return -err::IO;
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
    case Error::Access: return -err::ACCES;
    case Error::ReadOnly: return -err::ROFS;
    case Error::NotEmpty: return -err::NOTEMPTY;
    case Error::NameTooLong: return -err::NAMETOOLONG;
    case Error::Loop: return -err::LOOP;
    case Error::CrossDevice: return -err::XDEV;
    case Error::NoDevice: return -err::NODEV;
    case Error::Pipe: return -err::PIPE;
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

extern "C" u64 syscall_dispatch(InterruptFrame* f) {
    f->rax = (u64)dispatch(f->rax, f);
    // Signals and the end of the process are acted on here, on the way out
    // (a handler rewrites the frame, so this comes before the checks below).
    user_return(f);

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
    // sigreturn restores every register, which the fast return path cannot
    // do (it uses rcx and r11): nonzero asks the entry code to use iretq.
    Thread* t = thread_current();
    bool full = t->iret_return;
    t->iret_return = false;
    return full;
}
