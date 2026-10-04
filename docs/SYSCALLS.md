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
| 5 | `long seek(int fd, long off, int whence)` | Moves the file offset (SEEK_SET 0, SEEK_CUR 1, SEEK_END 2); returns the new offset. Directories only rewind to 0. EBADF, EINVAL, ESPIPE (devices). |
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
| 20 | `void* mmap(void* hint, size_t len, int prot, int flags, int fd, long off)` | Maps zero-filled private memory. flags must be MAP_PRIVATE|MAP_ANONYMOUS, optionally MAP_FIXED (which never replaces an existing mapping); fd must be -1 and off 0. Writable and executable together is refused. EINVAL, ENOMEM, EEXIST. |
| 21 | `int munmap(void* addr, size_t len)` | Unmaps a page-aligned range. EINVAL. |
| 30 | `int fork(void)` | Creates a copy of the calling process. Returns the child's pid in the parent and 0 in the child. ENOMEM. |
| 31 | `int execve(const char* path, char* const argv[], char* const envp[])` | Replaces the program with one from the boot archive. Does not return on success. At most 64 arguments and 64 environment strings, 32 KiB in total. ENOENT, EISDIR, EPERM (not executable), ENOEXEC, E2BIG, EFAULT, ENOMEM. |
| 32 | `int waitpid(int pid, int* status, int flags)` | Waits for a child to exit (pid > 0: that child, -1: any) and returns its pid. flags may be WNOHANG (1): return 0 if none has exited. status may be null. ECHILD, EINVAL, EFAULT. |
| 33 | `int getpid(void)` | Returns the calling process's id. |
| 38 | `int yield(void)` | Gives up the rest of the time slice. Returns 0. |
| 39 | `int sleep_ms(uint64_t ms)` | Sleeps at least ms milliseconds (at most 2^31). Returns 0. EINVAL. |
| 60 | `uint64_t time_ms(void)` | Milliseconds since the system started (monotonic). |
| 66 | `long getrandom(void* buf, size_t n, unsigned flags)` | Fills buf with random bytes from the kernel generator; returns the number written (at most 1 MiB per call). flags must be 0. EINVAL, EFAULT. |
| 120 | `int openat(int dirfd, const char* path, int flags, int mode)` | As open, relative to the directory open as dirfd (AT_FDCWD -100: the working directory). EBADF, ENOTDIR, and those of open. |
| 121 | `int rmdir(const char* path)` | Removes an empty directory. ENOTEMPTY, ENOTDIR, EBUSY (a mount point), ENOENT, EACCES, EROFS, EFAULT. |
| 122 | `int symlink(const char* target, const char* path)` | Creates a symbolic link at path pointing to target (not checked). EEXIST, ENOENT, EACCES, EROFS, EFAULT. |
| 123 | `long readlink(const char* path, char* buf, size_t n)` | Copies a symbolic link's target (not NUL-terminated); returns its length. EINVAL if not a link, ENOENT, EFAULT. |
| 124 | `int chmod(const char* path, int mode)` | Sets permission bits; owner or root only. EPERM, EROFS, ENOENT, EFAULT. |
| 125 | `int fsync(int fd)` | Writes one file's changes to its disk and returns when they are there. EBADF, EIO. |
| 126 | `int mount(const char* source, const char* target, const char* type, unsigned flags)` | Mounts a file system of `type` (tmpfs, lumfs) from source (a device path, or anything for tmpfs) on the directory target. flags: MS_RDONLY 1, MS_NOEXEC 2, MS_NOSUID 4, MS_NODEV 8. Root only. EPERM, ENODEV (unknown type), EBUSY, ENOTDIR, EINVAL, EIO, EFAULT. |
| 127 | `int umount(const char* target)` | Unmounts the file system mounted on target after writing its changes. EBUSY if a file in it is open, EINVAL if target is not a mount point, EPERM. |
| 128 | `int lstat(const char* path, struct stat* out)` | As stat, but describes a symbolic link itself. ENOENT, EACCES, EFAULT. |
| 129 | `int chown(const char* path, int uid, int gid)` | Changes owner and group; root only. EPERM, EROFS, ENOENT, EFAULT. |
