// See fs.h.
#include <fs/block.h>
#include <fs/dev.h>
#include <fs/file.h>
#include <fs/fs.h>
#include <fs/pagecache.h>
#include <fs/lumfs.h>
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <sched/sched.h>

namespace {

const Credentials ROOT = {0, 0};

void make_node(const char* path, VType type, u32 mode, u32 rdev) {
    Result<Vnode*> v = vfs_create(nullptr, path, ROOT, type, mode, true, nullptr, rdev);
    if (v.ok()) vnode_unref(v.value());
    else kprintf("fs: could not create %s: %s\n", path, error_name(v.error()));
}

void make_dir(const char* path, u32 mode) {
    Result<Vnode*> v = vfs_create(nullptr, path, ROOT, VType::Dir, mode, false);
    if (v.ok()) vnode_unref(v.value());
}

Mount* mount_tmpfs(const char* type, const char* path, u32 flags) {
    Result<Mount*> m = tmpfs_create(type, flags);
    if (!m.ok()) {
        kprintf("fs: no memory for %s\n", path);
        return nullptr;
    }
    Result<void> r = vfs_mount(m.value(), path, ROOT);
    if (!r.ok()) {
        kprintf("fs: could not mount %s on %s: %s\n", type, path, error_name(r.error()));
        return nullptr;
    }
    return m.value();
}

} // namespace

void fs_init() {
    dev_init();
    page_cache_init();
    tmpfs_register();
    lumfs_register();

    // The root: the boot archive in a tmpfs, then read-only.
    Result<Mount*> rm = tmpfs_create("initramfs", 0);
    if (!rm.ok()) PANIC("fs: no memory for the root file system");
    Mount* root = rm.value();
    strlcpy(root->source, "boot archive", sizeof root->source);
    const u8* archive;
    usize size;
    usize entries = 0;
    if (boot_archive(&archive, &size)) {
        Result<usize> n = tmpfs_populate_tar(root, archive, size);
        if (n.ok()) entries = n.value();
        else kprintf("fs: boot archive damaged: %s\n", error_name(n.error()));
    } else {
        kprintf("fs: no boot archive among the modules; / is empty\n");
    }
    static const char* const DIRS[] = {"dev", "tmp", "mnt", "bin", "etc", "home"};
    for (const char* d : DIRS) (void)tmpfs_ensure_dir(root, d);
    root->flags |= vfs::MNT_RDONLY;
    vfs_init(root->root, root);

    // Device nodes.
    if (mount_tmpfs("devfs", "/dev", vfs::MNT_NOEXEC | vfs::MNT_NOSUID)) {
        make_node("/dev/null", VType::CharDev, 0666, dev::make(dev::MEM, dev::NULL_));
        make_node("/dev/zero", VType::CharDev, 0666, dev::make(dev::MEM, dev::ZERO));
        make_node("/dev/random", VType::CharDev, 0666, dev::make(dev::MEM, dev::RANDOM));
        make_node("/dev/urandom", VType::CharDev, 0666, dev::make(dev::MEM, dev::URANDOM));
        make_node("/dev/console", VType::CharDev, 0620, dev::make(dev::TTY, dev::CONSOLE));
        make_node("/dev/tty0", VType::CharDev, 0620, dev::make(dev::TTY, dev::TTY0));
        // Screen and input: the window server's alone (phases 10 and 12).
        make_node("/dev/fb0", VType::CharDev, 0600, dev::make(dev::FB, 0));
        make_dir("/dev/input", 0755);
        make_node("/dev/input/kbd0", VType::CharDev, 0600, dev::make(dev::INPUT, dev::KBD));
        make_node("/dev/input/mouse0", VType::CharDev, 0600, dev::make(dev::INPUT, dev::MOUSE));
        make_dir("/dev/disk", 0755);
        for (u32 i = 0; i < block_count(); i++) {
            char path[32] = "/dev/disk/";
            strlcpy(path + 10, block_get(i)->name, sizeof path - 10);
            make_node(path, VType::BlockDev, 0600, dev::make(dev::DISK, i));
        }
    }
    mount_tmpfs("tmpfs", "/tmp", vfs::MNT_NOSUID | vfs::MNT_NODEV);
    vfs_lock();
    vfs_root()->mode = 0755;
    vfs_unlock();

    page_cache_start_writeback();
    kprintf("fs: / from the boot archive (%lu entries, read-only), /dev, /tmp; %u disk(s)\n",
            (unsigned long)entries, block_count());
}
