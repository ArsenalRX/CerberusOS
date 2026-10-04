// VFS rules (SPEC phase 9 and §5A): permissions against a non-root
// credential, mount flags, path resolution edge cases, rename and unlink
// rules, and the descriptor layer. Works in a private tmpfs under /tmp.
#include <fs/file.h>
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <kernel/ktest.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <proc/process.h>

namespace {

const Credentials ROOT = {0, 0};
const Credentials USER = {1000, 1000};

bool make_file(const char* path, u32 mode, const char* text, const Credentials& cred = ROOT) {
    Result<File*> f = file_open(nullptr, path, abi::O_WRONLY | abi::O_CREAT | abi::O_TRUNC, mode, cred);
    if (!f.ok()) return false;
    Result<usize> w = file_write(f.value(), text, strlen(text));
    file_unref(f.value());
    return w.ok();
}

Error open_error(const char* path, u32 flags, const Credentials& cred) {
    Result<File*> f = file_open(nullptr, path, flags, 0644, cred);
    if (f.ok()) {
        file_unref(f.value());
        return Error::None;
    }
    return f.error();
}

Error mkdir_error(const char* path, const Credentials& cred) {
    Result<Vnode*> v = vfs_create(nullptr, path, cred, VType::Dir, 0755, true);
    if (v.ok()) {
        vnode_unref(v.value());
        return Error::None;
    }
    return v.error();
}

} // namespace

int ktest_vfs(int, char**) {
    // A private tmpfs, so the test never touches anything else.
    KTEST_CHECK(mkdir_error("/tmp/vfstest", ROOT) == Error::None);
    Result<Mount*> m = tmpfs_create("tmpfs", 0);
    KTEST_CHECK(m.ok());
    KTEST_CHECK(vfs_mount(m.value(), "/tmp/vfstest", ROOT).ok());

    // --- permissions: a non-root credential against root's files ---
    KTEST_CHECK(make_file("/tmp/vfstest/secret", 0600, "root only\n"));
    KTEST_CHECK(make_file("/tmp/vfstest/public", 0644, "anyone\n"));
    KTEST_CHECK(open_error("/tmp/vfstest/secret", abi::O_RDONLY, USER) == Error::Access);
    KTEST_CHECK(open_error("/tmp/vfstest/secret", abi::O_RDONLY, ROOT) == Error::None);
    KTEST_CHECK(open_error("/tmp/vfstest/public", abi::O_RDONLY, USER) == Error::None);
    KTEST_CHECK(open_error("/tmp/vfstest/public", abi::O_WRONLY, USER) == Error::Access);
    // The directory belongs to root, mode 0755: a user cannot create in it.
    KTEST_CHECK(open_error("/tmp/vfstest/mine", abi::O_WRONLY | abi::O_CREAT, USER) == Error::Access);
    // A directory without search permission hides what is inside.
    KTEST_CHECK(mkdir_error("/tmp/vfstest/closed", ROOT) == Error::None);
    KTEST_CHECK(make_file("/tmp/vfstest/closed/inner", 0644, "x"));
    {
        Result<Vnode*> d = vfs_resolve(nullptr, "/tmp/vfstest/closed", ROOT, LookupFlags{});
        KTEST_CHECK(d.ok());
        KTEST_CHECK(vfs_chmod(d.value(), ROOT, 0700).ok());
        // Only the owner (or root) may change the mode.
        KTEST_CHECK(vfs_chmod(d.value(), USER, 0777).error() == Error::Perm);
        vnode_unref(d.value());
    }
    KTEST_CHECK(open_error("/tmp/vfstest/closed/inner", abi::O_RDONLY, USER) == Error::Access);
    // Root may not execute a file with no execute bit at all.
    {
        Result<Vnode*> v = vfs_resolve(nullptr, "/tmp/vfstest/public", ROOT, LookupFlags{});
        KTEST_CHECK(v.ok());
        KTEST_CHECK(vfs_access(v.value(), ROOT, vfs::X_OK).error() == Error::Access);
        vnode_unref(v.value());
    }
    kprintf("  permissions: a non-root user cannot read a 0600 root file, write a 0644 one, create in a\n"
            "  root directory or look inside a 0700 one; root cannot execute a file without x bits\n");

    // --- mount flags: noexec, nodev, ro ---
    KTEST_CHECK(mkdir_error("/tmp/vfstest/nx", ROOT) == Error::None);
    Result<Mount*> nx = tmpfs_create("tmpfs", vfs::MNT_NOEXEC | vfs::MNT_NODEV);
    KTEST_CHECK(nx.ok());
    KTEST_CHECK(vfs_mount(nx.value(), "/tmp/vfstest/nx", ROOT).ok());
    KTEST_CHECK(make_file("/tmp/vfstest/nx/prog", 0755, "#!"));
    {
        Result<Vnode*> v = vfs_resolve(nullptr, "/tmp/vfstest/nx/prog", ROOT, LookupFlags{});
        KTEST_CHECK(v.ok());
        KTEST_CHECK(vfs_access(v.value(), ROOT, vfs::X_OK).error() == Error::Access);
        vnode_unref(v.value());
    }
    // A program copied there will not start: the loader asks for X_OK.
    {
        Result<Vnode*> src = vfs_resolve(nullptr, "/bin/hello", ROOT, LookupFlags{});
        KTEST_CHECK(src.ok());
        usize size = 0;
        Result<u8*> data = vfs_read_all(src.value(), 1 << 22, &size);
        vnode_unref(src.value());
        KTEST_CHECK(data.ok());
        Result<File*> f = file_open(nullptr, "/tmp/vfstest/nx/hello", abi::O_WRONLY | abi::O_CREAT, 0755, ROOT);
        KTEST_CHECK(f.ok());
        KTEST_CHECK(file_write(f.value(), data.value(), size).ok());
        file_unref(f.value());
        kfree(data.value());
        const char* const argv[] = {"hello", nullptr};
        Result<Process*> p = process_spawn("/tmp/vfstest/nx/hello", argv, false);
        KTEST_CHECK(p.ok());
        int status = process_wait(p.value());
        KTEST_CHECK(((status >> 8) & 0xFF) == 127);      // the loader refused it
    }
    // Device nodes do not open on a nodev mount.
    {
        Result<Vnode*> d = vfs_create(nullptr, "/tmp/vfstest/nx/zero", ROOT, VType::CharDev, 0666, true, nullptr,
                                      (1u << 16) | 5);
        KTEST_CHECK(d.ok());
        vnode_unref(d.value());
        KTEST_CHECK(open_error("/tmp/vfstest/nx/zero", abi::O_RDONLY, ROOT) == Error::Access);
    }
    // The root file system is read-only.
    KTEST_CHECK(mkdir_error("/newdir", ROOT) == Error::ReadOnly);
    KTEST_CHECK(open_error("/bin/hello", abi::O_WRONLY, ROOT) == Error::ReadOnly);
    kprintf("  mount flags: noexec refuses to run a copied program, nodev refuses a device node,\n"
            "  the read-only root refuses writes\n");

    // --- paths ---
    KTEST_CHECK(mkdir_error("/tmp/vfstest/a", ROOT) == Error::None);
    KTEST_CHECK(mkdir_error("/tmp/vfstest/a/b", ROOT) == Error::None);
    {
        // "..", "." and repeated slashes.
        Result<Vnode*> x = vfs_resolve(nullptr, "/tmp/vfstest/a/b/../b/./..//b", ROOT, LookupFlags{});
        Result<Vnode*> y = vfs_resolve(nullptr, "/tmp/vfstest/a/b", ROOT, LookupFlags{});
        KTEST_CHECK(x.ok() && y.ok() && x.value() == y.value());
        char path[PATH_MAX];
        KTEST_CHECK(vfs_path_of(x.value(), path, sizeof path).ok());
        KTEST_CHECK(strcmp(path, "/tmp/vfstest/a/b") == 0);
        vnode_unref(x.value());
        vnode_unref(y.value());
        // ".." at the root stays at the root.
        Result<Vnode*> r = vfs_resolve(nullptr, "/../../..", ROOT, LookupFlags{});
        KTEST_CHECK(r.ok() && r.value() == vfs_root());
        vnode_unref(r.value());
    }
    // Symbolic links: followed, not followed on request, and loops stop.
    {
        Result<Vnode*> l = vfs_create(nullptr, "/tmp/vfstest/link", ROOT, VType::Symlink, 0777, true, "a/b");
        KTEST_CHECK(l.ok());
        vnode_unref(l.value());
        Result<Vnode*> t = vfs_resolve(nullptr, "/tmp/vfstest/link", ROOT, LookupFlags{});
        KTEST_CHECK(t.ok() && t.value()->type == VType::Dir);
        vnode_unref(t.value());
        KTEST_CHECK(open_error("/tmp/vfstest/link", abi::O_RDONLY | abi::O_NOFOLLOW, ROOT) == Error::Loop);
        Result<Vnode*> l1 = vfs_create(nullptr, "/tmp/vfstest/loop1", ROOT, VType::Symlink, 0777, true, "loop2");
        Result<Vnode*> l2 = vfs_create(nullptr, "/tmp/vfstest/loop2", ROOT, VType::Symlink, 0777, true, "loop1");
        KTEST_CHECK(l1.ok() && l2.ok());
        vnode_unref(l1.value());
        vnode_unref(l2.value());
        KTEST_CHECK(vfs_resolve(nullptr, "/tmp/vfstest/loop1", ROOT, LookupFlags{}).error() == Error::Loop);
    }
    // Names too long; a file used as a directory.
    {
        char longname[300];
        memset(longname, 'n', sizeof longname);
        memcpy(longname, "/tmp/", 5);
        longname[5 + 260] = 0;
        KTEST_CHECK(vfs_resolve(nullptr, longname, ROOT, LookupFlags{}).error() == Error::NameTooLong);
        KTEST_CHECK(vfs_resolve(nullptr, "/tmp/vfstest/public/x", ROOT, LookupFlags{}).error() == Error::NotDir);
        KTEST_CHECK(open_error("/tmp/vfstest/public", abi::O_RDONLY | abi::O_DIRECTORY, ROOT) == Error::NotDir);
        KTEST_CHECK(open_error("/tmp/vfstest/a", abi::O_WRONLY, ROOT) == Error::IsDir);
        KTEST_CHECK(open_error("/tmp/vfstest/public", abi::O_WRONLY | abi::O_CREAT | abi::O_EXCL, ROOT) ==
                    Error::Exists);
    }
    kprintf("  paths: . and .. resolve, .. stops at /, symlinks followed (or refused with O_NOFOLLOW),\n"
            "  a symlink loop ends in ELOOP, long names and files-as-directories are refused\n");

    // --- rename and unlink rules ---
    KTEST_CHECK(vfs_rename(nullptr, "/tmp/vfstest/a", "/tmp/vfstest/a/b/inside", ROOT).error() == Error::Invalid);
    KTEST_CHECK(vfs_rename(nullptr, "/tmp/vfstest/public", "/tmp/vfstest/nx/public", ROOT).error() ==
                Error::CrossDevice);
    KTEST_CHECK(vfs_unlink(nullptr, "/tmp/vfstest/a", ROOT, true).error() == Error::NotEmpty);
    KTEST_CHECK(vfs_unlink(nullptr, "/tmp/vfstest/a", ROOT, false).error() == Error::IsDir);
    KTEST_CHECK(vfs_unlink(nullptr, "/tmp/vfstest/nx", ROOT, true).error() == Error::Busy);     // a mount point
    KTEST_CHECK(vfs_rename(nullptr, "/tmp/vfstest/public", "/tmp/vfstest/a/moved", ROOT).ok());
    KTEST_CHECK(open_error("/tmp/vfstest/a/moved", abi::O_RDONLY, ROOT) == Error::None);
    KTEST_CHECK(open_error("/tmp/vfstest/public", abi::O_RDONLY, ROOT) == Error::NotFound);
    // A file stays readable through an open descriptor after its name goes.
    {
        Result<File*> f = file_open(nullptr, "/tmp/vfstest/a/moved", abi::O_RDONLY, 0, ROOT);
        KTEST_CHECK(f.ok());
        KTEST_CHECK(vfs_unlink(nullptr, "/tmp/vfstest/a/moved", ROOT, false).ok());
        char buf[16] = {};
        Result<usize> n = file_read(f.value(), buf, sizeof buf - 1);
        KTEST_CHECK(n.ok() && strcmp(buf, "anyone\n") == 0);
        file_unref(f.value());
    }
    kprintf("  rename/unlink: no directory into itself, no rename across file systems, rmdir needs an\n"
            "  empty directory, a mount point is busy, an unlinked open file stays readable\n");

    // --- unmount: busy while used, then clean ---
    {
        Result<File*> f = file_open(nullptr, "/tmp/vfstest/nx/prog", abi::O_RDONLY, 0, ROOT);
        KTEST_CHECK(f.ok());
        KTEST_CHECK(vfs_unmount("/tmp/vfstest/nx", ROOT).error() == Error::Busy);
        file_unref(f.value());
        KTEST_CHECK(vfs_unmount("/tmp/vfstest/nx", USER).error() == Error::Perm);
        KTEST_CHECK(vfs_unmount("/tmp/vfstest/nx", ROOT).ok());
    }
    KTEST_CHECK(vfs_unmount("/tmp/vfstest", ROOT).ok());
    KTEST_CHECK(vfs_unlink(nullptr, "/tmp/vfstest", ROOT, true).ok());
    kprintf("  unmount: refused while a file is open or for a non-root user; both test mounts removed\n");
    return 0;
}
