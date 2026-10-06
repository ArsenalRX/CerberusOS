// Small file tools in one program, chosen by the name it is started as
// (like busybox): rmdir, mv, cp, touch, stat, ln, sync, mount, umount, pwd,
// write, poweroff, reboot. The boot archive carries one copy per name.
#include <cerberus.h>

namespace {

int fail(const char* tool, const char* what) {
    dprintf(2, "%s: %s: %s\n", tool, what, strerror(errno));
    return 1;
}

const char* base(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') b = p + 1;
    return b;
}

int copy_file(const char* from, const char* to) {
    int in = open(from, O_RDONLY);
    if (in < 0) return fail("cp", from);
    struct stat st;
    fstat(in, &st);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, (int)(st.st_mode & 0777));
    if (out < 0) {
        close(in);
        return fail("cp", to);
    }
    static char buf[32768];
    int rc = 0;
    for (;;) {
        long n = read(in, buf, sizeof buf);
        if (n < 0) {
            rc = fail("cp", from);
            break;
        }
        if (n == 0) break;
        for (long done = 0; done < n;) {
            long w = write(out, buf + done, (size_t)(n - done));
            if (w <= 0) {
                rc = fail("cp", to);
                n = done = 0;
                break;
            }
            done += w;
        }
        if (rc) break;
    }
    close(in);
    close(out);
    return rc;
}

// Target path for "cp/mv src dir": dir/basename(src).
const char* into_dir(const char* src, const char* dst, char* buf, size_t n) {
    struct stat st;
    if (stat(dst, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(buf, n, "%s/%s", dst, base(src));
        return buf;
    }
    return dst;
}

int cmd_stat(int argc, char** argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) < 0) {
            rc = fail("stat", argv[i]);
            continue;
        }
        const char* type = S_ISDIR(st.st_mode)   ? "directory"
                           : S_ISLNK(st.st_mode) ? "symbolic link"
                           : S_ISCHR(st.st_mode) ? "character device"
                           : S_ISBLK(st.st_mode) ? "block device"
                                                 : "regular file";
        printf("  File: %s\n  Type: %s\n  Size: %lu bytes (%lu blocks)\n  Inode: %lu  Links: %u\n"
               "  Mode: %04o  Owner: %u  Group: %u\n  Modified: %lu (seconds since 1970)\n",
               argv[i], type, (unsigned long)st.st_size, (unsigned long)st.st_blocks, (unsigned long)st.st_ino,
               st.st_nlink, st.st_mode & 07777, st.st_uid, st.st_gid, (unsigned long)st.st_mtime);
    }
    return rc;
}

unsigned parse_flags(const char* opts) {
    unsigned f = 0;
    for (const char* p = opts; *p;) {
        const char* e = p;
        while (*e && *e != ',') e++;
        size_t n = (size_t)(e - p);
        if (n == 2 && !strncmp(p, "ro", 2)) f |= MS_RDONLY;
        else if (n == 6 && !strncmp(p, "noexec", 6)) f |= MS_NOEXEC;
        else if (n == 6 && !strncmp(p, "nosuid", 6)) f |= MS_NOSUID;
        else if (n == 5 && !strncmp(p, "nodev", 5)) f |= MS_NODEV;
        p = *e ? e + 1 : e;
    }
    return f;
}

} // namespace

int main(int argc, char** argv) {
    const char* tool = base(argv[0]);
    if (!strcmp(tool, "rmdir")) {
        int rc = 0;
        for (int i = 1; i < argc; i++)
            if (rmdir(argv[i]) < 0) rc = fail(tool, argv[i]);
        return rc;
    }
    if (!strcmp(tool, "mv") || !strcmp(tool, "cp")) {
        if (argc != 3) {
            dprintf(2, "usage: %s from to\n", tool);
            return 2;
        }
        char buf[512];
        const char* to = into_dir(argv[1], argv[2], buf, sizeof buf);
        if (!strcmp(tool, "cp")) return copy_file(argv[1], to);
        if (rename(argv[1], to) == 0) return 0;
        if (errno != EXDEV) return fail(tool, argv[1]);
        // Across file systems: copy, then remove the original.
        if (copy_file(argv[1], to)) return 1;
        return unlink(argv[1]) < 0 ? fail(tool, argv[1]) : 0;
    }
    if (!strcmp(tool, "touch")) {
        int rc = 0;
        for (int i = 1; i < argc; i++) {
            int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
            if (fd < 0) rc = fail(tool, argv[i]);
            else close(fd);
        }
        return rc;
    }
    if (!strcmp(tool, "stat")) return cmd_stat(argc, argv);
    if (!strcmp(tool, "ln")) {
        if (argc == 4 && !strcmp(argv[1], "-s")) return symlink(argv[2], argv[3]) < 0 ? fail(tool, argv[3]) : 0;
        if (argc == 3) return link(argv[1], argv[2]) < 0 ? fail(tool, argv[2]) : 0;
        dprintf(2, "usage: ln [-s] target linkname\n");
        return 2;
    }
    if (!strcmp(tool, "sync")) {
        sync();
        return 0;
    }
    if (!strcmp(tool, "mount")) {
        // mount -t type [-o opts] source target
        const char *type = nullptr, *opts = "", *src = nullptr, *dst = nullptr;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-t") && i + 1 < argc) type = argv[++i];
            else if (!strcmp(argv[i], "-o") && i + 1 < argc) opts = argv[++i];
            else if (!src) src = argv[i];
            else dst = argv[i];
        }
        if (!type || !src || !dst) {
            dprintf(2, "usage: mount -t type [-o ro,noexec,nosuid,nodev] source target\n");
            return 2;
        }
        return mount(src, dst, type, parse_flags(opts)) < 0 ? fail(tool, dst) : 0;
    }
    if (!strcmp(tool, "umount")) {
        if (argc != 2) {
            dprintf(2, "usage: umount target\n");
            return 2;
        }
        return umount(argv[1]) < 0 ? fail(tool, argv[1]) : 0;
    }
    if (!strcmp(tool, "pwd")) {
        char buf[256];
        if (!getcwd(buf, sizeof buf)) return fail(tool, ".");
        printf("%s\n", buf);
        return 0;
    }
    if (!strcmp(tool, "write")) {
        // write file text...: replaces the file's contents with the words.
        if (argc < 2) {
            dprintf(2, "usage: write file [text...]\n");
            return 2;
        }
        int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) return fail(tool, argv[1]);
        for (int i = 2; i < argc; i++) {
            if (i > 2) write(fd, " ", 1);
            write(fd, argv[i], strlen(argv[i]));
        }
        write(fd, "\n", 1);
        close(fd);
        return 0;
    }
    if (!strcmp(tool, "poweroff") || !strcmp(tool, "reboot")) {
        reboot(tool[0] == 'p' ? REBOOT_POWER_OFF : REBOOT_RESTART);
        return fail(tool, "the system");
    }
    dprintf(2, "fileutils: started as unknown name '%s'\n", tool);
    return 2;
}
