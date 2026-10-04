// The virtual file system (SPEC phase 9): one tree of names over several
// file systems. A Vnode is a file, directory, symbolic link or device node;
// each file system supplies the operations behind its vnodes (VnodeOps) and
// guarantees one Vnode per object, so vnodes can be compared by pointer.
//
// Locking: one sleeping lock (the VFS lock) serialises every operation on
// the tree and on file systems' metadata and data. Device reads and writes
// (devfs character devices) run without it, because a read from the console
// may wait for a long time. Simple and correct first; finer locking when
// measurements ask for it (docs/DECISIONS.md, phase 9).
//
// Permissions are checked here, not in the file systems: owner/group/other
// mode bits against the caller's credentials, with uid 0 allowed to read
// and write anything and to execute anything with at least one x bit.
// Mount flags (ro, noexec, nosuid, nodev) are enforced here too.
//
// Paths: absolute or relative to a directory vnode (the process's working
// directory, or a directory file descriptor for the *at calls). "." and ".."
// work, ".." at the root stays at the root, symbolic links are followed up
// to 40 deep (then Error::Loop), names are at most 255 bytes and paths at
// most PATH_MAX - 1.
#pragma once

#include <lib/result.h>
#include <lib/types.h>

struct Credentials;

namespace vfs {
constexpr usize NAME_MAX = 255;
constexpr u32 SYMLINK_MAX_DEPTH = 40;

// Mount flags.
constexpr u32 MNT_RDONLY = 1 << 0;
constexpr u32 MNT_NOEXEC = 1 << 1;
constexpr u32 MNT_NOSUID = 1 << 2;
constexpr u32 MNT_NODEV = 1 << 3;

// Access wanted, for vfs_access.
constexpr u32 R_OK = 4, W_OK = 2, X_OK = 1;
} // namespace vfs

// Object: a kernel object behind a descriptor (ports, shared memory, event
// queues; ipc/object.h). It has no name in the tree.
enum class VType : u8 { File, Dir, Symlink, CharDev, BlockDev, Object };

struct Vnode;
struct Mount;

struct DirEntry {
    u64 ino;
    VType type;
    char name[vfs::NAME_MAX + 1];
};

// What a file system implements. Pointers may be null where an operation
// makes no sense (the VFS then answers NotSupported). All are called with the
// VFS lock held, except read/write/ioctl on character devices.
struct VnodeOps {
    // The child `name` (len bytes, never "." or empty; ".." only for
    // directories that are not the root of their file system). Returns a
    // new reference. NotFound if absent.
    Result<Vnode*> (*lookup)(Vnode* dir, const char* name, usize len);
    // Creates a file, directory, symlink (target given) or device node
    // (rdev given). Returns a new reference. Exists if the name is taken.
    Result<Vnode*> (*create)(Vnode* dir, const char* name, usize len, VType type, u32 mode, u32 uid, u32 gid,
                             const char* symlink_target, u32 rdev);
    // Removes a name. dir_wanted: rmdir (NotDir if not a directory,
    // NotEmpty if it has entries); otherwise IsDir for directories.
    Result<void> (*unlink)(Vnode* dir, const char* name, usize len, bool dir_wanted);
    // Moves a name within the same file system, replacing a target of the
    // same kind (an empty directory, or a non-directory).
    Result<void> (*rename)(Vnode* from_dir, const char* from, usize from_len, Vnode* to_dir, const char* to,
                           usize to_len);
    Result<usize> (*read)(Vnode* v, u64 offset, void* buf, usize n);
    Result<usize> (*write)(Vnode* v, u64 offset, const void* buf, usize n);
    Result<void> (*truncate)(Vnode* v, u64 size);
    // Next entry after *cookie (0 = start); false at the end. "." and ".."
    // are listed by the VFS itself.
    Result<bool> (*readdir)(Vnode* dir, u64* cookie, DirEntry* out);
    Result<usize> (*readlink)(Vnode* v, char* buf, usize n);
    // Writes changed attributes (mode, uid, gid, times, already set in the
    // vnode) back to the file system.
    Result<void> (*setattr)(Vnode* v);
    Result<void> (*fsync)(Vnode* v);
    Result<i64> (*ioctl)(Vnode* v, u32 request, u64 arg);
    // The last reference went away. The file system may forget the object
    // (and free it if no name refers to it any more).
    void (*release)(Vnode* v);
};

struct Vnode {
    const VnodeOps* ops;
    Mount* mount;               // the file system this vnode belongs to
    Mount* mounted_here;        // a file system mounted on this directory, or null
    VType type;
    u32 mode;                   // permission bits (07777)
    u32 uid, gid;
    u32 nlink;
    u32 rdev;                   // device nodes: major << 16 | minor
    u64 ino;
    u64 size;
    u64 atime, mtime, ctime;    // seconds since 1970-01-01 UTC
    u32 refs;
    void* fs_data;              // the file system's own record
};

// One mounted file system.
struct Mount {
    const char* type;           // "tmpfs", "cerfs", "devfs", "initramfs"
    char source[32];            // device or "none"
    char path[64];              // where it is mounted, for listing
    u32 flags;                  // vfs::MNT_*
    Vnode* root;                // root directory of this file system
    Vnode* covered;             // the directory it is mounted on (null for /)
    void* fs_data;              // the file system instance
    // Called by vfs_unmount after the last vnode is released.
    Result<void> (*unmount)(Mount* m);
    Result<void> (*sync)(Mount* m);
    Mount* next;
    u32 refs;                   // references to this file system's vnodes (busy if more than the root's)

    void refs_inc() { __atomic_add_fetch(&refs, 1, __ATOMIC_RELAXED); }
    void refs_dec() { __atomic_sub_fetch(&refs, 1, __ATOMIC_RELAXED); }
};

// ---- lifetime and references ----
void vfs_init(Vnode* root, Mount* root_mount);
Vnode* vnode_ref(Vnode* v);
void vnode_unref(Vnode* v);
// A zeroed vnode with one reference, for file systems. Error: NoMemory.
Result<Vnode*> vnode_alloc(const VnodeOps* ops, Mount* m, VType type);
// Seconds since the epoch, for timestamps.
u64 vfs_now();

// The VFS lock, for code (file systems' background threads) that needs it.
void vfs_lock();
void vfs_unlock();

// ---- names ----
struct LookupFlags {
    bool follow_last = true;    // follow a symlink in the last component
    bool want_parent = false;   // stop at the parent; return the last name
};
// Resolves `path` relative to `start` (null = the root). Returns a new
// reference. With want_parent, returns the parent directory and copies the
// last component into `last` (NAME_MAX + 1 bytes); a path ending in "/"
// or naming "/" itself gives Invalid.
Result<Vnode*> vfs_resolve(Vnode* start, const char* path, const Credentials& cred, LookupFlags flags,
                           char* last = nullptr);
// May `cred` access `v` as `want` (vfs::R_OK | W_OK | X_OK)? Access if not,
// ReadOnly for writing on a read-only mount.
Result<void> vfs_access(Vnode* v, const Credentials& cred, u32 want);
Vnode* vfs_root();

// ---- operations (each takes the VFS lock) ----
Result<Vnode*> vfs_create(Vnode* start, const char* path, const Credentials& cred, VType type, u32 mode,
                          bool exclusive, const char* symlink_target = nullptr, u32 rdev = 0,
                          bool* created = nullptr);
Result<void> vfs_unlink(Vnode* start, const char* path, const Credentials& cred, bool dir_wanted);
Result<void> vfs_rename(Vnode* start, const char* from, const char* to, const Credentials& cred);
Result<usize> vfs_read(Vnode* v, u64 offset, void* buf, usize n);
Result<usize> vfs_write(Vnode* v, u64 offset, const void* buf, usize n);
Result<void> vfs_truncate(Vnode* v, u64 size);
Result<bool> vfs_readdir(Vnode* dir, u64* cookie, DirEntry* out);
Result<usize> vfs_readlink(Vnode* v, char* buf, usize n);
Result<void> vfs_chmod(Vnode* v, const Credentials& cred, u32 mode);
Result<void> vfs_chown(Vnode* v, const Credentials& cred, u32 uid, u32 gid);
Result<void> vfs_fsync(Vnode* v);
Result<i64> vfs_ioctl(Vnode* v, u32 request, u64 arg);
// Writes every file system's dirty data and metadata to its device.
void vfs_sync();
// Reads a whole regular file into a new kmalloc'd buffer (for the program
// loader). Errors: IsDir, TooBig (over `max`), NoMemory, IO.
Result<u8*> vfs_read_all(Vnode* v, usize max, usize* size);

// Name cache statistics (for `mount` and benchmarks).
struct VfsCacheStats {
    u32 entries, capacity;
    u64 hits, misses;
};
VfsCacheStats vfs_cache_stats();

// ---- mounts ----
// Mounts `m` (filled by the file system) on the directory `path`.
// Errors: NotDir, Busy (already a mount point), Perm (not uid 0).
Result<void> vfs_mount(Mount* m, const char* path, const Credentials& cred);
// Unmounts the file system at `path`. Busy if a vnode in it is in use.
Result<void> vfs_unmount(const char* path, const Credentials& cred);
// For listing (`mount` with no arguments).
Mount* vfs_mounts();
// Builds the absolute path of directory `dir` into buf (for getcwd).
Result<void> vfs_path_of(Vnode* dir, char* buf, usize n);

// The file system types register a mount function so `mount -t <type>`
// can create them by name.
using MountFn = Result<Mount*> (*)(const char* source, u32 flags);
void vfs_register_type(const char* type, MountFn fn);
Result<Mount*> vfs_make_mount(const char* type, const char* source, u32 flags);
