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
| 3 | `int open(const char* path, int flags, int mode)` | Opens a file; relative paths start at the working directory. flags: O_RDONLY/O_WRONLY/O_RDWR plus O_CREAT, O_EXCL, O_TRUNC, O_APPEND, O_NONBLOCK, O_DIRECTORY, O_NOFOLLOW, O_CLOEXEC. A new file gets mode & ~umask. Returns the lowest free descriptor. ENOENT, EACCES, EEXIST, EISDIR, ENOTDIR, ELOOP, EROFS, ENODEV, EINVAL, EMFILE, ENAMETOOLONG, EFAULT. |
| 4 | `int close(int fd)` | Closes a descriptor. EBADF. |
| 5 | `long seek(int fd, long off, int whence)` | Moves the file offset (SEEK_SET 0, SEEK_CUR 1, SEEK_END 2); returns the new offset. Directories only rewind to 0; stream devices ignore the offset. EBADF, EINVAL. |
| 6 | `int stat(const char* path, struct stat* out)` | Describes a file, following symbolic links. ENOENT, EACCES, ENOTDIR, ELOOP, ENAMETOOLONG, EFAULT. |
| 7 | `int fstat(int fd, struct stat* out)` | Describes an open file. EBADF, EFAULT. |
| 8 | `int mkdir(const char* path, int mode)` | Creates a directory with mode & ~umask. EEXIST, ENOENT, EACCES, EROFS, ENOSPC, ENAMETOOLONG, EFAULT. |
| 9 | `int unlink(const char* path)` | Removes a name that is not a directory; the file goes when the last descriptor closes. ENOENT, EISDIR, EACCES, EBUSY, EROFS, EFAULT. |
| 10 | `int rename(const char* from, const char* to)` | Moves a name, replacing `to` if it is of the same kind (an empty directory, or a non-directory). EXDEV across file systems; EINVAL moving a directory into itself; ENOENT, EACCES, ENOTEMPTY, EISDIR, ENOTDIR, EBUSY, EROFS, EFAULT. |
| 11 | `long readdir(int fd, struct dirent* out, size_t n)` | Reads up to n directory entries (\".\" and \"..\" first); returns how many, 0 at the end. EBADF, ENOTDIR, EFAULT. |
| 12 | `int chdir(const char* path)` | Changes the working directory. ENOENT, ENOTDIR, EACCES, EFAULT. |
| 13 | `long getcwd(char* buf, size_t n)` | Stores the working directory's absolute path; returns its length. ENAMETOOLONG if n is too small, EFAULT. |
| 14 | `int dup(int fd)` | Duplicates a descriptor onto the lowest free one (close-on-exec cleared). EBADF, EMFILE. |
| 15 | `int dup2(int oldfd, int newfd)` | Makes newfd refer to oldfd's file, closing newfd first. EBADF. |
| 17 | `long ioctl(int fd, unsigned request, void* arg)` | Device control. Block devices: BLKGETSIZE64 (0x80081272) stores the size in bytes. EBADF, ENODEV, EFAULT. |
| 18 | `int truncate(int fd, long size)` | Sets the length of a file open for writing. EBADF, EINVAL, EISDIR, EROFS, ENOSPC. |
| 19 | `int sync(void)` | Writes every file system's changes to its disk. Returns 0. |
| 20 | `void* mmap(void* hint, size_t len, int prot, int flags, int fd, long off)` | Maps private memory. With MAP_PRIVATE|MAP_ANONYMOUS (fd -1, off 0) it is zero-filled; with MAP_PRIVATE and a file descriptor it is the caller's own copy of the file from the page-aligned offset off, read in when mapped, zero past the end of the file (PROT_EXEC needs execute permission on the file). MAP_FIXED never replaces an existing mapping. Writable and executable together is refused. Memory shared between processes comes from shm_create. EINVAL, EBADF, EACCES, ENODEV (not a regular file), ENOMEM, EEXIST, EIO. |
| 21 | `int munmap(void* addr, size_t len)` | Unmaps a page-aligned range. EINVAL. |
| 22 | `int mprotect(void* addr, size_t len, int prot)` | Changes the protection of a page-aligned range that is entirely mapped. Writable and executable together is refused. EINVAL. |
| 30 | `int fork(void)` | Creates a copy of the calling process. Returns the child's pid in the parent and 0 in the child. ENOMEM. |
| 31 | `int execve(const char* path, char* const argv[], char* const envp[])` | Replaces the program with one from the boot archive. Does not return on success. At most 64 arguments and 64 environment strings, 32 KiB in total. ENOENT, EISDIR, EPERM (not executable), ENOEXEC, E2BIG, EFAULT, ENOMEM. |
| 32 | `int waitpid(int pid, int* status, int flags)` | Waits for a child to exit (pid > 0: that child, -1: any) and returns its pid. flags may be WNOHANG (1): return 0 if none has exited. status may be null. ECHILD, EINVAL, EFAULT. |
| 33 | `int getpid(void)` | Returns the calling process's id. |
| 34 | `int getppid(void)` | Returns the parent's process id (0 for a process started by the kernel). |
| 35 | `int kill(int pid, int sig)` | Sends a signal to a process of the same user (root: any process). sig 0 only checks that it could be sent. ESRCH, EPERM, EINVAL. |
| 36 | `void* signal(int sig, void* handler)` | Sets what a signal does: SIG_DFL (0), SIG_IGN (1) or a function void handler(int). Returns the previous setting. SIGKILL cannot be changed. Handlers run on the thread's own stack and return through sigreturn. EINVAL. |
| 37 | `void sigreturn(void)` | Restores the state a signal handler interrupted. Called by the kernel-provided trampoline a handler returns into, not by programs. |
| 38 | `int yield(void)` | Gives up the rest of the time slice. Returns 0. |
| 39 | `int sleep_ms(uint64_t ms)` | Sleeps at least ms milliseconds (at most 2^31). Returns 0. EINVAL, EINTR (a signal arrived). |
| 40 | `int thread_spawn(void* entry, void* arg, void* stack, void* tls)` | Starts a thread in the calling process at entry(arg) with the given stack pointer and FS base. Returns its id. The C library's pthread_create wraps this. EINVAL, EAGAIN (256 threads), ENOMEM. |
| 41 | `void thread_exit(int code)` | Ends the calling thread; the process ends with it if it was the last. Does not return. |
| 42 | `int thread_join(int tid, int* code)` | Waits for a thread of the calling process to end and stores its exit code. Each thread can be joined once. ESRCH, EINVAL, EINTR, EFAULT. |
| 43 | `int futex_wait(uint32_t* addr, uint32_t val, uint64_t timeout_ms)` | If *addr still equals val, sleeps until futex_wake(addr) or the timeout (~0 = none). A word in shared memory is the same futex in every process mapping it. EAGAIN (the value differs), ETIMEDOUT, EINTR, EINVAL (unaligned), EFAULT. |
| 44 | `int futex_wake(uint32_t* addr, int count)` | Wakes up to count threads waiting on addr; returns how many. EINVAL, EFAULT. |
| 50 | `int port_create(const char* name, int mode)` | Creates a named port (letters, digits, '.', '-', '_'; up to 63) and returns a descriptor to accept connections on. mode is permission bits as for a file: connecting needs write permission. EEXIST, EINVAL, EMFILE, ENOMEM. |
| 51 | `int port_connect(const char* name)` | Connects to a named port; returns one end of a new two-way channel. ENOENT, EACCES, EAGAIN (the server has 32 connections waiting), EMFILE, ENOMEM. |
| 52 | `int port_send(int port, const void* msg, size_t len, const int* fds, int nfds)` | Sends one message of up to 64 KiB with up to 8 descriptors (files, devices, shared memory) over a channel. Waits while 64 messages or 1 MiB are queued. EMSGSIZE, EPIPE (the other end is closed), EBADF, EINVAL, EINTR, EFAULT. |
| 53 | `long port_recv(int port, void* buf, size_t len, int* fds, int* nfds, uint64_t timeout_ms)` | Receives the next message; returns its length. *nfds is the room in fds on entry and the number of descriptors received on return. On a named port it accepts a connection: length 0 and one descriptor, the server's end of the channel. timeout 0 = do not wait, ~0 = no limit. EAGAIN, ETIMEDOUT, EPIPE, ENOSPC (the message or its descriptors do not fit; it stays queued), EMFILE, EINTR, EFAULT. |
| 55 | `int shm_create(size_t size)` | Creates a block of zero-filled shared memory (at most 256 MiB) and returns a descriptor, which can be sent over a port. EINVAL, ENOMEM, EMFILE. |
| 56 | `void* shm_map(int handle, int prot)` | Maps a shared-memory block; prot is PROT_READ, optionally with PROT_WRITE. Returns the address. EBADF, EINVAL, ENOMEM. |
| 57 | `int shm_unmap(void* addr)` | Removes a mapping made by shm_map. EINVAL. |
| 58 | `int port_peer(int port, struct port_peer* out)` | Stores the pid, uid and gid of the process at the other end of a channel, as recorded by the kernel when the channel was made. EBADF, EINVAL, EFAULT. |
| 60 | `uint64_t time_ms(void)` | Milliseconds since the system started (monotonic). |
| 63 | `int sysinfo(struct sysinfo* out)` | Stores memory, file-cache and scheduler figures. EFAULT. |
| 64 | `int reboot(int how)` | Writes every file system to disk, then powers the machine off (how = 1) or restarts it (how = 2). Root only. Returns only on failure: EPERM, EINVAL, ENOSYS (power off not available). |
| 66 | `long getrandom(void* buf, size_t n, unsigned flags)` | Fills buf with random bytes from the kernel generator; returns the number written (at most 1 MiB per call). flags must be 0. EINVAL, EFAULT. |
| 67 | `uint64_t time_us(void)` | Microseconds since the system started (monotonic). |
| 90 | `int event_create(int flags)` | Creates an event queue. flags: 0 or O_CLOEXEC. EINVAL, EMFILE, ENOMEM. |
| 91 | `int event_ctl(int ev, int op, int fd, const struct event* e)` | Adds (1), changes (2) or removes (3) a watch: on a descriptor (e->events: EVENT_READ, EVENT_WRITE), a one-shot timer (fd EVENT_FD_TIMER, e->timeout_ms) or the exit of a child (fd EVENT_FD_CHILD). e->data comes back with the event. EBADF, EEXIST, EINVAL, ENOSPC (64 watches), EFAULT. |
| 92 | `int event_wait(int ev, struct event* out, int max, uint64_t timeout_ms)` | Sleeps until something watched is ready and stores up to max events; returns how many, 0 after the timeout (~0 = none). Level-triggered. EBADF, EINVAL, EINTR, EFAULT. |
| 100 | `int getuid(void)` | Returns the user id of the calling process. |
| 101 | `int geteuid(void)` | The same as getuid: a process has one identity. |
| 102 | `int getgid(void)` | Returns the group id of the calling process. |
| 103 | `int getegid(void)` | The same as getgid. |
| 104 | `int setuid(int uid)` | Changes the user id. Root only, and one way: once root is given up it cannot be taken back. EPERM, EINVAL. |
| 105 | `int setgid(int gid)` | Changes the group id. Root only (call it before setuid). EPERM, EINVAL. |
| 111 | `int umask(int mask)` | Sets the permission bits removed from new files; returns the previous mask. |
| 120 | `int openat(int dirfd, const char* path, int flags, int mode)` | As open, relative to the directory open as dirfd (AT_FDCWD -100: the working directory). EBADF, ENOTDIR, and those of open. |
| 121 | `int rmdir(const char* path)` | Removes an empty directory. ENOTEMPTY, ENOTDIR, EBUSY (a mount point), ENOENT, EACCES, EROFS, EFAULT. |
| 122 | `int symlink(const char* target, const char* path)` | Creates a symbolic link at path pointing to target (not checked). EEXIST, ENOENT, EACCES, EROFS, EFAULT. |
| 123 | `long readlink(const char* path, char* buf, size_t n)` | Copies a symbolic link's target (not NUL-terminated); returns its length. EINVAL if not a link, ENOENT, EFAULT. |
| 124 | `int chmod(const char* path, int mode)` | Sets permission bits; owner or root only. EPERM, EROFS, ENOENT, EFAULT. |
| 125 | `int fsync(int fd)` | Writes one file's changes to its disk and returns when they are there. EBADF, EIO. |
| 126 | `int mount(const char* source, const char* target, const char* type, unsigned flags)` | Mounts a file system of `type` (tmpfs, cerfs) from source (a device path, or anything for tmpfs) on the directory target. flags: MS_RDONLY 1, MS_NOEXEC 2, MS_NOSUID 4, MS_NODEV 8. Root only. EPERM, ENODEV (unknown type), EBUSY, ENOTDIR, EINVAL, EIO, EFAULT. |
| 127 | `int umount(const char* target)` | Unmounts the file system mounted on target after writing its changes. EBUSY if a file in it is open, EINVAL if target is not a mount point, EPERM. |
| 128 | `int lstat(const char* path, struct stat* out)` | As stat, but describes a symbolic link itself. ENOENT, EACCES, EFAULT. |
| 129 | `int chown(const char* path, int uid, int gid)` | Changes owner and group; root only. EPERM, EROFS, ENOENT, EFAULT. |
| 130 | `int set_tls(void* base)` | Sets the calling thread's FS base (the thread pointer). EINVAL. |
