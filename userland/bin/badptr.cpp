// Hostile arguments to every system call: kernel addresses, unmapped
// addresses, ranges that wrap or run off the end of user space, unknown flag
// bits, bad descriptors, unknown call numbers. Each call must fail with the
// expected error and the kernel must still be standing afterwards.
#include <cerberus.h>

static int g_checks = 0, g_wrong = 0;

static void expect(const char* what, long got, long want) {
    g_checks++;
    if (got == want) return;
    g_wrong++;
    printf("badptr: WRONG: %s returned %ld, expected %ld\n", what, got, want);
}

#define CALL(nr, a, b, c, d, e, f) syscall6(nr, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

int main(int, char**, char**) {
    const unsigned long KERNEL = 0xFFFFFFFF80000000ul;      // kernel image
    const unsigned long DIRECT = 0xFFFF800000100000ul;      // kernel's direct map of RAM
    const unsigned long UNMAPPED = 0x0000000012345000ul;    // user half, nothing there
    const unsigned long NULLPAGE = 0x10;                    // inside the never-mapped first 64 KiB
    const unsigned long EDGE = 0x00007FFFFFFFFFF8ul;        // 8 bytes below the end of user space
    const unsigned long NONCANON = 0x0000800000000000ul;    // first non-canonical address
    const unsigned long bad[] = {KERNEL, DIRECT, UNMAPPED, NULLPAGE, NONCANON};
    char buf[64];
    memset(buf, 'x', sizeof buf);

    int fd = open("/bin/hello", O_RDONLY);
    expect("open of a real file", fd >= 0, 1);

    for (unsigned long p : bad) {
        expect("write(bad buffer)", CALL(SYS_write, 1, p, 16, 0, 0, 0), -EFAULT);
        expect("read(bad buffer)", CALL(SYS_read, fd, p, 16, 0, 0, 0), -EFAULT);
        expect("open(bad path)", CALL(SYS_open, p, 0, 0, 0, 0, 0), -EFAULT);
        expect("execve(bad path)", CALL(SYS_execve, p, 0, 0, 0, 0, 0), -EFAULT);
        expect("execve(bad argv)", CALL(SYS_execve, "/bin/hello", p, 0, 0, 0, 0), -EFAULT);
        expect("execve(bad envp)", CALL(SYS_execve, "/bin/hello", 0, p, 0, 0, 0), -EFAULT);
        expect("waitpid(bad status)", CALL(SYS_waitpid, -1, p, 0, 0, 0, 0), -EFAULT);
        expect("getrandom(bad buffer)", CALL(SYS_getrandom, p, 16, 0, 0, 0, 0), -EFAULT);
        // File-system calls (phase 9).
        expect("openat(bad path)", CALL(SYS_openat, AT_FDCWD, p, 0, 0, 0, 0), -EFAULT);
        expect("stat(bad path)", CALL(SYS_stat, p, buf, 0, 0, 0, 0), -EFAULT);
        expect("stat(bad buffer)", CALL(SYS_stat, "/bin/hello", p, 0, 0, 0, 0), -EFAULT);
        expect("lstat(bad buffer)", CALL(SYS_lstat, "/bin/hello", p, 0, 0, 0, 0), -EFAULT);
        expect("fstat(bad buffer)", CALL(SYS_fstat, fd, p, 0, 0, 0, 0), -EFAULT);
        expect("mkdir(bad path)", CALL(SYS_mkdir, p, 0755, 0, 0, 0, 0), -EFAULT);
        expect("unlink(bad path)", CALL(SYS_unlink, p, 0, 0, 0, 0, 0), -EFAULT);
        expect("rmdir(bad path)", CALL(SYS_rmdir, p, 0, 0, 0, 0, 0), -EFAULT);
        expect("rename(bad from)", CALL(SYS_rename, p, "/tmp/x", 0, 0, 0, 0), -EFAULT);
        expect("rename(bad to)", CALL(SYS_rename, "/tmp/x", p, 0, 0, 0, 0), -EFAULT);
        expect("chdir(bad path)", CALL(SYS_chdir, p, 0, 0, 0, 0, 0), -EFAULT);
        expect("getcwd(bad buffer)", CALL(SYS_getcwd, p, 64, 0, 0, 0, 0), -EFAULT);
        expect("readlink(bad path)", CALL(SYS_readlink, p, buf, 16, 0, 0, 0), -EFAULT);
        expect("symlink(bad target)", CALL(SYS_symlink, p, "/tmp/l", 0, 0, 0, 0), -EFAULT);
        expect("symlink(bad path)", CALL(SYS_symlink, "/x", p, 0, 0, 0, 0), -EFAULT);
        expect("link(bad old path)", CALL(SYS_link, p, "/tmp/l", 0, 0, 0, 0), -EFAULT);
        expect("link(bad new path)", CALL(SYS_link, "/bin/hello", p, 0, 0, 0, 0), -EFAULT);
        expect("chmod(bad path)", CALL(SYS_chmod, p, 0644, 0, 0, 0, 0), -EFAULT);
        expect("chown(bad path)", CALL(SYS_chown, p, 0, 0, 0, 0, 0), -EFAULT);
        expect("mount(bad source)", CALL(SYS_mount, p, "/mnt", "tmpfs", 0, 0, 0), -EFAULT);
        expect("mount(bad type)", CALL(SYS_mount, "none", "/mnt", p, 0, 0, 0), -EFAULT);
        expect("umount(bad path)", CALL(SYS_umount, p, 0, 0, 0, 0, 0), -EFAULT);
        expect("sigprocmask(bad set)", CALL(SYS_sigprocmask, 0, p, 0, 0, 0, 0), -EFAULT);
        expect("sigprocmask(bad old set)", CALL(SYS_sigprocmask, 0, 0, p, 0, 0, 0), -EFAULT);
        expect("port_try_send(bad message)", CALL(SYS_port_try_send, 0, p, 16, 0, 0, 0), -EFAULT);
        int dfd = open("/bin", O_RDONLY | O_DIRECTORY);
        expect("readdir(bad buffer)", CALL(SYS_readdir, dfd, p, 1, 0, 0, 0), -EFAULT);
        close(dfd);
    }
    // Descriptors and directories used the wrong way.
    expect("readdir(not a directory)", CALL(SYS_readdir, fd, buf, 1, 0, 0, 0), -ENOTDIR);
    expect("seek(bad whence)", CALL(SYS_seek, fd, 0, 9, 0, 0, 0), -EINVAL);
    expect("seek(before the start)", CALL(SYS_seek, fd, -5, SEEK_SET, 0, 0, 0), -EINVAL);
    expect("dup2(bad target)", CALL(SYS_dup2, fd, 999, 0, 0, 0, 0), -EBADF);
    expect("truncate(read-only descriptor)", CALL(SYS_truncate, fd, 0, 0, 0, 0, 0), -EBADF);
    expect("openat(bad directory descriptor)", CALL(SYS_openat, 29, "x", 0, 0, 0, 0), -EBADF);
    expect("mkdir(in the read-only root)", CALL(SYS_mkdir, "/newdir", 0755, 0, 0, 0, 0), -EROFS);
    expect("link(a directory)", CALL(SYS_link, "/tmp", "/tmp/dirlink", 0, 0, 0, 0), -EPERM);
    expect("link(across file systems)", CALL(SYS_link, "/bin/hello", "/tmp/hello", 0, 0, 0, 0), -EXDEV);
    char longpath[300];
    memset(longpath, 'a', sizeof longpath - 1);
    longpath[0] = '/';
    longpath[sizeof longpath - 1] = 0;
    expect("open(path too long)", CALL(SYS_open, longpath, 0, 0, 0, 0, 0), -ENAMETOOLONG);
    expect("reboot(unknown request)", CALL(SYS_reboot, 99, 0, 0, 0, 0, 0), -EINVAL);
    expect("sigprocmask(unknown how)", CALL(SYS_sigprocmask, 7, buf, 0, 0, 0, 0), -EINVAL);
    expect("mount(unknown type)", CALL(SYS_mount, "none", "/mnt", "nosuchfs", 0, 0, 0), -ENODEV);
    // A string pointer array whose entries point at bad addresses.
    unsigned long argv_bad[] = {(unsigned long)"ok", KERNEL, 0};
    expect("execve(argv entry in the kernel)", CALL(SYS_execve, "/bin/hello", argv_bad, 0, 0, 0, 0), -EFAULT);

    // Ranges that start in valid memory or near the top and run off the end, or wrap.
    expect("write(range past the end of user space)", CALL(SYS_write, 1, EDGE, 64, 0, 0, 0), -EFAULT);
    expect("read(range that wraps around zero)", CALL(SYS_read, fd, ~0ul - 8, 64, 0, 0, 0), -EFAULT);
    expect("read(range past the end of user space)", CALL(SYS_read, fd, EDGE, 64, 0, 0, 0), -EFAULT);
    // An enormous length over one real page: the kernel fills what exists,
    // stops at the first unmapped byte, and reports how much it did.
    void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    expect("mmap of one page", page != MAP_FAILED, 1);
    expect("getrandom(huge length over one page)", CALL(SYS_getrandom, page, ~0ul, 0, 0, 0, 0), 4096);
    expect("getrandom(range past the end)", CALL(SYS_getrandom, EDGE, 64, 0, 0, 0, 0), -EFAULT);
    expect("read(huge length into a small real buffer)", CALL(SYS_read, 0, buf, ~0ul, 0, 0, 0), 0);

    // Unknown flag bits and invalid values.
    expect("open(unknown flags)", CALL(SYS_open, "/bin/hello", 0x1234, 0, 0, 0, 0), -EINVAL);
    expect("open(for writing, read-only root)", CALL(SYS_open, "/bin/hello", 1, 0, 0, 0, 0), -EROFS);
    expect("open(missing file)", CALL(SYS_open, "/bin/none", 0, 0, 0, 0, 0), -ENOENT);
    expect("close(bad descriptor)", CALL(SYS_close, 999, 0, 0, 0, 0, 0), -EBADF);
    expect("close(negative descriptor)", CALL(SYS_close, -1, 0, 0, 0, 0, 0), -EBADF);
    expect("write(bad descriptor)", CALL(SYS_write, 31, buf, 4, 0, 0, 0), -EBADF);
    expect("mmap(zero length)", CALL(SYS_mmap, 0, 0, 3, 0x22, -1, 0), -EINVAL);
    expect("mmap(writable and executable)", CALL(SYS_mmap, 0, 4096, 7, 0x22, -1, 0), -EINVAL);
    expect("mmap(unknown flags)", CALL(SYS_mmap, 0, 4096, 3, 0x8022, -1, 0), -EINVAL);
    expect("mmap(unknown protection bits)", CALL(SYS_mmap, 0, 4096, 0x13, 0x22, -1, 0), -EINVAL);
    expect("mmap(with a file descriptor)", CALL(SYS_mmap, 0, 4096, 3, 0x22, fd, 0), -EINVAL);
    expect("mmap(absurd length)", CALL(SYS_mmap, 0, ~0ul, 3, 0x22, -1, 0), -ENOMEM);
    expect("mmap(fixed, in the kernel)", CALL(SYS_mmap, KERNEL, 4096, 3, 0x32, -1, 0), -EINVAL);
    expect("mmap(fixed, in the null guard)", CALL(SYS_mmap, 0x1000, 4096, 3, 0x32, -1, 0), -EINVAL);
    expect("munmap(unaligned)", CALL(SYS_munmap, 0x12345001, 4096, 0, 0, 0, 0), -EINVAL);
    expect("munmap(kernel address)", CALL(SYS_munmap, KERNEL, 4096, 0, 0, 0, 0), -EINVAL);
    expect("munmap(zero length)", CALL(SYS_munmap, 0x12345000, 0, 0, 0, 0, 0), -EINVAL);
    expect("waitpid(unknown flags)", CALL(SYS_waitpid, -1, 0, 0x80, 0, 0, 0), -EINVAL);
    expect("waitpid(pid 0)", CALL(SYS_waitpid, 0, 0, 0, 0, 0, 0), -EINVAL);
    expect("waitpid(no children)", CALL(SYS_waitpid, -1, 0, 0, 0, 0, 0), -ECHILD);
    expect("getrandom(unknown flags)", CALL(SYS_getrandom, buf, 16, 7, 0, 0, 0), -EINVAL);
    expect("sleep_ms(absurd)", CALL(SYS_sleep_ms, ~0ul, 0, 0, 0, 0, 0), -EINVAL);
    expect("execve(missing file)", CALL(SYS_execve, "/bin/none", 0, 0, 0, 0, 0), -ENOENT);

    // Call numbers that do not exist, including the gaps in the table.
    static const int known[] = SYS_ALL_NUMBERS;
    int unknown_tried = 0;
    for (long nr = 0; nr < 512; nr++) {
        bool is_known = false;
        for (int k : known) is_known |= k == nr;
        if (is_known) continue;
        unknown_tried++;
        expect("unknown call number", CALL(nr, KERNEL, KERNEL, KERNEL, KERNEL, KERNEL, KERNEL), -ENOSYS);
    }
    expect("call number -1", CALL(-1, 0, 0, 0, 0, 0, 0), -ENOSYS);
    expect("call number 2^63", CALL(0x8000000000000000ul, 0, 0, 0, 0, 0, 0), -ENOSYS);

    // After all that, ordinary calls still work.
    expect("getrandom into a real buffer", CALL(SYS_getrandom, buf, 16, 0, 0, 0, 0), 16);
    expect("close of the real descriptor", CALL(SYS_close, fd, 0, 0, 0, 0, 0), 0);

    printf("badptr: %d checks (%d of them unknown call numbers), %d wrong\n", g_checks, unknown_tried, g_wrong);
    printf("badptr: %s\n", g_wrong ? "FAIL" : "PASS: every bad argument was refused and the kernel is still running");
    return g_wrong ? 1 : 0;
}
