// File and directory wrappers (phase 9), plus strerror and dprintf.
#include <cerberus.h>

extern "C" {

static long check(long r) {
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

#define CALL(nr, a, b, c, d) check(syscall6(nr, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0))

int openat(int dirfd, const char* path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = va_arg(ap, int);
    va_end(ap);
    return (int)CALL(SYS_openat, dirfd, path, flags, mode);
}

long lseek(int fd, long off, int whence) { return CALL(SYS_seek, fd, off, whence, 0); }
int stat(const char* path, struct stat* out) { return (int)CALL(SYS_stat, path, out, 0, 0); }
int lstat(const char* path, struct stat* out) { return (int)CALL(SYS_lstat, path, out, 0, 0); }
int fstat(int fd, struct stat* out) { return (int)CALL(SYS_fstat, fd, out, 0, 0); }
int mkdir(const char* path, int mode) { return (int)CALL(SYS_mkdir, path, mode, 0, 0); }
int rmdir(const char* path) { return (int)CALL(SYS_rmdir, path, 0, 0, 0); }
int unlink(const char* path) { return (int)CALL(SYS_unlink, path, 0, 0, 0); }
int rename(const char* from, const char* to) { return (int)CALL(SYS_rename, from, to, 0, 0); }
int symlink(const char* target, const char* path) { return (int)CALL(SYS_symlink, target, path, 0, 0); }
int link(const char* oldpath, const char* newpath) { return (int)CALL(SYS_link, oldpath, newpath, 0, 0); }
long readlink(const char* path, char* buf, size_t n) { return CALL(SYS_readlink, path, buf, n, 0); }
int chmod(const char* path, int mode) { return (int)CALL(SYS_chmod, path, mode, 0, 0); }
int chown(const char* path, int uid, int gid) { return (int)CALL(SYS_chown, path, uid, gid, 0); }
int chdir(const char* path) { return (int)CALL(SYS_chdir, path, 0, 0, 0); }
int dup(int fd) { return (int)CALL(SYS_dup, fd, 0, 0, 0); }
int dup2(int oldfd, int newfd) { return (int)CALL(SYS_dup2, oldfd, newfd, 0, 0); }
int ioctl(int fd, unsigned long request, void* arg) { return (int)CALL(SYS_ioctl, fd, request, arg, 0); }
int ftruncate(int fd, long size) { return (int)CALL(SYS_truncate, fd, size, 0, 0); }
int fsync(int fd) { return (int)CALL(SYS_fsync, fd, 0, 0, 0); }
void sync(void) { syscall6(SYS_sync, 0, 0, 0, 0, 0, 0); }
int mount(const char* source, const char* target, const char* type, unsigned flags) {
    return (int)CALL(SYS_mount, source, target, type, flags);
}
int umount(const char* target) { return (int)CALL(SYS_umount, target, 0, 0, 0); }
long getdents(int fd, struct dirent* out, size_t n) { return CALL(SYS_readdir, fd, out, n, 0); }

int reboot(int how) { return (int)CALL(SYS_reboot, how, 0, 0, 0); }
uint64_t time_ms(void) { return (uint64_t)syscall6(SYS_time_ms, 0, 0, 0, 0, 0, 0); }

char* getcwd(char* buf, size_t n) {
    return check(syscall6(SYS_getcwd, (long)buf, (long)n, 0, 0, 0, 0)) < 0 ? nullptr : buf;
}

DIR* opendir(const char* path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    DIR* d = (DIR*)malloc(sizeof(DIR));
    if (!d) {
        close(fd);
        errno = ENOMEM;
        return nullptr;
    }
    d->fd = fd;
    d->count = d->pos = 0;
    return d;
}

struct dirent* readdir(DIR* d) {
    if (d->pos == d->count) {
        long n = getdents(d->fd, d->buf, sizeof d->buf / sizeof d->buf[0]);
        if (n <= 0) return nullptr;
        d->count = (int)n;
        d->pos = 0;
    }
    return &d->buf[d->pos++];
}

int closedir(DIR* d) {
    int r = close(d->fd);
    free(d);
    return r;
}

const char* strerror(int err) {
    switch (err) {
    case 0: return "no error";
    case EPERM: return "operation not permitted";
    case ENOENT: return "no such file or directory";
    case ESRCH: return "no such process";
    case EINTR: return "interrupted";
    case EIO: return "input/output error";
    case E2BIG: return "argument list too long";
    case ENOEXEC: return "not an executable";
    case EBADF: return "bad file descriptor";
    case ECHILD: return "no child processes";
    case EAGAIN: return "try again";
    case ENOMEM: return "out of memory";
    case EACCES: return "permission denied";
    case EFAULT: return "bad address";
    case EBUSY: return "device or resource busy";
    case EEXIST: return "file exists";
    case EXDEV: return "cross-device link";
    case ENODEV: return "no such device";
    case ENOTDIR: return "not a directory";
    case EISDIR: return "is a directory";
    case EINVAL: return "invalid argument";
    case EMFILE: return "too many open files";
    case ENOSPC: return "no space left on device";
    case ESPIPE: return "illegal seek";
    case EROFS: return "read-only file system";
    case ENAMETOOLONG: return "file name too long";
    case ENOSYS: return "function not implemented";
    case ENOTEMPTY: return "directory not empty";
    case ELOOP: return "too many symbolic links";
    case EPIPE: return "the other end is closed";
    case EDEADLK: return "deadlock avoided";
    case EMSGSIZE: return "message too long";
    case ETIMEDOUT: return "timed out";
    }
    return "unknown error";
}

int dprintf(int fd, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) write(fd, buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
    return n;
}

} // extern "C"
