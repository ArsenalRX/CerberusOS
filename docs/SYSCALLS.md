# System calls

Generated from `kernel/syscall/table.def` by `tools/gen-syscalls.py`. Do not edit by hand.

Convention: the `syscall` instruction; number in `rax`; arguments in `rdi`, `rsi`, `rdx`, `r10`,
`r8`, `r9`; result in `rax`, where a negative value is `-errno`. `rcx` and `r11` are destroyed;
every other register is preserved. Numbers never change. Only implemented calls are listed.

| No. | Prototype | Description |
|---|---|---|
| 0 | `void exit(int status)` | Ends the calling process. Only the low 8 bits of status reach the parent. Does not return. |
| 1 | `long write(int fd, const void* buf, size_t n)` | Writes up to n bytes; returns the number written, which may be less. EBADF, EFAULT. |
| 2 | `long read(int fd, void* buf, size_t n)` | Reads up to n bytes; returns the number read, 0 at end of file. The console has no input yet and returns 0. EBADF, EFAULT. |
| 3 | `int open(const char* path, int flags, int mode)` | Opens a file from the read-only boot archive. flags must be O_RDONLY (0). Returns a descriptor. ENOENT, EISDIR, EPERM (write access), EINVAL (unknown flags), EMFILE, EFAULT, E2BIG (path too long). |
| 4 | `int close(int fd)` | Closes a descriptor. EBADF. |
| 20 | `void* mmap(void* hint, size_t len, int prot, int flags, int fd, long off)` | Maps zero-filled private memory. flags must be MAP_PRIVATE|MAP_ANONYMOUS, optionally MAP_FIXED (which never replaces an existing mapping); fd must be -1 and off 0. Writable and executable together is refused. EINVAL, ENOMEM, EEXIST. |
| 21 | `int munmap(void* addr, size_t len)` | Unmaps a page-aligned range. EINVAL. |
| 30 | `int fork(void)` | Creates a copy of the calling process. Returns the child's pid in the parent and 0 in the child. ENOMEM. |
| 31 | `int execve(const char* path, char* const argv[], char* const envp[])` | Replaces the program with one from the boot archive. Does not return on success. At most 64 arguments and 64 environment strings, 32 KiB in total. ENOENT, EISDIR, EPERM (not executable), ENOEXEC, E2BIG, EFAULT, ENOMEM. |
| 32 | `int waitpid(int pid, int* status, int flags)` | Waits for a child to exit (pid > 0: that child, -1: any) and returns its pid. flags may be WNOHANG (1): return 0 if none has exited. status may be null. ECHILD, EINVAL, EFAULT. |
| 33 | `int getpid(void)` | Returns the calling process's id. |
| 38 | `int yield(void)` | Gives up the rest of the time slice. Returns 0. |
| 39 | `int sleep_ms(uint64_t ms)` | Sleeps at least ms milliseconds (at most 2^31). Returns 0. EINVAL. |
| 66 | `long getrandom(void* buf, size_t n, unsigned flags)` | Fills buf with random bytes from the kernel generator; returns the number written (at most 1 MiB per call). flags must be 0. EINVAL, EFAULT. |
