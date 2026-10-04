// cerfs fuzzer (SPEC §5A phase 9: "a corrupted image yields Error::IO, never
// a panic"). A small cerfs with files, directories, an indirect block and a
// symlink is built on a RAM disk; then, until the time is up, a copy is
// damaged at random and handed to the real driver: mount, walk and read
// everything, write, rename, delete, unmount. Errors are expected; a panic,
// a hang or a leaked reference (unmount refused) is a failure.
//
// Half of the time the checksums of the damaged blocks are recomputed, so
// the damage gets past the CRC checks and reaches the field validation.
//
//   test cerfsfuzz [seconds]     (default 60; `make fuzz` runs it)
#include <drivers/ramdisk.h>
#include <drivers/refclock.h>
#include <fs/cerfs_format.h>
#include <fs/cerfs_mkfs.h>
#include <fs/dev.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <kernel/ktest.h>
#include <lib/csprng.h>
#include <lib/kprintf.h>
#include <lib/string.h>
#include <mm/kheap.h>

using namespace cerfs;

namespace {

const Credentials ROOT = {0, 0};
constexpr u64 DISK_BYTES = 4 * MIB;
constexpr const char* MNT = "/tmp/cerfsfuzz";

RamDisk* g_disk = nullptr;
u64 g_rng = 0;

u64 rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

Error mount_disk() {
    Result<Mount*> m = vfs_make_mount("cerfs", "/dev/disk/fuzz0", 0);
    if (!m.ok()) return m.error();
    Result<void> r = vfs_mount(m.value(), MNT, ROOT);
    if (!r.ok()) {
        vnode_unref(m.value()->root);
        if (m.value()->unmount) (void)m.value()->unmount(m.value());
        return r.error();
    }
    return Error::None;
}

void write_file(const char* path, usize size) {
    Result<File*> f = file_open(nullptr, path, abi::O_WRONLY | abi::O_CREAT | abi::O_TRUNC, 0644, ROOT);
    if (!f.ok()) return;
    u8 buf[512];
    for (usize done = 0; done < size;) {
        usize n = size - done < sizeof buf ? size - done : sizeof buf;
        for (usize i = 0; i < n; i++) buf[i] = (u8)(done + i);
        if (!file_write(f.value(), buf, n).ok()) break;
        done += n;
    }
    file_unref(f.value());
}

void make_dir(const char* path) {
    Result<Vnode*> v = vfs_create(nullptr, path, ROOT, VType::Dir, 0755, false);
    if (v.ok()) vnode_unref(v.value());
}

// Reads everything reachable, with limits so a damaged tree (a loop of
// directories, say) cannot make the walk endless.
void walk(Vnode* dir, int depth, u32* budget) {
    u64 cookie = 0;
    DirEntry e;
    for (int k = 0; k < 64 && *budget; k++) {
        Result<bool> more = vfs_readdir(dir, &cookie, &e);
        if (!more.ok() || !more.value()) return;
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        (*budget)--;
        LookupFlags lf;
        lf.follow_last = false;
        Result<Vnode*> v = vfs_resolve(dir, e.name, ROOT, lf);
        if (!v.ok()) continue;
        Vnode* c = v.value();
        if (c->type == VType::File) {
            u8 buf[1024];
            for (u64 off = 0; off < 65536; off += sizeof buf) {
                Result<usize> n = vfs_read(c, off, buf, sizeof buf);
                if (!n.ok() || n.value() == 0) break;
            }
        } else if (c->type == VType::Symlink) {
            char t[256];
            (void)vfs_readlink(c, t, sizeof t);
        } else if (c->type == VType::Dir && depth < 6) {
            walk(c, depth + 1, budget);
        }
        vnode_unref(c);
    }
}

void exercise() {
    Result<Vnode*> root = vfs_resolve(nullptr, MNT, ROOT, LookupFlags{});
    if (root.ok()) {
        u32 budget = 400;
        walk(root.value(), 0, &budget);
        vnode_unref(root.value());
    }
    write_file("/tmp/cerfsfuzz/fresh", 20000);
    make_dir("/tmp/cerfsfuzz/newdir");
    (void)vfs_rename(nullptr, "/tmp/cerfsfuzz/fresh", "/tmp/cerfsfuzz/newdir/moved", ROOT);
    (void)vfs_unlink(nullptr, "/tmp/cerfsfuzz/a/small", ROOT, false);
    (void)vfs_unlink(nullptr, "/tmp/cerfsfuzz/newdir/moved", ROOT, false);
    (void)vfs_unlink(nullptr, "/tmp/cerfsfuzz/newdir", ROOT, true);
    Result<File*> f = file_open(nullptr, "/tmp/cerfsfuzz/large", abi::O_WRONLY, 0, ROOT);
    if (f.ok()) {
        (void)vfs_truncate(f.value()->vnode, 5000);
        file_unref(f.value());
    }
    vfs_sync();
}

// Damages the image: a few bytes in metadata blocks (or the first data
// blocks, where directories and indirect blocks live), then maybe reseals.
void mutate(u8* img, const SuperBlock& sb) {
    u32 hits = 1 + (u32)(rnd() % 6);
    u64 hot = sb.data_start + 32;           // metadata + the first data blocks
    for (u32 i = 0; i < hits; i++) {
        u64 block = rnd() % 8 == 0 ? rnd() % (DISK_BYTES / BLOCK) : rnd() % hot;
        u8* b = img + block * BLOCK;
        u32 off = (u32)(rnd() % BLOCK);
        switch (rnd() % 4) {
        case 0: b[off] ^= (u8)(1u << (rnd() % 8)); break;               // one bit
        case 1: b[off] = (u8)rnd(); break;                               // one byte
        case 2: {                                                        // a 32-bit field to an edge value
            static const u32 edges[] = {0, 1, 0x7FFFFFFF, 0xFFFFFFFF, 0x80000000, 4096, 0x10000};
            u32 v = edges[rnd() % 7];
            memcpy(b + (off & ~3u), &v, 4);
            break;
        }
        default: {                                                       // a 64-bit field
            u64 v = rnd() % 3 == 0 ? ~0ull : rnd();
            memcpy(b + (off & ~7u), &v, 8);
            break;
        }
        }
        if (rnd() % 2 == 0) continue;
        // Reseal so the damage passes the checksum.
        if (block == 0) {
            SuperBlock s;
            memcpy(&s, b, sizeof s);
            s.crc = super_crc(s);
            memcpy(b, &s, sizeof s);
        } else if (block >= sb.inode_table_start && block < sb.inode_table_start + sb.inode_table_blocks) {
            for (u32 k = 0; k < INODES_PER_BLOCK; k++) {
                Inode* in = (Inode*)(b + k * INODE_SIZE);
                if (in->mode) in->crc = inode_crc(*in);
            }
        } else {
            u32 magic;
            memcpy(&magic, b, 4);
            if (magic == MAGIC_BITMAP || magic == MAGIC_DIR || magic == MAGIC_INDIRECT || magic == MAGIC_JHEAD ||
                magic == MAGIC_JDESC || magic == MAGIC_JCOMMIT)
                seal_block(b);
        }
    }
}

} // namespace

int ktest_cerfsfuzz(int argc, char** argv) {
    u64 seconds = 60;
    if (argc > 1) {
        seconds = 0;
        for (const char* p = argv[1]; *p >= '0' && *p <= '9'; p++) seconds = seconds * 10 + (u64)(*p - '0');
        if (!seconds) seconds = 60;
    }
    if (!g_disk) {
        Result<RamDisk*> d = ramdisk_create(DISK_BYTES);
        KTEST_CHECK(d.ok());
        g_disk = d.value();
        Result<Vnode*> node = vfs_create(nullptr, "/dev/disk/fuzz0", ROOT, VType::BlockDev, 0600, true, nullptr,
                                         dev::make(dev::DISK, g_disk->minor));
        KTEST_CHECK(node.ok());
        vnode_unref(node.value());
        make_dir(MNT);
    }
    csprng_bytes(&g_rng, sizeof g_rng);
    g_rng |= 1;
    kprintf("  seed %#lx\n", (unsigned long)g_rng);

    // A pristine image with something in it.
    u8 uuid[16] = {1, 2, 3, 4};
    MkfsResult made = mkfs(DISK_BYTES / BLOCK, "fuzz", uuid, vfs_now(), [](u64 b, const u8* data) {
        memcpy(g_disk->data + b * BLOCK, data, BLOCK);
        return true;
    });
    KTEST_CHECK(made.ok);
    KTEST_CHECK(mount_disk() == Error::None);
    make_dir("/tmp/cerfsfuzz/a");
    make_dir("/tmp/cerfsfuzz/a/b");
    write_file("/tmp/cerfsfuzz/a/small", 100);
    write_file("/tmp/cerfsfuzz/a/b/medium", 20000);
    write_file("/tmp/cerfsfuzz/large", 200000);         // past the direct blocks: an indirect block
    {
        Result<Vnode*> l = vfs_create(nullptr, "/tmp/cerfsfuzz/link", ROOT, VType::Symlink, 0777, true, "a/b/medium");
        KTEST_CHECK(l.ok());
        vnode_unref(l.value());
    }
    KTEST_CHECK(vfs_unmount(MNT, ROOT).ok());
    u8* pristine = (u8*)kmalloc(DISK_BYTES);
    KTEST_CHECK(pristine != nullptr);
    memcpy(pristine, g_disk->data, DISK_BYTES);

    u64 end = refclock_now_us() + seconds * 1000000;
    u64 rounds = 0, mounted = 0, refused = 0;
    while (refclock_now_us() < end) {
        memcpy(g_disk->data, pristine, DISK_BYTES);
        mutate(g_disk->data, made.sb);
        rounds++;
        if (mount_disk() != Error::None) {
            refused++;
            continue;
        }
        mounted++;
        exercise();
        Result<void> u = vfs_unmount(MNT, ROOT);
        if (!u.ok() && u.error() == Error::Busy) {
            kprintf("  round %lu: unmount refused (a reference leaked)\n", (unsigned long)rounds);
            kfree(pristine);
            return 1;
        }
    }
    kfree(pristine);
    kprintf("  cerfs fuzz: %lu damaged images in %lu s (%lu mounted and exercised, %lu refused at mount); "
            "the kernel is still running\n",
            (unsigned long)rounds, (unsigned long)seconds, (unsigned long)mounted, (unsigned long)refused);
    return 0;
}
