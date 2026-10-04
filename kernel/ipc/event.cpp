// See event.h. A wait scans the watches; if none is ready it sleeps until
// some readiness changes anywhere (poll_wake), a timer of its own is due,
// or the timeout passes, and scans again.
#include <drivers/refclock.h>
#include <ipc/event.h>
#include <mm/kheap.h>
#include <proc/process.h>
#include <sched/sync.h>

namespace {

struct Watch {
    bool used;
    i32 fd;
    File* file;                 // a reference, for descriptor watches
    u32 events;
    u64 data;
    u64 due_us;                 // timers: when it fires; 0 = fired
};

struct EventQueue : KObject {
    Mutex lock;
    Watch watches[event::MAX_WATCHES];
};

void queue_destroy(KObject* self) {
    EventQueue* q = (EventQueue*)self;
    for (Watch& w : q->watches)
        if (w.used && w.file) file_unref(w.file);
    kfree(q);
}

Watch* find(EventQueue* q, i32 fd, File* file) {
    for (Watch& w : q->watches)
        if (w.used && w.fd == fd && w.file == file) return &w;
    return nullptr;
}

} // namespace

Result<File*> event_create() {
    EventQueue* q = (EventQueue*)kzalloc(sizeof(EventQueue));
    if (!q) return Error::NoMemory;
    q->kind = ObjectKind::EventQueue;
    q->destroy = queue_destroy;
    Result<File*> f = object_file(q);
    if (!f.ok()) kfree(q);
    return f;
}

Result<void> event_ctl(File* ev, u32 op, i32 fd, const event::Event& e) {
    EventQueue* q = (EventQueue*)file_object(ev, ObjectKind::EventQueue);
    if (!q) return Error::Invalid;
    File* file = nullptr;       // a reference of this call's own
    if (fd >= 0) {
        file = fd_get((u64)fd);
        if (!file) return Error::BadFd;
        // A queue cannot watch a queue: two watching each other would keep
        // each other alive for ever.
        if (file->vnode->type == VType::Object && ((KObject*)file->vnode->fs_data)->kind == ObjectKind::EventQueue) {
            file_unref(file);
            return Error::Invalid;
        }
    } else if (fd != event::FD_TIMER && fd != event::FD_CHILD) {
        return Error::BadFd;
    }
    // References are dropped after the lock is released: letting go of the
    // last one destroys the object behind it.
    File* drop = file;
    Error err = Error::None;
    q->lock.lock();
    Watch* w = find(q, fd, file);
    switch (op) {
    case event::OP_ADD:
        if (w) {
            err = Error::Exists;
            break;
        }
        for (Watch& slot : q->watches)
            if (!slot.used) {
                w = &slot;
                break;
            }
        if (!w) {
            err = Error::NoSpace;
            break;
        }
        *w = Watch{true, fd, file, e.events, e.data, 0};
        drop = nullptr;         // the watch keeps the reference
        break;
    case event::OP_MOD:
        if (!w) {
            err = Error::Invalid;
            break;
        }
        w->events = e.events;
        w->data = e.data;
        break;
    case event::OP_DEL:
        if (!w) {
            err = Error::Invalid;
            break;
        }
        *w = Watch{};
        w = nullptr;
        break;
    default: err = Error::Invalid;
    }
    if (err == Error::None && w && fd == event::FD_TIMER) w->due_us = refclock_now_us() + e.timeout_ms * 1000;
    q->lock.unlock();
    if (op == event::OP_DEL && err == Error::None && file) file_unref(file);    // the one the watch held
    if (drop) file_unref(drop);
    if (err != Error::None) return err;
    poll_wake();            // a wait already asleep must take the change into account
    return {};
}

Result<u32> event_wait(File* ev, event::Event* out, u32 max, u64 timeout_ms) {
    EventQueue* q = (EventQueue*)file_object(ev, ObjectKind::EventQueue);
    if (!q || max == 0) return Error::Invalid;
    u64 deadline_us = timeout_ms == event::FOREVER ? 0 : refclock_now_us() + timeout_ms * 1000;
    for (;;) {
        u64 seen = poll_generation();
        u64 now = refclock_now_us();
        u64 next_timer = 0;
        u32 n = 0;
        q->lock.lock();
        for (Watch& w : q->watches) {
            if (!w.used || n == max) continue;
            u32 ready = 0;
            if (w.fd == event::FD_TIMER) {
                if (w.due_us && now >= w.due_us) {
                    ready = poll::TIMER;
                    w.due_us = 0;                   // one shot
                } else if (w.due_us && (!next_timer || w.due_us < next_timer)) {
                    next_timer = w.due_us;
                }
            } else if (w.fd == event::FD_CHILD) {
                if (process_has_exited_child()) ready = poll::CHILD;
            } else {
                // Closing and errors are always reported, wanted or not.
                ready = file_poll(w.file) & (w.events | poll::HUP);
            }
            if (ready) out[n++] = event::Event{ready, w.fd, w.data, 0};
        }
        q->lock.unlock();
        if (n) return n;
        if (deadline_us && now >= deadline_us) return (u32)0;
        u64 wake_us = next_timer;
        if (deadline_us && (!wake_us || deadline_us < wake_us)) wake_us = deadline_us;
        u64 ticks = wake_us ? (wake_us - now + SCHED_TICK_MS * 1000 - 1) / (SCHED_TICK_MS * 1000) : 0;
        if (wake_us && !ticks) ticks = 1;
        if (poll_wait(seen, ticks) == WaitResult::Interrupted) return Error::Interrupted;
    }
}
