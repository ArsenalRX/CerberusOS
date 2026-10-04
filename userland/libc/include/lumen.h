// The Lumen C library: one header for the whole (small) interface. This is
// the minimal in-tree library of SPEC phase 7: enough to write and test
// programs against the first system calls. Functions follow their usual C
// meanings; on failure the system-call wrappers return -1 and set errno.
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <lumen/syscall_nr.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- errno ----
extern int errno;
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOSPC 28
#define ENOSYS 38
#define EACCES 13
#define EXDEV 18
#define ENODEV 19
#define ESPIPE 29
#define EROFS 30
#define ENAMETOOLONG 36
#define ENOTEMPTY 39
#define ELOOP 40

// ---- raw system call: returns the kernel's result, negative values being -errno ----
long syscall6(long number, long a0, long a1, long a2, long a3, long a4, long a5);

// ---- processes ----
typedef int pid_t;
__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void abort(void);
pid_t getpid(void);
pid_t fork(void);
int execve(const char* path, char* const argv[], char* const envp[]);
pid_t waitpid(pid_t pid, int* status, int flags);
int sched_yield(void);
int sleep_ms(uint64_t ms);
#define WNOHANG 1
#define WIFEXITED(s) (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WIFSIGNALED(s) (((s) & 0x7F) != 0)
#define WTERMSIG(s) ((s) & 0x7F)
extern char** environ;

// ---- files ----
// Values match the kernel (kernel/syscall/abi.h).
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3
#define O_CREAT 0x40
#define O_EXCL 0x80
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW 0x20000
#define O_CLOEXEC 0x80000
#define AT_FDCWD (-100)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define DT_UNKNOWN 0
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define MS_RDONLY 1
#define MS_NOEXEC 2
#define MS_NOSUID 4
#define MS_NODEV 8
#define BLKGETSIZE64 0x80081272u

struct stat {
    uint64_t st_ino;
    uint64_t st_size;
    uint64_t st_blocks;
    uint64_t st_atime, st_mtime, st_ctime;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid, st_gid;
    uint32_t st_rdev;
    uint32_t st_blksize;
};

struct dirent {
    uint64_t d_ino;
    uint8_t d_type;
    uint8_t d_reserved[7];
    char d_name[256];
};

typedef struct {
    int fd;
    int count, pos;
    struct dirent buf[8];
} DIR;

long read(int fd, void* buf, size_t n);
long write(int fd, const void* buf, size_t n);
int open(const char* path, int flags, ...);
int openat(int dirfd, const char* path, int flags, ...);
int close(int fd);
long lseek(int fd, long off, int whence);
int stat(const char* path, struct stat* out);
int lstat(const char* path, struct stat* out);
int fstat(int fd, struct stat* out);
int mkdir(const char* path, int mode);
int rmdir(const char* path);
int unlink(const char* path);
int rename(const char* from, const char* to);
int symlink(const char* target, const char* path);
long readlink(const char* path, char* buf, size_t n);
int chmod(const char* path, int mode);
int chown(const char* path, int uid, int gid);
int chdir(const char* path);
char* getcwd(char* buf, size_t n);
int dup(int fd);
int dup2(int oldfd, int newfd);
int ioctl(int fd, unsigned long request, void* arg);
int ftruncate(int fd, long size);
int fsync(int fd);
void sync(void);
int mount(const char* source, const char* target, const char* type, unsigned flags);
int umount(const char* target);
// Raw directory reading: up to n entries, 0 at the end.
long getdents(int fd, struct dirent* out, size_t n);
DIR* opendir(const char* path);
struct dirent* readdir(DIR* d);
int closedir(DIR* d);

// ---- memory ----
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void*)-1)
void* mmap(void* hint, size_t len, int prot, int flags, int fd, long off);
int munmap(void* addr, size_t len);
void* malloc(size_t n);
void* calloc(size_t count, size_t size);
void* realloc(void* p, size_t n);
void free(void* p);

// ---- randomness ----
long getrandom(void* buf, size_t n, unsigned flags);

// ---- strings ----
void* memcpy(void* dst, const void* src, size_t n);
void* memmove(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strchr(const char* s, int c);
size_t strlcpy(char* dst, const char* src, size_t n);
unsigned long strtoul(const char* s, char** end, int base);

// ---- output ----
// Formats: %d %i %u %o %x %X %c %s %p %%, with l/ll/z length modifiers, a width,
// '-' and '0' flags. No floating point.
int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int snprintf(char* buf, size_t n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);
int puts(const char* s);
int dprintf(int fd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
const char* strerror(int err);

#ifdef __cplusplus
}
#endif
