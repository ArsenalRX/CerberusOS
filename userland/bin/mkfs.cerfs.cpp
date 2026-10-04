// mkfs.cerfs [-L label] [-y] /dev/disk/sdX: formats a disk with cerfs.
// Without -y it only says what it would erase. The formatter is the same
// code the kernel and the build machine's tool use (kernel/fs/cerfs_mkfs.h).
#include <cerberus.h>
// Built with -I kernel (see the Makefile): the shared format code.
#include <fs/cerfs_mkfs.h>
#include <lib/crc32c.cpp>

namespace {

int g_fd = -1;

bool write_block(u64 block, const u8* data) {
    if (lseek(g_fd, (long)(block * cerfs::BLOCK), SEEK_SET) < 0) return false;
    return write(g_fd, data, cerfs::BLOCK) == (long)cerfs::BLOCK;
}

} // namespace

int main(int argc, char** argv) {
    const char* label = "cerberus";
    const char* device = nullptr;
    bool yes = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-L") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "-y")) yes = true;
        else device = argv[i];
    }
    if (!device) {
        dprintf(2, "usage: mkfs.cerfs [-L label] [-y] /dev/disk/sdX\n");
        return 2;
    }
    g_fd = open(device, yes ? O_RDWR : O_RDONLY);
    if (g_fd < 0) {
        dprintf(2, "mkfs.cerfs: %s: %s\n", device, strerror(errno));
        return 1;
    }
    struct stat st;
    fstat(g_fd, &st);
    if (!S_ISBLK(st.st_mode)) {
        dprintf(2, "mkfs.cerfs: %s is not a disk\n", device);
        return 1;
    }
    uint64_t bytes = 0;
    if (ioctl(g_fd, BLKGETSIZE64, &bytes) < 0) {
        dprintf(2, "mkfs.cerfs: %s: cannot read its size: %s\n", device, strerror(errno));
        return 1;
    }
    if (!yes) {
        printf("mkfs.cerfs: this would erase everything on %s (%lu MiB).\n"
               "Run it again with -y to format the disk.\n",
               device, (unsigned long)(bytes >> 20));
        return 1;
    }
    uint8_t uuid[16];
    getrandom(uuid, sizeof uuid, 0);
    uint64_t now = 0;
    cerfs::MkfsResult r = cerfs::mkfs(bytes / cerfs::BLOCK, label, uuid, now, write_block);
    if (!r.ok) {
        dprintf(2, "mkfs.cerfs: %s\n", r.error);
        return 1;
    }
    fsync(g_fd);
    close(g_fd);
    printf("mkfs.cerfs: %s: cerfs, %lu MiB, %u inodes, label \"%s\"\n", device, (unsigned long)(bytes >> 20),
           r.sb.inode_count, label);
    printf("mount it with: mount -t cerfs %s /mnt\n", device);
    return 0;
}
