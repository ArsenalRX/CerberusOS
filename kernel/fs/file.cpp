// See file.h.
#include <boot/bootinfo.h>
#include <fs/dev.h>
#include <fs/file.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <sched/sched.h>

bool boot_archive(const u8** data, usize* size) {
    const BootInfo& bi = g_boot_info;
    for (usize i = 0; i < bi.module_count; i++) {
        usize len = strlen(bi.modules[i].path);
        if (len >= 4 && strcmp(bi.modules[i].path + len - 4, ".tar") == 0) {
            *data = (const u8*)bi.modules[i].address;
            *size = bi.modules[i].size;
            return true;
        }
    }
    return false;
}

namespace {

Result<File*> make_file(Vnode* v, u32 flags) {
    File* f = (File*)kzalloc(sizeof(File));
    if (!f) return Error::NoMemory;
    f->refs = 1;
    f->flags = flags & (abi::O_ACCMODE | abi::O_APPEND | abi::O_NONBLOCK);
    f->vnode = v;               // takes the caller's reference
    return f;
}

u8 dirent_type(VType t) {
    switch (t) {
    case VType::File: return abi::DT_REG;
    case VType::Dir: return abi::DT_DIR;
    case VType::Symlink: return abi::DT_LNK;
    case VType::CharDev: return abi::DT_CHR;
    case VType::BlockDev: return abi::DT_BLK;
    }
    return abi::DT_UNKNOWN;
}

} // namespace

Result<File*> file_open(Vnode* start, const char* path, u32 flags, u32 mode, const Credentials& cred) {
    if (flags & ~abi::O_KNOWN) return Error::Invalid;
    u32 acc = flags & abi::O_ACCMODE;
    if (acc == 3) return Error::Invalid;
    bool want_write = acc != abi::O_RDONLY;
    bool follow = !(flags & abi::O_NOFOLLOW);

    Vnode* v = nullptr;
    bool created = false;
    if (flags & abi::O_CREAT) {
        Result<Vnode*> c = vfs_create(start, path, cred, VType::File, mode, flags & abi::O_EXCL, nullptr, 0, &created);
        if (!c.ok()) return c.error();
        v = c.value();
        // An existing symlink is followed like any other open.
        if (v->type == VType::Symlink && follow && !(flags & abi::O_EXCL)) {
            vnode_unref(v);
            Result<Vnode*> r = vfs_resolve(start, path, cred, LookupFlags{});
            if (!r.ok()) return r.error();
            v = r.value();
            created = false;
        }
    } else {
        LookupFlags lf;
        lf.follow_last = follow;
        Result<Vnode*> r = vfs_resolve(start, path, cred, lf);
        if (!r.ok()) return r.error();
        v = r.value();
    }

    Result<void> ok;
    if (v->type == VType::Symlink) ok = Error::Loop;                      // O_NOFOLLOW
    else if ((flags & abi::O_DIRECTORY) && v->type != VType::Dir) ok = Error::NotDir;
    else if (v->type == VType::Dir && want_write) ok = Error::IsDir;
    if (ok.ok() && !created) {
        u32 want = (acc == abi::O_RDONLY ? vfs::R_OK : acc == abi::O_WRONLY ? vfs::W_OK : vfs::R_OK | vfs::W_OK);
        ok = vfs_access(v, cred, want);
    }
    if (ok.ok() && (v->type == VType::CharDev || v->type == VType::BlockDev)) {
        if (v->mount && (v->mount->flags & vfs::MNT_NODEV)) ok = Error::Access;
        else ok = dev_open(v, want_write);
    }
    if (ok.ok() && (flags & abi::O_TRUNC) && want_write && v->type == VType::File && v->size) ok = vfs_truncate(v, 0);
    if (!ok.ok()) {
        vnode_unref(v);
        return ok.error();
    }
    Result<File*> f = make_file(v, flags);
    if (!f.ok()) vnode_unref(v);
    return f;
}

Result<File*> file_open_console() {
    Credentials root{0, 0};
    return file_open(nullptr, "/dev/console", abi::O_RDWR, 0, root);
}

File* file_ref(File* f) {
    __atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED);
    return f;
}

void file_unref(File* f) {
    if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    vnode_unref(f->vnode);
    kfree(f);
}

bool file_readable(const File* f) { return (f->flags & abi::O_ACCMODE) != abi::O_WRONLY; }
bool file_writable(const File* f) { return (f->flags & abi::O_ACCMODE) != abi::O_RDONLY; }

Result<usize> file_read(File* f, void* buf, usize n) {
    if (!file_readable(f)) return Error::BadFd;
    Vnode* v = f->vnode;
    if (v->type == VType::Dir) return Error::IsDir;
    if (v->type == VType::CharDev) return vfs_read(v, 0, buf, n);
    MutexGuard g(f->pos_lock);
    Result<usize> r = vfs_read(v, f->offset, buf, n);
    if (r.ok()) f->offset += r.value();
    return r;
}

Result<usize> file_write(File* f, const void* buf, usize n) {
    if (!file_writable(f)) return Error::BadFd;
    Vnode* v = f->vnode;
    if (v->type == VType::CharDev) return vfs_write(v, 0, buf, n);
    MutexGuard g(f->pos_lock);
    if (f->flags & abi::O_APPEND) f->offset = v->size;
    Result<usize> r = vfs_write(v, f->offset, buf, n);
    if (r.ok()) f->offset += r.value();
    return r;
}

Result<u64> file_seek(File* f, i64 off, u32 whence) {
    Vnode* v = f->vnode;
    if (v->type == VType::CharDev) return Error::Invalid;
    MutexGuard g(f->pos_lock);
    i64 base;
    switch (whence) {
    case abi::SEEK_SET: base = 0; break;
    case abi::SEEK_CUR: base = (i64)f->offset; break;
    case abi::SEEK_END: base = (i64)v->size; break;
    default: return Error::Invalid;
    }
    if (v->type == VType::Dir && !(whence == abi::SEEK_SET && off == 0)) return Error::Invalid;   // rewind only
    i64 pos = base + off;
    if ((off > 0 && pos < base) || pos < 0) return Error::Invalid;
    f->offset = (u64)pos;
    return (u64)pos;
}

Result<bool> file_readdir(File* f, abi::Dirent* out) {
    Vnode* v = f->vnode;
    if (v->type != VType::Dir) return Error::NotDir;
    MutexGuard g(f->pos_lock);
    DirEntry e;
    u64 cookie = f->offset;
    Result<bool> r = vfs_readdir(v, &cookie, &e);
    if (!r.ok() || !r.value()) return r;
    f->offset = cookie;
    memset(out, 0, sizeof *out);
    out->ino = e.ino;
    out->type = dirent_type(e.type);
    strlcpy(out->name, e.name, sizeof out->name);
    return true;
}

void file_stat(const Vnode* v, abi::Stat* out) {
    memset(out, 0, sizeof *out);
    u32 type = 0;
    switch (v->type) {
    case VType::File: type = abi::S_IFREG; break;
    case VType::Dir: type = abi::S_IFDIR; break;
    case VType::Symlink: type = abi::S_IFLNK; break;
    case VType::CharDev: type = abi::S_IFCHR; break;
    case VType::BlockDev: type = abi::S_IFBLK; break;
    }
    out->ino = v->ino;
    out->size = v->size;
    out->blocks = (v->size + 511) / 512;
    out->atime = v->atime;
    out->mtime = v->mtime;
    out->ctime = v->ctime;
    out->mode = type | (v->mode & 07777);
    out->nlink = v->nlink;
    out->uid = v->uid;
    out->gid = v->gid;
    out->rdev = v->rdev;
    out->blksize = PAGE_SIZE;
}
