// See vfs.h.
#include <drivers/rtc.h>
#include <fs/vfs.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <proc/process.h>
#include <sched/sched.h>
#include <sched/sync.h>

namespace {

// ------------------------------------------------------------------ lock --
// Recursive, so a file system that calls back into the VFS (vnode_unref
// inside an operation) does not deadlock against itself.
Mutex g_mutex;
Thread* g_owner = nullptr;
u32 g_depth = 0;

Vnode* g_root = nullptr;
Mount* g_mounts = nullptr;

struct FsType {
    const char* name;
    MountFn fn;
};
FsType g_types[8];
u32 g_type_count = 0;

struct Locked {
    Locked() { vfs_lock(); }
    ~Locked() { vfs_unlock(); }
};

void unref_locked(Vnode* v) {
    ASSERT_ALWAYS(v->refs > 0);
    v->refs--;
    if (v->mount) v->mount->refs_dec();
    if (v->refs == 0 && v->ops->release) v->ops->release(v);
}

// ------------------------------------------------------------ name cache --
// Recently resolved (directory, name) -> vnode pairs, so walking a path does
// not search the same directories again. Each entry holds a reference on
// both vnodes; entries are dropped oldest first, and whenever the name could
// have changed (unlink, rename, unmount). Only positive results and names
// of up to DNAME_MAX bytes are kept.
constexpr u32 DCACHE_ENTRIES = 512, DCACHE_BUCKETS = 256, DNAME_MAX = 31;

struct DEntry {
    Vnode* parent;
    Vnode* child;
    DEntry* hash_next;
    DEntry* lru_prev;
    DEntry* lru_next;
    u8 len;
    char name[DNAME_MAX];
};

DEntry g_dentries[DCACHE_ENTRIES];
DEntry* g_dhash[DCACHE_BUCKETS];
DEntry* g_dfree = nullptr;
DEntry* g_dlru_head = nullptr;      // oldest
DEntry* g_dlru_tail = nullptr;
bool g_dinit = false;
u64 g_dhits = 0, g_dmisses = 0;

u32 dhash(const Vnode* parent, const char* name, usize len) {
    u64 h = (u64)parent * 0x9E3779B97F4A7C15ull;
    for (usize i = 0; i < len; i++) h = (h ^ (u8)name[i]) * 0x100000001B3ull;
    return (u32)(h >> 32) % DCACHE_BUCKETS;
}

void dlru_unlink(DEntry* e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else g_dlru_head = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else g_dlru_tail = e->lru_prev;
    e->lru_prev = e->lru_next = nullptr;
}

void dlru_append(DEntry* e) {
    e->lru_prev = g_dlru_tail;
    e->lru_next = nullptr;
    if (g_dlru_tail) g_dlru_tail->lru_next = e;
    else g_dlru_head = e;
    g_dlru_tail = e;
}

void unref_locked(Vnode* v);

void dentry_drop(DEntry* e) {
    DEntry** link = &g_dhash[dhash(e->parent, e->name, e->len)];
    while (*link && *link != e) link = &(*link)->hash_next;
    if (*link) *link = e->hash_next;
    dlru_unlink(e);
    Vnode* parent = e->parent;
    Vnode* child = e->child;
    e->parent = e->child = nullptr;
    e->hash_next = g_dfree;
    g_dfree = e;
    unref_locked(child);
    unref_locked(parent);
}

DEntry* dcache_find(Vnode* parent, const char* name, usize len) {
    if (len > DNAME_MAX) return nullptr;
    for (DEntry* e = g_dhash[dhash(parent, name, len)]; e; e = e->hash_next)
        if (e->parent == parent && e->len == len && memcmp(e->name, name, len) == 0) return e;
    return nullptr;
}

Vnode* dcache_lookup(Vnode* parent, const char* name, usize len) {
    DEntry* e = dcache_find(parent, name, len);
    if (!e) {
        g_dmisses++;
        return nullptr;
    }
    g_dhits++;
    dlru_unlink(e);
    dlru_append(e);
    return vnode_ref(e->child);
}

void dcache_insert(Vnode* parent, const char* name, usize len, Vnode* child) {
    if (len > DNAME_MAX || dcache_find(parent, name, len)) return;
    if (!g_dinit) {
        for (DEntry& e : g_dentries) {
            e.hash_next = g_dfree;
            g_dfree = &e;
        }
        g_dinit = true;
    }
    if (!g_dfree) dentry_drop(g_dlru_head);
    DEntry* e = g_dfree;
    g_dfree = e->hash_next;
    e->parent = vnode_ref(parent);
    e->child = vnode_ref(child);
    e->len = (u8)len;
    memcpy(e->name, name, len);
    u32 b = dhash(parent, name, len);
    e->hash_next = g_dhash[b];
    g_dhash[b] = e;
    dlru_append(e);
}

void dcache_forget(Vnode* parent, const char* name, usize len) {
    DEntry* e = dcache_find(parent, name, len);
    if (e) dentry_drop(e);
}

// Drops every entry that involves file system `m`.
void dcache_forget_mount(Mount* m) {
    DEntry* e = g_dlru_head;
    while (e) {
        DEntry* next = e->lru_next;
        if (e->parent->mount == m || e->child->mount == m) dentry_drop(e);
        e = next;
    }
}

bool is_dot(const char* s, usize n) { return n == 1 && s[0] == '.'; }
bool is_dotdot(const char* s, usize n) { return n == 2 && s[0] == '.' && s[1] == '.'; }

// The parent of a directory, crossing mount points upward. New reference.
Result<Vnode*> parent_of(Vnode* dir) {
    if (dir == g_root) return vnode_ref(dir);
    Vnode* cur = dir;
    while (cur->mount && cur == cur->mount->root && cur->mount->covered) cur = cur->mount->covered;
    if (cur == g_root) return vnode_ref(cur);
    if (!cur->ops->lookup) return Error::NotSupported;
    return cur->ops->lookup(cur, "..", 2);
}

// Enters any file system mounted on `v` (consumes the reference).
Vnode* cross_down(Vnode* v) {
    while (v->mounted_here) {
        Vnode* root = vnode_ref(v->mounted_here->root);
        unref_locked(v);
        v = root;
    }
    return v;
}

Result<Vnode*> resolve_locked(Vnode* start, const char* path, const Credentials& cred, LookupFlags flags,
                              char* last) {
    usize plen = strlen(path);
    if (plen == 0) return Error::NotFound;
    if (plen >= PATH_MAX) return Error::NameTooLong;
    char work[PATH_MAX];
    memcpy(work, path, plen + 1);
    Vnode* cur = vnode_ref(work[0] == '/' || !start ? g_root : start);
    usize pos = 0;
    u32 links = 0;
    for (;;) {
        while (work[pos] == '/') pos++;
        if (!work[pos]) {
            if (flags.want_parent) {        // "/" or a path of only slashes
                unref_locked(cur);
                return Error::Invalid;
            }
            return cur;
        }
        usize begin = pos;
        while (work[pos] && work[pos] != '/') pos++;
        usize len = pos - begin;
        const char* name = work + begin;
        usize after = pos;
        while (work[after] == '/') after++;
        bool is_last = !work[after];
        if (len > vfs::NAME_MAX) {
            unref_locked(cur);
            return Error::NameTooLong;
        }
        if (cur->type != VType::Dir) {
            unref_locked(cur);
            return Error::NotDir;
        }
        Result<void> search = vfs_access(cur, cred, vfs::X_OK);
        if (!search.ok()) {
            unref_locked(cur);
            return search.error();
        }
        if (is_last && flags.want_parent) {
            if (is_dot(name, len) || is_dotdot(name, len)) {
                unref_locked(cur);
                return Error::Invalid;
            }
            memcpy(last, name, len);
            last[len] = 0;
            return cur;
        }
        if (is_dot(name, len)) continue;
        Result<Vnode*> next(Error::NotFound);
        if (is_dotdot(name, len)) {
            next = parent_of(cur);
        } else if (Vnode* cached = dcache_lookup(cur, name, len)) {
            next = cached;
        } else {
            next = cur->ops->lookup ? cur->ops->lookup(cur, name, len) : Result<Vnode*>(Error::NotSupported);
            if (next.ok()) dcache_insert(cur, name, len, next.value());
        }
        if (!next.ok()) {
            unref_locked(cur);
            return next.error();
        }
        Vnode* n = cross_down(next.value());
        if (n->type == VType::Symlink && (!is_last || flags.follow_last)) {
            if (++links > vfs::SYMLINK_MAX_DEPTH) {
                unref_locked(n);
                unref_locked(cur);
                return Error::Loop;
            }
            char target[PATH_MAX];
            Result<usize> tl = n->ops->readlink ? n->ops->readlink(n, target, sizeof target - 1)
                                                : Result<usize>(Error::NotSupported);
            unref_locked(n);
            if (!tl.ok() || tl.value() == 0) {
                unref_locked(cur);
                return tl.ok() ? Error::NotFound : tl.error();
            }
            target[tl.value()] = 0;
            // New path = target + "/" + the rest.
            usize rest = strlen(work + pos);
            if (tl.value() + rest + 1 >= PATH_MAX) {
                unref_locked(cur);
                return Error::NameTooLong;
            }
            char joined[PATH_MAX];
            memcpy(joined, target, tl.value());
            memcpy(joined + tl.value(), work + pos, rest + 1);
            memcpy(work, joined, tl.value() + rest + 1);
            pos = 0;
            if (work[0] == '/') {
                unref_locked(cur);
                cur = vnode_ref(g_root);
            }
            continue;
        }
        unref_locked(cur);
        cur = n;
    }
}

// In a directory with the sticky bit (01000, as on /tmp) a name may be
// removed or renamed only by the file's owner, the directory's owner, or root.
bool sticky_allows(const Vnode* dir, const Vnode* target, const Credentials& cred) {
    if (!(dir->mode & 01000) || cred.uid == 0) return true;
    return cred.uid == target->uid || cred.uid == dir->uid;
}

void add_type(const char* name, MountFn fn) {
    if (g_type_count < sizeof g_types / sizeof g_types[0]) g_types[g_type_count++] = {name, fn};
}

} // namespace

// ------------------------------------------------------------- lifetime --
void vfs_lock() {
    Thread* me = thread_current();
    if (g_owner == me) {
        g_depth++;
        return;
    }
    g_mutex.lock();
    g_owner = me;
    g_depth = 1;
}

void vfs_unlock() {
    ASSERT_ALWAYS(g_owner == thread_current() && g_depth > 0);
    if (--g_depth == 0) {
        g_owner = nullptr;
        g_mutex.unlock();
    }
}

void vfs_init(Vnode* root, Mount* root_mount) {
    g_root = root;
    root_mount->next = nullptr;
    strlcpy(root_mount->path, "/", sizeof root_mount->path);
    g_mounts = root_mount;
}

Vnode* vfs_root() { return g_root; }
Mount* vfs_mounts() { return g_mounts; }

Vnode* vnode_ref(Vnode* v) {
    __atomic_add_fetch(&v->refs, 1, __ATOMIC_RELAXED);
    if (v->mount) v->mount->refs_inc();
    return v;
}

void vnode_unref(Vnode* v) {
    Locked l;
    unref_locked(v);
}

Result<Vnode*> vnode_alloc(const VnodeOps* ops, Mount* m, VType type) {
    Vnode* v = (Vnode*)kzalloc(sizeof(Vnode));
    if (!v) return Error::NoMemory;
    v->ops = ops;
    v->mount = m;
    v->type = type;
    v->refs = 1;
    v->nlink = 1;
    if (m) m->refs_inc();
    return v;
}

u64 vfs_now() { return datetime_to_unix(rtc_now()); }

// ---------------------------------------------------------------- names --
Result<Vnode*> vfs_resolve(Vnode* start, const char* path, const Credentials& cred, LookupFlags flags, char* last) {
    Locked l;
    return resolve_locked(start, path, cred, flags, last);
}

Result<void> vfs_access(Vnode* v, const Credentials& cred, u32 want) {
    bool fs_object = v->type == VType::File || v->type == VType::Dir || v->type == VType::Symlink;
    if ((want & vfs::W_OK) && fs_object && v->mount && (v->mount->flags & vfs::MNT_RDONLY)) return Error::ReadOnly;
    if ((want & vfs::X_OK) && v->type == VType::File && v->mount && (v->mount->flags & vfs::MNT_NOEXEC))
        return Error::Access;
    if (cred.uid == 0) {
        // Root reads and writes anything, and executes anything that has
        // at least one execute bit (directories are always searchable).
        if ((want & vfs::X_OK) && v->type != VType::Dir && !(v->mode & 0111)) return Error::Access;
        return {};
    }
    u32 bits = cred.uid == v->uid ? (v->mode >> 6) & 7 : cred.gid == v->gid ? (v->mode >> 3) & 7 : v->mode & 7;
    return (bits & want) == want ? Result<void>() : Result<void>(Error::Access);
}

// ----------------------------------------------------------- operations --
Result<Vnode*> vfs_create(Vnode* start, const char* path, const Credentials& cred, VType type, u32 mode,
                          bool exclusive, const char* symlink_target, u32 rdev, bool* created) {
    Locked l;
    if (created) *created = false;
    char name[vfs::NAME_MAX + 1];
    LookupFlags f;
    f.want_parent = true;
    Result<Vnode*> pr = resolve_locked(start, path, cred, f, name);
    if (!pr.ok()) return pr.error();
    Vnode* dir = pr.value();
    usize len = strlen(name);
    Result<Vnode*> existing = dir->ops->lookup ? dir->ops->lookup(dir, name, len) : Result<Vnode*>(Error::NotFound);
    if (existing.ok()) {
        Vnode* e = cross_down(existing.value());
        unref_locked(dir);
        if (exclusive) {
            unref_locked(e);
            return Error::Exists;
        }
        return e;
    }
    Result<void> ok = existing.error() == Error::NotFound ? vfs_access(dir, cred, vfs::W_OK | vfs::X_OK)
                                                          : Result<void>(existing.error());
    if (ok.ok() && (type == VType::CharDev || type == VType::BlockDev) && cred.uid != 0) ok = Error::Perm;
    if (ok.ok() && !dir->ops->create) ok = Error::NotSupported;
    if (!ok.ok()) {
        unref_locked(dir);
        return ok.error();
    }
    Result<Vnode*> made = dir->ops->create(dir, name, len, type, mode & 07777, cred.uid, cred.gid, symlink_target, rdev);
    unref_locked(dir);
    if (made.ok() && created) *created = true;
    return made;
}

Result<void> vfs_unlink(Vnode* start, const char* path, const Credentials& cred, bool dir_wanted) {
    Locked l;
    char name[vfs::NAME_MAX + 1];
    LookupFlags f;
    f.want_parent = true;
    Result<Vnode*> pr = resolve_locked(start, path, cred, f, name);
    if (!pr.ok()) return pr.error();
    Vnode* dir = pr.value();
    usize len = strlen(name);
    Result<void> r = vfs_access(dir, cred, vfs::W_OK | vfs::X_OK);
    if (r.ok()) {
        // A directory something is mounted on cannot go away.
        Result<Vnode*> t = dir->ops->lookup ? dir->ops->lookup(dir, name, len) : Result<Vnode*>(Error::NotFound);
        if (!t.ok()) r = t.error();
        else {
            if (t.value()->mounted_here) r = Error::Busy;
            else if (!sticky_allows(dir, t.value(), cred)) r = Error::Perm;
            unref_locked(t.value());
        }
    }
    if (r.ok()) {
        dcache_forget(dir, name, len);
        r = dir->ops->unlink ? dir->ops->unlink(dir, name, len, dir_wanted) : Result<void>(Error::NotSupported);
    }
    unref_locked(dir);
    return r;
}

Result<void> vfs_rename(Vnode* start, const char* from, const char* to, const Credentials& cred) {
    Locked l;
    char fname[vfs::NAME_MAX + 1], tname[vfs::NAME_MAX + 1];
    LookupFlags f;
    f.want_parent = true;
    Result<Vnode*> fr = resolve_locked(start, from, cred, f, fname);
    if (!fr.ok()) return fr.error();
    Result<Vnode*> tr = resolve_locked(start, to, cred, f, tname);
    if (!tr.ok()) {
        unref_locked(fr.value());
        return tr.error();
    }
    Vnode* fd = fr.value();
    Vnode* td = tr.value();
    Result<void> r;
    if (fd->mount != td->mount) r = Error::CrossDevice;
    if (r.ok()) r = vfs_access(fd, cred, vfs::W_OK | vfs::X_OK);
    if (r.ok()) r = vfs_access(td, cred, vfs::W_OK | vfs::X_OK);
    Vnode* src = nullptr;
    if (r.ok()) {
        Result<Vnode*> s = fd->ops->lookup ? fd->ops->lookup(fd, fname, strlen(fname)) : Result<Vnode*>(Error::NotFound);
        if (!s.ok()) r = s.error();
        else src = s.value();
    }
    // A directory may not move into itself or below itself.
    if (r.ok() && src->type == VType::Dir) {
        Vnode* walk = vnode_ref(td);
        for (u32 depth = 0; depth < 4096; depth++) {
            if (walk == src) {
                r = Error::Invalid;
                break;
            }
            if (walk == g_root || walk == walk->mount->root) break;
            Result<Vnode*> up = parent_of(walk);
            unref_locked(walk);
            if (!up.ok()) {
                walk = nullptr;
                r = up.error();
                break;
            }
            walk = up.value();
        }
        if (walk) unref_locked(walk);
    }
    if (r.ok() && src->mounted_here) r = Error::Busy;
    if (r.ok() && !sticky_allows(fd, src, cred)) r = Error::Perm;
    if (r.ok()) {
        // Replacing someone else's file in a sticky directory is deleting it.
        Result<Vnode*> old = td->ops->lookup ? td->ops->lookup(td, tname, strlen(tname)) : Result<Vnode*>(Error::NotFound);
        if (old.ok()) {
            if (!sticky_allows(td, old.value(), cred)) r = Error::Perm;
            unref_locked(old.value());
        }
    }
    if (src) unref_locked(src);
    if (r.ok()) {
        dcache_forget(fd, fname, strlen(fname));
        dcache_forget(td, tname, strlen(tname));
        r = fd->ops->rename ? fd->ops->rename(fd, fname, strlen(fname), td, tname, strlen(tname))
                            : Result<void>(Error::NotSupported);
    }
    unref_locked(fd);
    unref_locked(td);
    return r;
}

Result<usize> vfs_read(Vnode* v, u64 offset, void* buf, usize n) {
    if (v->type == VType::Dir) return Error::IsDir;
    if (!v->ops->read) return Error::NotSupported;
    if (v->type == VType::CharDev) return v->ops->read(v, offset, buf, n);
    Locked l;
    return v->ops->read(v, offset, buf, n);
}

Result<usize> vfs_write(Vnode* v, u64 offset, const void* buf, usize n) {
    if (v->type == VType::Dir) return Error::IsDir;
    if (!v->ops->write) return Error::NotSupported;
    if (v->type == VType::CharDev) return v->ops->write(v, offset, buf, n);
    if (v->type == VType::File && v->mount && (v->mount->flags & vfs::MNT_RDONLY)) return Error::ReadOnly;
    Locked l;
    return v->ops->write(v, offset, buf, n);
}

Result<void> vfs_truncate(Vnode* v, u64 size) {
    if (v->type == VType::Dir) return Error::IsDir;
    if (v->type != VType::File) return Error::Invalid;
    if (v->mount && (v->mount->flags & vfs::MNT_RDONLY)) return Error::ReadOnly;
    if (!v->ops->truncate) return Error::NotSupported;
    Locked l;
    return v->ops->truncate(v, size);
}

Result<bool> vfs_readdir(Vnode* dir, u64* cookie, DirEntry* out) {
    if (dir->type != VType::Dir) return Error::NotDir;
    Locked l;
    if (*cookie == 0 || *cookie == 1) {
        bool dotdot = *cookie == 1;
        out->type = VType::Dir;
        out->ino = dir->ino;
        if (dotdot) {
            Result<Vnode*> p = parent_of(dir);
            if (p.ok()) {
                out->ino = p.value()->ino;
                unref_locked(p.value());
            }
        }
        strlcpy(out->name, dotdot ? ".." : ".", sizeof out->name);
        (*cookie)++;
        return true;
    }
    if (!dir->ops->readdir) return false;
    u64 fs_cookie = *cookie - 2;
    Result<bool> r = dir->ops->readdir(dir, &fs_cookie, out);
    if (r.ok() && r.value()) *cookie = fs_cookie + 2;
    return r;
}

Result<usize> vfs_readlink(Vnode* v, char* buf, usize n) {
    if (v->type != VType::Symlink) return Error::Invalid;
    if (!v->ops->readlink) return Error::NotSupported;
    Locked l;
    return v->ops->readlink(v, buf, n);
}

Result<void> vfs_chmod(Vnode* v, const Credentials& cred, u32 mode) {
    Locked l;
    if (cred.uid != 0 && cred.uid != v->uid) return Error::Perm;
    if (v->mount && (v->mount->flags & vfs::MNT_RDONLY)) return Error::ReadOnly;
    v->mode = mode & 07777;
    v->ctime = vfs_now();
    return v->ops->setattr ? v->ops->setattr(v) : Result<void>();
}

Result<void> vfs_chown(Vnode* v, const Credentials& cred, u32 uid, u32 gid) {
    Locked l;
    if (cred.uid != 0) return Error::Perm;
    if (v->mount && (v->mount->flags & vfs::MNT_RDONLY)) return Error::ReadOnly;
    v->uid = uid;
    v->gid = gid;
    v->ctime = vfs_now();
    return v->ops->setattr ? v->ops->setattr(v) : Result<void>();
}

Result<void> vfs_fsync(Vnode* v) {
    if (!v->ops->fsync) return {};
    Locked l;
    return v->ops->fsync(v);
}

Result<i64> vfs_ioctl(Vnode* v, u32 request, u64 arg) {
    if (!v->ops->ioctl) return Error::NotSupported;
    if (v->type == VType::CharDev || v->type == VType::BlockDev) return v->ops->ioctl(v, request, arg);
    Locked l;
    return v->ops->ioctl(v, request, arg);
}

void vfs_sync() {
    Locked l;
    for (Mount* m = g_mounts; m; m = m->next)
        if (m->sync) {
            Result<void> r = m->sync(m);
            if (!r.ok()) kprintf("vfs: sync of %s failed: %s\n", m->path, error_name(r.error()));
        }
}

Result<u8*> vfs_read_all(Vnode* v, usize max, usize* size) {
    if (v->type == VType::Dir) return Error::IsDir;
    if (v->type != VType::File) return Error::Invalid;
    Locked l;
    if (v->size > max) return Error::TooBig;
    usize n = (usize)v->size;
    u8* buf = (u8*)kmalloc(n ? n : 1);
    if (!buf) return Error::NoMemory;
    usize done = 0;
    while (done < n) {
        Result<usize> r = v->ops->read(v, done, buf + done, n - done);
        if (!r.ok() || r.value() == 0) {
            kfree(buf);
            return r.ok() ? Error::IO : r.error();
        }
        done += r.value();
    }
    *size = n;
    return buf;
}

// ---------------------------------------------------------------- mounts --
void vfs_register_type(const char* type, MountFn fn) { add_type(type, fn); }

Result<Mount*> vfs_make_mount(const char* type, const char* source, u32 flags) {
    for (u32 i = 0; i < g_type_count; i++)
        if (strcmp(g_types[i].name, type) == 0) return g_types[i].fn(source, flags);
    return Error::NoDevice;
}

Result<void> vfs_mount(Mount* m, const char* path, const Credentials& cred) {
    if (cred.uid != 0) return Error::Perm;
    Locked l;
    Result<Vnode*> r = resolve_locked(nullptr, path, cred, LookupFlags{}, nullptr);
    if (!r.ok()) return r.error();
    Vnode* dir = r.value();
    if (dir->type != VType::Dir) {
        unref_locked(dir);
        return Error::NotDir;
    }
    if (dir->mounted_here) {
        unref_locked(dir);
        return Error::Busy;
    }
    Result<void> p = vfs_path_of(dir, m->path, sizeof m->path);
    if (!p.ok()) strlcpy(m->path, path, sizeof m->path);
    m->covered = dir;               // keeps the reference
    dir->mounted_here = m;
    m->next = nullptr;
    Mount** tail = &g_mounts;
    while (*tail) tail = &(*tail)->next;
    *tail = m;
    return {};
}

Result<void> vfs_unmount(const char* path, const Credentials& cred) {
    if (cred.uid != 0) return Error::Perm;
    Locked l;
    Result<Vnode*> r = resolve_locked(nullptr, path, cred, LookupFlags{}, nullptr);
    if (!r.ok()) return r.error();
    Vnode* v = r.value();
    Mount* m = v->mount;
    bool is_root_of_mount = m && v == m->root && m->covered;
    unref_locked(v);
    if (!is_root_of_mount) return Error::Invalid;
    dcache_forget_mount(m);
    // In use: anything but the mount's own reference to its root, or a file
    // system mounted somewhere inside it.
    if (m->refs != 1) return Error::Busy;
    for (Mount* o = g_mounts; o; o = o->next)
        if (o->covered && o->covered->mount == m) return Error::Busy;
    // A file system that cannot write (a failing or damaged disk) is still
    // detached; what it could not write is lost, and it says so.
    if (m->sync) {
        Result<void> s = m->sync(m);
        if (!s.ok()) kprintf("vfs: %s: could not write everything before unmounting: %s\n", m->path, error_name(s.error()));
    }
    for (Mount** link = &g_mounts; *link; link = &(*link)->next)
        if (*link == m) {
            *link = m->next;
            break;
        }
    Vnode* covered = m->covered;
    covered->mounted_here = nullptr;
    unref_locked(covered);
    unref_locked(m->root);
    return m->unmount ? m->unmount(m) : Result<void>();
}

Result<void> vfs_path_of(Vnode* dir, char* buf, usize n) {
    Locked l;
    // Built backwards from the end of `tmp`: each step finds the current
    // directory's name in its parent.
    char tmp[PATH_MAX];
    usize at = sizeof tmp - 1;
    tmp[at] = 0;
    Vnode* cur = vnode_ref(dir);
    for (u32 depth = 0; cur != g_root; depth++) {
        // Name of `cur` in its parent: the covered directory's name when
        // `cur` is the root of a mount.
        Vnode* named = cur;
        while (named->mount && named == named->mount->root && named->mount->covered) named = named->mount->covered;
        if (named == g_root) break;
        Result<Vnode*> up = depth < 128 ? parent_of(cur) : Result<Vnode*>(Error::NameTooLong);
        if (!up.ok()) {
            unref_locked(cur);
            return up.error();
        }
        Vnode* parent = up.value();
        bool found = false;
        u64 cookie = 0;
        DirEntry e;
        for (;;) {
            Result<bool> more = parent->ops->readdir ? parent->ops->readdir(parent, &cookie, &e) : Result<bool>(false);
            if (!more.ok() || !more.value()) break;
            if (e.ino == named->ino) {
                found = true;
                break;
            }
        }
        unref_locked(cur);
        cur = parent;
        usize len = found ? strlen(e.name) : 0;
        if (!found || len + 1 > at) {
            unref_locked(cur);
            return found ? Error::NameTooLong : Error::NotFound;
        }
        at -= len;
        memcpy(tmp + at, e.name, len);
        tmp[--at] = '/';
    }
    unref_locked(cur);
    if (at == sizeof tmp - 1) tmp[--at] = '/';
    usize len = sizeof tmp - 1 - at;
    if (len + 1 > n) return Error::NameTooLong;
    memcpy(buf, tmp + at, len + 1);
    return {};
}

VfsCacheStats vfs_cache_stats() {
    Locked l;
    u32 used = 0;
    for (DEntry* e = g_dlru_head; e; e = e->lru_next) used++;
    return {used, DCACHE_ENTRIES, g_dhits, g_dmisses};
}
