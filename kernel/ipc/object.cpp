// See object.h.
#include <drivers/input.h>
#include <fs/dev.h>
#include <fs/vfs.h>
#include <ipc/object.h>
#include <mm/kheap.h>
#include <sched/sched.h>
#include <sched/sync.h>

namespace {

void object_release(Vnode* v) {
    KObject* obj = (KObject*)v->fs_data;
    if (obj && obj->destroy) obj->destroy(obj);
    kfree(v);
}

// Everything but release is absent: reading, writing and the like are
// refused by the VFS for this vnode type.
const VnodeOps g_object_ops = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    object_release,
};

Spinlock g_fd_lock;               // every process's descriptor table and working directory
WaitQueue g_poll_wq;
u64 g_poll_generation = 0;

} // namespace

Result<File*> object_file(KObject* obj) {
    Result<Vnode*> v = vnode_alloc(&g_object_ops, nullptr, VType::Object);
    if (!v.ok()) return v.error();
    v.value()->mode = 0600;
    File* f = (File*)kzalloc(sizeof(File));
    if (!f) {
        // Free the vnode without running the object's destructor.
        kfree(v.value());
        return Error::NoMemory;
    }
    v.value()->fs_data = obj;
    f->refs = 1;
    f->flags = abi::O_RDWR;
    f->vnode = v.value();
    return f;
}

KObject* file_object(File* f, ObjectKind kind) {
    if (!f || f->vnode->type != VType::Object) return nullptr;
    KObject* obj = (KObject*)f->vnode->fs_data;
    return obj && obj->kind == kind ? obj : nullptr;
}

u32 file_poll(File* f) {
    Vnode* v = f->vnode;
    if (v->type == VType::Object) {
        KObject* obj = (KObject*)v->fs_data;
        return obj && obj->poll ? obj->poll(obj) : 0;
    }
    if (v->type == VType::CharDev && dev::major_of(v->rdev) == dev::INPUT)
        return input_pending(dev::minor_of(v->rdev)) ? poll::READ : 0;
    // Files, directories and the other devices never make a caller wait.
    return poll::READ | poll::WRITE;
}

Result<int> fd_install(File* f, bool cloexec) {
    Process* p = thread_current()->process;
    g_fd_lock.lock();
    for (usize fd = 0; fd < PROCESS_MAX_FDS; fd++) {
        if (p->files[fd]) continue;
        p->files[fd] = f;
        if (cloexec) p->fd_cloexec |= 1u << fd;
        else p->fd_cloexec &= ~(1u << fd);
        g_fd_lock.unlock();
        return (int)fd;
    }
    g_fd_lock.unlock();
    file_unref(f);
    return Error::TooManyFiles;
}

File* fd_get(u64 fd) {
    if (fd >= PROCESS_MAX_FDS) return nullptr;
    g_fd_lock.lock();
    File* f = thread_current()->process->files[fd];
    if (f) file_ref(f);
    g_fd_lock.unlock();
    return f;
}

File* fd_take(u64 fd) {
    if (fd >= PROCESS_MAX_FDS) return nullptr;
    Process* p = thread_current()->process;
    g_fd_lock.lock();
    File* f = p->files[fd];
    p->files[fd] = nullptr;
    p->fd_cloexec &= ~(1u << fd);
    g_fd_lock.unlock();
    return f;
}

Result<void> fd_replace(u64 fd, File* f) {
    if (fd >= PROCESS_MAX_FDS) {
        file_unref(f);
        return Error::BadFd;
    }
    Process* p = thread_current()->process;
    g_fd_lock.lock();
    File* old = p->files[fd];
    p->files[fd] = f;
    p->fd_cloexec &= ~(1u << fd);
    g_fd_lock.unlock();
    if (old) file_unref(old);
    return {};
}

void fd_close_on_exec() {
    Process* p = thread_current()->process;
    File* closing[PROCESS_MAX_FDS];
    usize n = 0;
    g_fd_lock.lock();
    for (usize fd = 0; fd < PROCESS_MAX_FDS; fd++)
        if ((p->fd_cloexec >> fd) & 1 && p->files[fd]) {
            closing[n++] = p->files[fd];
            p->files[fd] = nullptr;
        }
    p->fd_cloexec = 0;
    g_fd_lock.unlock();
    for (usize i = 0; i < n; i++) file_unref(closing[i]);
}

void fd_fork(Process* child) {
    Process* p = thread_current()->process;
    g_fd_lock.lock();
    for (usize fd = 0; fd < PROCESS_MAX_FDS; fd++)
        if (p->files[fd]) child->files[fd] = file_ref(p->files[fd]);
    child->fd_cloexec = p->fd_cloexec;
    if (p->cwd) child->cwd = vnode_ref(p->cwd);
    g_fd_lock.unlock();
}

Vnode* cwd_get() {
    Process* p = thread_current()->process;
    g_fd_lock.lock();
    Vnode* v = p->cwd ? vnode_ref(p->cwd) : nullptr;
    g_fd_lock.unlock();
    return v;
}

void cwd_set(Vnode* dir) {
    Process* p = thread_current()->process;
    g_fd_lock.lock();
    Vnode* old = p->cwd;
    p->cwd = dir;
    g_fd_lock.unlock();
    if (old) vnode_unref(old);
}

void poll_wake_locked() {
    g_poll_generation++;
    sched_wake_all_locked(g_poll_wq);
}

void poll_wake() {
    u64 irq = sched_lock();
    poll_wake_locked();
    sched_unlock(irq);
}

u64 poll_generation() { return __atomic_load_n(&g_poll_generation, __ATOMIC_ACQUIRE); }

WaitResult poll_wait(u64 seen, u64 ticks) {
    u64 irq = sched_lock();
    WaitResult r = WaitResult::Woken;
    if (g_poll_generation == seen) r = sched_block_interruptible_locked(&g_poll_wq, ticks);
    sched_unlock(irq);
    return r;
}
