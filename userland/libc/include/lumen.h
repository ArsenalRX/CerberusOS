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
#define O_RDONLY 0
long read(int fd, void* buf, size_t n);
long write(int fd, const void* buf, size_t n);
int open(const char* path, int flags, ...);
int close(int fd);

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
// Formats: %d %i %u %x %X %c %s %p %%, with l/ll/z length modifiers, a width,
// '-' and '0' flags. No floating point.
int printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int snprintf(char* buf, size_t n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);
int puts(const char* s);

#ifdef __cplusplus
}
#endif
