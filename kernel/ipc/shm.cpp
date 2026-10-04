// See shm.h.
#include <ipc/shm.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <sched/sync.h>

namespace {

struct Shm : KObject {
    paddr_t phys;
    usize pages;
    u32 mappings;               // address spaces it is mapped in (g_lock)
    bool closed;                // no descriptor left
};

Mutex g_lock;                   // mapping counts and every process's mapping list

void free_block(Shm* s) {
    pmm_free(s->phys, s->pages);
    kfree(s);
}

void shm_destroy(KObject* self) {
    Shm* s = (Shm*)self;
    g_lock.lock();
    s->closed = true;
    bool unused = s->mappings == 0;
    g_lock.unlock();
    if (unused) free_block(s);
}

// g_lock held.
void drop_mapping(Shm* s) {
    if (--s->mappings == 0 && s->closed) free_block(s);
}

} // namespace

struct ShmMapping {
    vaddr_t addr;
    Shm* shm;
    ShmMapping* next;
};

Result<File*> shm_create(usize size) {
    if (size == 0 || size > SHM_MAX_BYTES) return Error::Invalid;
    Shm* s = (Shm*)kzalloc(sizeof(Shm));
    if (!s) return Error::NoMemory;
    s->kind = ObjectKind::Shm;
    s->destroy = shm_destroy;
    s->pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    s->phys = pmm_alloc_zeroed(s->pages);
    if (s->phys == PMM_NO_MEMORY) {
        kfree(s);
        return Error::NoMemory;
    }
    Result<File*> f = object_file(s);
    if (!f.ok()) free_block(s);
    return f;
}

usize shm_size(File* f) {
    Shm* s = (Shm*)file_object(f, ObjectKind::Shm);
    return s ? s->pages * PAGE_SIZE : 0;
}

Result<vaddr_t> shm_map(File* f, bool writable) {
    Shm* s = (Shm*)file_object(f, ObjectKind::Shm);
    if (!s) return Error::Invalid;
    Process* p = thread_current()->process;
    ShmMapping* m = (ShmMapping*)kzalloc(sizeof(ShmMapping));
    if (!m) return Error::NoMemory;
    Result<vaddr_t> at = p->space->mmap_device(s->phys, s->pages * PAGE_SIZE, writable ? vm::WRITE : 0);
    if (!at.ok()) {
        kfree(m);
        return at.error();
    }
    m->addr = at.value();
    m->shm = s;
    g_lock.lock();
    s->mappings++;
    m->next = p->shm_maps;
    p->shm_maps = m;
    g_lock.unlock();
    return at.value();
}

Result<void> shm_unmap(vaddr_t addr) {
    Process* p = thread_current()->process;
    g_lock.lock();
    ShmMapping** link = &p->shm_maps;
    while (*link && (*link)->addr != addr) link = &(*link)->next;
    ShmMapping* m = *link;
    if (!m) {
        g_lock.unlock();
        return Error::Invalid;
    }
    *link = m->next;
    // Unmap before the block can be freed: no address space may keep a
    // translation to frames that have been given back.
    (void)p->space->munmap(m->addr, m->shm->pages * PAGE_SIZE);
    drop_mapping(m->shm);
    g_lock.unlock();
    kfree(m);
    return {};
}

Result<void> shm_fork(Process* parent, Process* child) {
    g_lock.lock();
    Error err = Error::None;
    for (ShmMapping* m = parent->shm_maps; m; m = m->next) {
        ShmMapping* c = (ShmMapping*)kzalloc(sizeof(ShmMapping));
        if (!c) {
            err = Error::NoMemory;
            break;
        }
        c->addr = m->addr;
        c->shm = m->shm;
        m->shm->mappings++;
        c->next = child->shm_maps;
        child->shm_maps = c;
    }
    g_lock.unlock();
    if (err != Error::None) return err;
    return {};
}

// The caller destroys (or has destroyed) the address space itself; only
// the bookkeeping is undone here, after the translations are gone.
void shm_release_process(Process* p) {
    g_lock.lock();
    ShmMapping* m = p->shm_maps;
    p->shm_maps = nullptr;
    while (m) {
        ShmMapping* next = m->next;
        drop_mapping(m->shm);
        kfree(m);
        m = next;
    }
    g_lock.unlock();
}
