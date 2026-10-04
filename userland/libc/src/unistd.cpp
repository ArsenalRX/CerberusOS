// System-call wrappers: each turns the kernel's "negative means -errno"
// result into the C convention of returning -1 and setting errno.
#include <cerberus.h>

extern "C" {

long syscall6(long number, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long r10 asm("r10") = a3;
    register long r8 asm("r8") = a4;
    register long r9 asm("r9") = a5;
    long ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
    return ret;
}

static long check(long r) {
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

__attribute__((noreturn)) void exit(int status) {
    syscall6(SYS_exit, status, 0, 0, 0, 0, 0);
    for (;;) asm volatile("ud2");       // exit does not return; if it ever did, stop here
}

__attribute__((noreturn)) void abort(void) { exit(134); }

pid_t getpid(void) { return (pid_t)syscall6(SYS_getpid, 0, 0, 0, 0, 0, 0); }
pid_t fork(void) { return (pid_t)check(syscall6(SYS_fork, 0, 0, 0, 0, 0, 0)); }

int execve(const char* path, char* const argv[], char* const envp[]) {
    return (int)check(syscall6(SYS_execve, (long)path, (long)argv, (long)envp, 0, 0, 0));
}

pid_t waitpid(pid_t pid, int* status, int flags) {
    return (pid_t)check(syscall6(SYS_waitpid, pid, (long)status, flags, 0, 0, 0));
}

int sched_yield(void) { return (int)check(syscall6(SYS_yield, 0, 0, 0, 0, 0, 0)); }
int sleep_ms(uint64_t ms) { return (int)check(syscall6(SYS_sleep_ms, (long)ms, 0, 0, 0, 0, 0)); }

long read(int fd, void* buf, size_t n) { return check(syscall6(SYS_read, fd, (long)buf, (long)n, 0, 0, 0)); }
long write(int fd, const void* buf, size_t n) { return check(syscall6(SYS_write, fd, (long)buf, (long)n, 0, 0, 0)); }

int open(const char* path, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    int mode = va_arg(ap, int);
    va_end(ap);
    return (int)check(syscall6(SYS_open, (long)path, flags, mode, 0, 0, 0));
}

int close(int fd) { return (int)check(syscall6(SYS_close, fd, 0, 0, 0, 0, 0)); }

void* mmap(void* hint, size_t len, int prot, int flags, int fd, long off) {
    long r = syscall6(SYS_mmap, (long)hint, (long)len, prot, flags, fd, off);
    if (r < 0 && r > -4096) {           // a valid address is never in the last page of the address space
        errno = (int)-r;
        return MAP_FAILED;
    }
    return (void*)r;
}

int munmap(void* addr, size_t len) { return (int)check(syscall6(SYS_munmap, (long)addr, (long)len, 0, 0, 0, 0)); }

long getrandom(void* buf, size_t n, unsigned flags) {
    return check(syscall6(SYS_getrandom, (long)buf, (long)n, flags, 0, 0, 0));
}

} // extern "C"
