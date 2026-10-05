// See fs.h.
#include <fs/block.h>
#include <fs/dev.h>
#include <fs/file.h>
#include <fs/fs.h>
#include <fs/pagecache.h>
#include <fs/cerfs.h>
#include <fs/cerfs_format.h>
#include <fs/cerfs_mkfs.h>
#include <lib/csprng.h>
#include <mm/kheap.h>
#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <sched/sched.h>

namespace {

const Credentials ROOT = {0, 0};
bool read_super(u32 minor, cerfs::SuperBlock* sb);
Result<void> mount_data(u32 minor);

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
    cerfs_register();

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
    static const char* const DIRS[] = {"dev", "tmp", "mnt", "bin", "etc", "home", "data"};
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
    // /tmp: anyone may create files; the sticky bit keeps users from removing
    // each other's.
    if (Mount* tmp = mount_tmpfs("tmpfs", "/tmp", vfs::MNT_NOSUID | vfs::MNT_NODEV)) {
        vfs_lock();
        tmp->root->mode = 01777;
        vfs_unlock();
    }
    vfs_lock();
    vfs_root()->mode = 0755;
    vfs_unlock();

    page_cache_start_writeback();
    kprintf("fs: / from the boot archive (%lu entries, read-only), /dev, /tmp; %u disk(s)\n",
            (unsigned long)entries, block_count());
    // The first disk labelled "data" becomes /data (settings, reminders, the
    // user's files). Other cerfs disks are left for `mount`.
    for (u32 i = 0; i < block_count(); i++) {
        cerfs::SuperBlock sb;
        if (!read_super(i, &sb) || strcmp(sb.label, "data") != 0) continue;
        Result<void> r = mount_data(i);
        if (!r.ok()) kprintf("fs: could not mount %s on /data: %s\n", block_get(i)->name, error_name(r.error()));
        break;
    }
}

// ---------------------------------------------------------------- /data --
// Reads a disk's first block raw (nothing of an unmounted disk is cached)
// and says whether it holds a cerfs volume, and with which label.
namespace {

constexpr u32 SECTORS_PER_BLOCK = cerfs::BLOCK / 512;

bool read_super(u32 minor, cerfs::SuperBlock* sb) {
    BlockDevice* d = block_get(minor);
    if (!d || d->sector_size != 512 || d->sectors < SECTORS_PER_BLOCK) return false;
    u8* b = (u8*)kmalloc(cerfs::BLOCK);
    if (!b) return false;
    bool ok = d->read(d, 0, SECTORS_PER_BLOCK, b).ok();
    if (ok) memcpy(sb, b, sizeof *sb);
    kfree(b);
    if (!ok) return false;
    if (memcmp(sb->magic, cerfs::MAGIC, sizeof cerfs::MAGIC) != 0) return false;
    sb->label[sizeof sb->label - 1] = 0;        // never trust a label to be terminated
    return true;
}

char g_data_disk[8];

Result<void> mount_data(u32 minor) {
    char source[32] = "/dev/disk/";
    strlcpy(source + 10, block_get(minor)->name, sizeof source - 10);
    Result<Mount*> m = vfs_make_mount("cerfs", source, vfs::MNT_NOSUID | vfs::MNT_NODEV);
    if (!m.ok()) return m.error();
    Result<void> r = vfs_mount(m.value(), "/data", ROOT);
    if (!r.ok()) {
        vnode_unref(m.value()->root);
        if (m.value()->unmount) (void)m.value()->unmount(m.value());
        return r.error();
    }
    strlcpy(g_data_disk, block_get(minor)->name, sizeof g_data_disk);
    kprintf("fs: /data is %s (cerfs \"data\")\n", source);
    return {};
}

} // namespace

bool fs_data_mounted() { return g_data_disk[0] != 0; }
const char* fs_data_disk() { return g_data_disk; }

u32 fs_disks(DiskInfo* out, u32 max) {
    u32 n = 0;
    for (u32 i = 0; i < block_count() && n < max; i++) {
        BlockDevice* d = block_get(i);
        if (!d) continue;
        DiskInfo& di = out[n++];
        strlcpy(di.name, d->name, sizeof di.name);
        di.mib = d->sectors * d->sector_size / MIB;
        cerfs::SuperBlock sb;
        if (strcmp(d->name, g_data_disk) == 0) di.state = DiskInfo::Data;
        else if (block_claimed(i)) di.state = DiskInfo::Busy;
        else if (read_super(i, &sb)) di.state = DiskInfo::Cerfs;
        else di.state = DiskInfo::Blank;
    }
    return n;
}

Result<void> fs_data_format(const char* name) {
    if (fs_data_mounted()) return Error::Busy;
    u32 minor = ~0u;
    for (u32 i = 0; i < block_count(); i++)
        if (strcmp(block_get(i)->name, name) == 0) minor = i;
    if (minor == ~0u) return Error::NoDevice;
    if (block_claimed(minor)) return Error::Busy;
    BlockDevice* d = block_get(minor);
    if (d->sector_size != 512) return Error::NotSupported;
    // Whatever the page cache holds of this disk (someone may have read it
    // through /dev/disk) must not come back after the format.
    char source[32] = "/dev/disk/";
    strlcpy(source + 10, name, sizeof source - 10);
    Result<Vnode*> dn = vfs_resolve(nullptr, source, ROOT, LookupFlags{});
    if (dn.ok()) {
        page_drop_owner(dn.value(), 0);
        vnode_unref(dn.value());
    }
    u8 uuid[16];
    csprng_bytes(uuid, sizeof uuid);
    bool io_ok = true;
    cerfs::MkfsResult r = cerfs::mkfs(d->sectors / SECTORS_PER_BLOCK, "data", uuid, vfs_now(), [&](u64 block, const void* data) {
        if (!d->write(d, block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK, data).ok()) io_ok = false;
        return io_ok;
    });
    if (d->flush) (void)d->flush(d);
    if (!r.ok || !io_ok) {
        kprintf("fs: formatting %s failed: %s\n", name, r.ok ? "I/O error" : r.error);
        return Error::IO;
    }
    kprintf("fs: %s formatted as cerfs \"data\" (%lu MiB)\n", name, (unsigned long)(d->sectors * 512 / MIB));
    return mount_data(minor);
}
