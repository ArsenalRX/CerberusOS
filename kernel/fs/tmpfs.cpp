// See tmpfs.h.
#include <fs/dev.h>
#include <fs/tmpfs.h>
#include <fs/ustar.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <mm/pmm.h>

namespace {

struct TmpFs;
struct TmpEntry;

// One file, directory, symlink or device node. Lives as long as a name
// refers to it or a vnode reference is held, whichever is longer.
struct TmpNode {
    Vnode* v;
    TmpFs* fs;
    TmpNode* parent;            // directories: the containing directory
    TmpEntry* entries;          // directories: names in this directory
    u8** pages;                 // files: page pointers (null = hole)
    u64 page_slots;
    char* link;                 // symlinks: target
};

struct TmpEntry {
    TmpEntry* next;
    TmpNode* node;
    u16 len;
    char name[1];               // len bytes + NUL
};

struct TmpFs {
    Mount mount;
    u64 next_ino;
    u64 pages_used, pages_max;
};

extern const VnodeOps g_tmp_ops;
VnodeOps g_tmp_dev_ops;         // device ops, with tmpfs's release

TmpNode* node_of(Vnode* v) { return (TmpNode*)v->fs_data; }

TmpEntry* find_entry(TmpNode* dir, const char* name, usize len, TmpEntry** prev_out = nullptr) {
    TmpEntry* prev = nullptr;
    for (TmpEntry* e = dir->entries; e; prev = e, e = e->next) {
        if (e->len == len && memcmp(e->name, name, len) == 0) {
            if (prev_out) *prev_out = prev;
            return e;
        }
    }
    return nullptr;
}

TmpEntry* new_entry(const char* name, usize len, TmpNode* node) {
    TmpEntry* e = (TmpEntry*)kmalloc(sizeof(TmpEntry) + len);
    if (!e) return nullptr;
    e->next = nullptr;
    e->node = node;
    e->len = (u16)len;
    memcpy(e->name, name, len);
    e->name[len] = 0;
    return e;
}

void free_node(TmpNode* n) {
    if (n->pages) {
        for (u64 i = 0; i < n->page_slots; i++)
            if (n->pages[i]) {
                kfree(n->pages[i]);
                n->fs->pages_used--;
            }
        kfree(n->pages);
    }
    kfree(n->link);
    kfree(n->v);
    kfree(n);
}

// A name no longer refers to `n`: free it now, or when its last vnode
// reference goes (tmp_release).
// One name fewer: the node goes when its last name and reference are gone.
void drop_name(TmpNode* n) {
    if (n->v->nlink > 1 && n->v->type != VType::Dir) {
        n->v->nlink--;
        n->v->ctime = vfs_now();
        return;
    }
    n->v->nlink = 0;
    if (n->v->refs == 0) free_node(n);
}

Result<void> tmp_link(Vnode* dir, const char* name, usize len, Vnode* existing) {
    TmpNode* d = node_of(dir);
    if (find_entry(d, name, len)) return Error::Exists;
    TmpEntry* e = new_entry(name, len, node_of(existing));
    if (!e) return Error::NoMemory;
    e->next = d->entries;
    d->entries = e;
    existing->nlink++;
    existing->ctime = d->v->mtime = d->v->ctime = vfs_now();
    return {};
}

Result<TmpNode*> make_node(TmpFs* fs, TmpNode* parent, VType type, u32 mode, u32 uid, u32 gid) {
    const VnodeOps* ops = (type == VType::CharDev || type == VType::BlockDev) ? &g_tmp_dev_ops : &g_tmp_ops;
    Result<Vnode*> v = vnode_alloc(ops, &fs->mount, type);
    if (!v.ok()) return v.error();
    TmpNode* n = (TmpNode*)kzalloc(sizeof(TmpNode));
    if (!n) {
        vnode_unref(v.value());
        return Error::NoMemory;
    }
    n->v = v.value();
    n->fs = fs;
    n->parent = parent ? parent : n;
    Vnode* vn = n->v;
    vn->fs_data = n;
    vn->mode = mode & 07777;
    vn->uid = uid;
    vn->gid = gid;
    vn->ino = fs->next_ino++;
    vn->atime = vn->mtime = vn->ctime = vfs_now();
    vn->nlink = type == VType::Dir ? 2 : 1;
    return n;
}

// Adds a new node under `dir`. The caller gets the node's vnode reference.
Result<TmpNode*> add_child(TmpNode* dir, const char* name, usize len, VType type, u32 mode, u32 uid, u32 gid) {
    if (find_entry(dir, name, len)) return Error::Exists;
    Result<TmpNode*> made = make_node(dir->fs, dir, type, mode, uid, gid);
    if (!made.ok()) return made.error();
    TmpEntry* e = new_entry(name, len, made.value());
    if (!e) {
        made.value()->v->refs = 0;
        free_node(made.value());
        dir->fs->mount.refs_dec();
        return Error::NoMemory;
    }
    e->next = dir->entries;
    dir->entries = e;
    dir->v->mtime = dir->v->ctime = vfs_now();
    return made.value();
}

// ------------------------------------------------------- vnode operations --
Result<Vnode*> tmp_lookup(Vnode* dir, const char* name, usize len) {
    TmpNode* d = node_of(dir);
    if (len == 2 && name[0] == '.' && name[1] == '.') return vnode_ref(d->parent->v);
    TmpEntry* e = find_entry(d, name, len);
    if (!e) return Error::NotFound;
    return vnode_ref(e->node->v);
}

Result<Vnode*> tmp_create(Vnode* dir, const char* name, usize len, VType type, u32 mode, u32 uid, u32 gid,
                          const char* target, u32 rdev) {
    TmpNode* d = node_of(dir);
    char* link = nullptr;
    if (type == VType::Symlink) {
        usize tl = target ? strlen(target) : 0;
        if (tl == 0) return Error::Invalid;
        link = (char*)kmalloc(tl + 1);
        if (!link) return Error::NoMemory;
        memcpy(link, target, tl + 1);
    }
    Result<TmpNode*> n = add_child(d, name, len, type, type == VType::Symlink ? 0777 : mode, uid, gid);
    if (!n.ok()) {
        kfree(link);
        return n.error();
    }
    n.value()->link = link;
    if (link) n.value()->v->size = strlen(link);
    n.value()->v->rdev = rdev;
    return n.value()->v;
}

Result<void> tmp_unlink(Vnode* dir, const char* name, usize len, bool dir_wanted) {
    TmpNode* d = node_of(dir);
    TmpEntry* prev;
    TmpEntry* e = find_entry(d, name, len, &prev);
    if (!e) return Error::NotFound;
    TmpNode* n = e->node;
    if (dir_wanted && n->v->type != VType::Dir) return Error::NotDir;
    if (!dir_wanted && n->v->type == VType::Dir) return Error::IsDir;
    if (n->v->type == VType::Dir && n->entries) return Error::NotEmpty;
    if (prev) prev->next = e->next;
    else d->entries = e->next;
    kfree(e);
    d->v->mtime = d->v->ctime = vfs_now();
    drop_name(n);
    return {};
}

Result<void> tmp_rename(Vnode* from_dir, const char* from, usize from_len, Vnode* to_dir, const char* to,
                        usize to_len) {
    TmpNode* fd = node_of(from_dir);
    TmpNode* td = node_of(to_dir);
    TmpEntry* fprev;
    TmpEntry* fe = find_entry(fd, from, from_len, &fprev);
    if (!fe) return Error::NotFound;
    TmpNode* src = fe->node;
    TmpEntry* tprev;
    TmpEntry* te = find_entry(td, to, to_len, &tprev);
    if (te) {
        TmpNode* dst = te->node;
        if (dst == src) return {};
        bool sdir = src->v->type == VType::Dir, ddir = dst->v->type == VType::Dir;
        if (sdir && !ddir) return Error::NotDir;
        if (!sdir && ddir) return Error::IsDir;
        if (ddir && dst->entries) return Error::NotEmpty;
    }
    TmpEntry* moved = new_entry(to, to_len, src);
    if (!moved) return Error::NoMemory;
    if (te) {
        // Re-find: the target's predecessor may be the source entry.
        te = find_entry(td, to, to_len, &tprev);
        if (tprev) tprev->next = te->next;
        else td->entries = te->next;
        drop_name(te->node);
        kfree(te);
        fe = find_entry(fd, from, from_len, &fprev);
    }
    if (fprev) fprev->next = fe->next;
    else fd->entries = fe->next;
    kfree(fe);
    moved->next = td->entries;
    td->entries = moved;
    if (src->v->type == VType::Dir) src->parent = td;
    u64 now = vfs_now();
    fd->v->mtime = fd->v->ctime = td->v->mtime = td->v->ctime = src->v->ctime = now;
    return {};
}

Result<usize> tmp_read(Vnode* v, u64 off, void* buf, usize n) {
    TmpNode* t = node_of(v);
    if (off >= v->size) return (usize)0;
    if (n > v->size - off) n = (usize)(v->size - off);
    usize done = 0;
    while (done < n) {
        u64 at = off + done;
        u64 page = at / PAGE_SIZE;
        usize in = (usize)(at % PAGE_SIZE);
        usize take = PAGE_SIZE - in < n - done ? PAGE_SIZE - in : n - done;
        if (page < t->page_slots && t->pages[page]) memcpy((u8*)buf + done, t->pages[page] + in, take);
        else memset((u8*)buf + done, 0, take);
        done += take;
    }
    v->atime = vfs_now();
    return done;
}

Result<void> grow_slots(TmpNode* t, u64 pages) {
    if (pages <= t->page_slots) return {};
    u64 slots = t->page_slots ? t->page_slots : 4;
    while (slots < pages) slots *= 2;
    u8** p = (u8**)krealloc(t->pages, slots * sizeof(u8*));
    if (!p) return Error::NoMemory;
    memset(p + t->page_slots, 0, (slots - t->page_slots) * sizeof(u8*));
    t->pages = p;
    t->page_slots = slots;
    return {};
}

Result<usize> tmp_write(Vnode* v, u64 off, const void* buf, usize n) {
    TmpNode* t = node_of(v);
    if (n == 0) return (usize)0;
    if (off + n < off) return Error::TooBig;
    Result<void> g = grow_slots(t, (off + n + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!g.ok()) return g.error();
    usize done = 0;
    while (done < n) {
        u64 at = off + done;
        u64 page = at / PAGE_SIZE;
        usize in = (usize)(at % PAGE_SIZE);
        usize take = PAGE_SIZE - in < n - done ? PAGE_SIZE - in : n - done;
        if (!t->pages[page]) {
            if (t->fs->pages_used >= t->fs->pages_max) break;
            t->pages[page] = (u8*)kzalloc(PAGE_SIZE);
            if (!t->pages[page]) break;
            t->fs->pages_used++;
        }
        memcpy(t->pages[page] + in, (const u8*)buf + done, take);
        done += take;
    }
    if (done == 0) return Error::NoSpace;
    if (off + done > v->size) v->size = off + done;
    v->mtime = v->ctime = vfs_now();
    return done;
}

Result<void> tmp_truncate(Vnode* v, u64 size) {
    TmpNode* t = node_of(v);
    if (size < v->size) {
        u64 keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        for (u64 i = keep; i < t->page_slots; i++)
            if (t->pages[i]) {
                kfree(t->pages[i]);
                t->pages[i] = nullptr;
                t->fs->pages_used--;
            }
        // Bytes after the new end in the last page read as zero if it grows again.
        if (size % PAGE_SIZE && keep - 1 < t->page_slots && t->pages[keep - 1])
            memset(t->pages[keep - 1] + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
    }
    v->size = size;
    v->mtime = v->ctime = vfs_now();
    return {};
}

Result<bool> tmp_readdir(Vnode* dir, u64* cookie, DirEntry* out) {
    TmpNode* d = node_of(dir);
    u64 i = 0;
    for (TmpEntry* e = d->entries; e; e = e->next, i++) {
        if (i < *cookie) continue;
        out->ino = e->node->v->ino;
        out->type = e->node->v->type;
        memcpy(out->name, e->name, (usize)e->len + 1);
        *cookie = i + 1;
        return true;
    }
    return false;
}

Result<usize> tmp_readlink(Vnode* v, char* buf, usize n) {
    TmpNode* t = node_of(v);
    if (!t->link) return Error::Invalid;
    usize len = strlen(t->link);
    if (len > n) len = n;
    memcpy(buf, t->link, len);
    return len;
}

void tmp_release(Vnode* v) {
    TmpNode* t = node_of(v);
    if (v->nlink == 0) free_node(t);
}

const VnodeOps g_tmp_ops = {
    tmp_lookup, tmp_create, tmp_unlink, tmp_rename, tmp_read, tmp_write, tmp_truncate, tmp_readdir, tmp_readlink,
    nullptr,    nullptr,    nullptr,    tmp_release, tmp_link,
};

Result<Mount*> mount_tmpfs(const char*, u32 flags) { return tmpfs_create("tmpfs", flags); }

Result<void> tmp_unmount(Mount* m) {
    // Everything below the root goes with it. Unmount guarantees no vnode
    // of this file system is referenced any more.
    TmpFs* fs = (TmpFs*)m->fs_data;
    struct Walk {
        static void free_tree(TmpNode* n) {
            TmpEntry* e = n->entries;
            while (e) {
                TmpEntry* next = e->next;
                if (e->node->v->type == VType::Dir) free_tree(e->node);
                else free_node(e->node);
                kfree(e);
                e = next;
            }
            n->entries = nullptr;
        }
    };
    TmpNode* root = node_of(m->root);
    Walk::free_tree(root);
    free_node(root);
    kfree(fs);
    return {};
}

// mkdir -p inside the tmpfs, without permission checks (boot only).
Result<TmpNode*> ensure_path(TmpNode* root, const char* path, usize len) {
    TmpNode* cur = root;
    usize i = 0;
    while (i < len) {
        while (i < len && path[i] == '/') i++;
        usize b = i;
        while (i < len && path[i] != '/') i++;
        usize n = i - b;
        if (!n || (n == 1 && path[b] == '.')) continue;
        if (n > vfs::NAME_MAX) return Error::NameTooLong;
        TmpEntry* e = find_entry(cur, path + b, n);
        if (e) {
            if (e->node->v->type != VType::Dir) return Error::NotDir;
            cur = e->node;
            continue;
        }
        Result<TmpNode*> made = add_child(cur, path + b, n, VType::Dir, 0755, 0, 0);
        if (!made.ok()) return made.error();
        made.value()->v->refs = 0;          // the tree keeps it; nobody holds a reference
        cur->fs->mount.refs_dec();
        cur = made.value();
    }
    return cur;
}

struct PopulateCtx {
    TmpFs* fs;
    usize added;
    Error error;
};

bool populate_one(const char* name, const UstarEntry& e, void* arg) {
    PopulateCtx* c = (PopulateCtx*)arg;
    TmpNode* root = node_of(c->fs->mount.root);
    usize len = strlen(name);
    while (len && name[len - 1] == '/') len--;
    if (e.is_dir) {
        Result<TmpNode*> d = ensure_path(root, name, len);
        if (!d.ok()) {
            c->error = d.error();
            return false;
        }
        d.value()->v->mode = e.mode & 07777;
        c->added++;
        return true;
    }
    // Split into directory and file name.
    usize slash = len;
    while (slash && name[slash - 1] != '/') slash--;
    Result<TmpNode*> dir = ensure_path(root, name, slash);
    if (!dir.ok()) {
        c->error = dir.error();
        return false;
    }
    Result<TmpNode*> f = add_child(dir.value(), name + slash, len - slash, VType::File, e.mode & 07777, 0, 0);
    if (!f.ok()) {
        c->error = f.error();
        return f.error() == Error::Exists;  // a duplicate entry: keep the first
    }
    Vnode* v = f.value()->v;
    if (e.size) {
        Result<usize> w = tmp_write(v, 0, e.data, e.size);
        if (!w.ok() || w.value() != e.size) {
            c->error = w.ok() ? Error::NoSpace : w.error();
            vnode_unref(v);
            return false;
        }
    }
    vnode_unref(v);
    c->added++;
    return true;
}

} // namespace

Result<Mount*> tmpfs_create(const char* type, u32 flags) {
    TmpFs* fs = (TmpFs*)kzalloc(sizeof(TmpFs));
    if (!fs) return Error::NoMemory;
    if (!g_tmp_dev_ops.read) {
        g_tmp_dev_ops = *dev_vnode_ops();
        g_tmp_dev_ops.release = tmp_release;
    }
    fs->next_ino = 1;
    fs->pages_max = pmm_stats().usable_frames / 2;
    Mount* m = &fs->mount;
    m->type = type;
    strlcpy(m->source, "none", sizeof m->source);
    m->flags = flags;
    m->fs_data = fs;
    m->unmount = tmp_unmount;
    Result<TmpNode*> root = make_node(fs, nullptr, VType::Dir, 0755, 0, 0);
    if (!root.ok()) {
        kfree(fs);
        return root.error();
    }
    m->root = root.value()->v;          // the mount's reference
    return m;
}

void tmpfs_register() { vfs_register_type("tmpfs", mount_tmpfs); }

Result<usize> tmpfs_populate_tar(Mount* m, const u8* archive, usize size) {
    PopulateCtx c{(TmpFs*)m->fs_data, 0, Error::None};
    vfs_lock();
    ustar_each(archive, size, populate_one, &c);
    vfs_unlock();
    if (c.error != Error::None && c.error != Error::Exists) return c.error;
    return c.added;
}

Result<void> tmpfs_ensure_dir(Mount* m, const char* name) {
    vfs_lock();
    Result<TmpNode*> d = ensure_path(node_of(m->root), name, strlen(name));
    vfs_unlock();
    return d.ok() ? Result<void>() : Result<void>(d.error());
}
