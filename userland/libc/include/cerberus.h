// The Cerberus C library: one header for the whole (small) interface. This is
// the minimal in-tree library of SPEC phase 7: enough to write and test
// programs against the first system calls. Functions follow their usual C
// meanings; on failure the system-call wrappers return -1 and set errno.
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cerberus/syscall_nr.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- errno ----
// One per thread.
extern __thread int errno;
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
#define EPIPE 32
#define EDEADLK 35
#define EMSGSIZE 90
#define ETIMEDOUT 110

// "No time limit" for the calls that take a timeout in milliseconds.
#define WAIT_FOREVER (~0ull)

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
pid_t getppid(void);
// A process has one identity (no separate effective ids). setuid and setgid
// are for root, and one way: call setgid first.
int getuid(void);
int geteuid(void);
int getgid(void);
int getegid(void);
int setuid(int uid);
int setgid(int gid);
int umask(int mask);

// ---- signals ----
#define SIGILL 4
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGTERM 15
#define SIGCHLD 17
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)
// Sets what a signal does and returns the previous setting. A handler runs
// on the stack of whichever thread takes the signal; a system call that was
// waiting returns EINTR.
sighandler_t signal(int sig, sighandler_t handler);
int kill(pid_t pid, int sig);
int raise(int sig);

// ---- threads ----
// Thread-local variables (__thread) work, and errno is one.
struct __tcb;
typedef struct __tcb* pthread_t;
// These return 0 or an error number (they do not set errno).
int pthread_create(pthread_t* out, const void* attr, void* (*fn)(void*), void* arg);
int pthread_join(pthread_t t, void** result);
pthread_t pthread_self(void);
__attribute__((noreturn)) void pthread_exit(void* result);

// Locks that enter the kernel only to wait. Placed in shared memory they
// work between processes.
typedef struct {
    volatile uint32_t state;
} pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER {0}
int pthread_mutex_init(pthread_mutex_t* m, const void* attr);
int pthread_mutex_destroy(pthread_mutex_t* m);
int pthread_mutex_lock(pthread_mutex_t* m);
int pthread_mutex_trylock(pthread_mutex_t* m);
int pthread_mutex_unlock(pthread_mutex_t* m);
typedef struct {
    volatile uint32_t seq;
} pthread_cond_t;
#define PTHREAD_COND_INITIALIZER {0}
int pthread_cond_init(pthread_cond_t* c, const void* attr);
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m);
int pthread_cond_signal(pthread_cond_t* c);
int pthread_cond_broadcast(pthread_cond_t* c);

// If *addr still equals val, sleeps until futex_wake(addr) or the timeout.
int futex_wait(uint32_t* addr, uint32_t val, uint64_t timeout_ms);
// Wakes up to count waiters; returns how many.
int futex_wake(uint32_t* addr, int count);

// ---- ports: messages between processes ----
#define PORT_MESSAGE_MAX 65536
#define PORT_FDS_MAX 8
struct port_peer {
    uint32_t pid, uid, gid;
};
// A server creates a named port; mode says who may connect (write permission,
// as for a file).
int port_create(const char* name, int mode);
// A client connects and gets one end of a two-way channel.
int port_connect(const char* name);
// The server's end of the next connection made to a named port.
int port_accept(int port, uint64_t timeout_ms);
// One whole message, optionally with open descriptors (files, shared memory).
int port_send(int port, const void* msg, size_t len, const int* fds, int nfds);
// Returns the message's length. *nfds: room in fds on entry, count on return.
long port_recv(int port, void* buf, size_t len, int* fds, int* nfds, uint64_t timeout_ms);
// Who is at the other end, as the kernel recorded it.
int port_peer(int port, struct port_peer* out);

// ---- shared memory ----
int shm_create(size_t size);
void* shm_map(int handle, int prot);        // MAP_FAILED on failure
int shm_unmap(void* addr);

// ---- event queues: wait for any of several things ----
#define EVENT_READ 1
#define EVENT_WRITE 2
#define EVENT_HUP 4         // the other end closed; always reported
#define EVENT_TIMER 8
#define EVENT_CHILD 16
#define EVENT_ADD 1
#define EVENT_MOD 2
#define EVENT_DEL 3
#define EVENT_FD_TIMER (-2) // a one-shot timer: event.timeout_ms from now
#define EVENT_FD_CHILD (-3) // a child of this process has exited
struct event {
    uint32_t events;
    int32_t fd;
    uint64_t data;          // yours; comes back with the event
    uint64_t timeout_ms;
};
int event_create(int flags);
int event_ctl(int ev, int op, int fd, const struct event* e);
// Returns how many events were stored; 0 after the timeout.
int event_wait(int ev, struct event* out, int max, uint64_t timeout_ms);

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
int mprotect(void* addr, size_t len, int prot);
void* malloc(size_t n);
void* calloc(size_t count, size_t size);
void* realloc(void* p, size_t n);
void free(void* p);

// ---- machine ----
#define REBOOT_POWER_OFF 1
#define REBOOT_RESTART 2
// Writes everything to disk, then powers off or restarts. Root only;
// returns only on failure.
int reboot(int how);
uint64_t time_ms(void);
uint64_t time_us(void);
struct sysinfo {
    uint64_t uptime_ms;
    uint64_t mem_total, mem_free;
    uint64_t cache_pages, cache_hits, cache_misses;
    uint64_t ticks, idle_ticks;         // timer ticks over all CPUs, and those spent idle
    uint64_t context_switches;
    uint32_t cpus, threads, processes;
    uint32_t page_size;
};
int sysinfo(struct sysinfo* out);

// ---- input devices (/dev/input/kbd0, /dev/input/mouse0; root only) ----
#define EV_KEY 1        // code: key code; value: 1 press, 0 release, 2 repeat
#define EV_REL 2        // code: REL_X, REL_Y, REL_WHEEL; value: movement
#define EV_BUTTON 3     // code: 0 left, 1 right, 2 middle; value: 1 press, 0 release
#define REL_X 0
#define REL_Y 1
#define REL_WHEEL 2
struct input_event {
    uint64_t time_us;
    uint16_t type, code;
    int32_t value;
    uint32_t unicode;
    uint16_t mods;      // 1 shift, 2 ctrl, 4 alt, 8 super, 16 caps lock, 32 num lock
    uint16_t reserved;
};

// ---- the screen (/dev/fb0; root only) ----
#define FBIO_GET_INFO 0x4600
struct fb_info {
    uint32_t width, height, pitch, bpp;
};

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
