// See port.h. Queue state is protected by the scheduler lock, which also
// makes "find the queue empty and go to sleep" one step; memory is
// allocated and freed, and descriptors released, outside it.
#include <ipc/port.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <sched/sync.h>

namespace {

struct Message {
    Message* next;
    u8* data;
    usize len;
    File* fds[port::FDS_MAX];
    u32 nfds;
};

struct Queue {
    Message* head;
    Message* tail;
    u32 count;
    usize bytes;
    WaitQueue readers;          // waiting for a message
    WaitQueue writers;          // waiting for room
};

struct Channel {
    Queue inbox[2];             // inbox[s]: messages for side s
    bool open[2];
    PortPeer who[2];            // who[s]: the process holding side s
};

struct Endpoint : KObject {
    Channel* channel;
    u32 side;
};

struct Listener : KObject {
    char name[port::NAME_MAX + 1];
    u32 uid, gid, mode;
    u32 owner_pid;
    File* pending[port::BACKLOG];   // server ends not yet accepted
    u32 pending_count;
    WaitQueue acceptors;
    Listener* next;
};

Listener* g_listeners = nullptr;    // scheduler lock

void free_message(Message* m) {
    for (u32 i = 0; i < m->nfds; i++) file_unref(m->fds[i]);
    kfree(m->data);
    kfree(m);
}

u32 endpoint_poll(KObject* self) {
    Endpoint* e = (Endpoint*)self;
    Channel* c = e->channel;
    u32 peer = e->side ^ 1;
    u32 r = 0;
    u64 irq = sched_lock();
    if (c->inbox[e->side].count) r |= poll::READ;
    if (!c->open[peer]) r |= poll::HUP | poll::READ;
    else if (c->inbox[peer].count < port::QUEUE_MESSAGES) r |= poll::WRITE;
    sched_unlock(irq);
    return r;
}

void endpoint_destroy(KObject* self) {
    Endpoint* e = (Endpoint*)self;
    Channel* c = e->channel;
    u64 irq = sched_lock();
    c->open[e->side] = false;
    bool last = !c->open[e->side ^ 1];
    // Nobody will read this side's inbox any more.
    // (Only the messages go: senders may be asleep on the queue's wait list,
    // and are woken just below to find the channel closed.)
    Queue& mine = c->inbox[e->side];
    Message* dead = mine.head;
    mine.head = mine.tail = nullptr;
    mine.count = 0;
    mine.bytes = 0;
    // The peer may be waiting for a message from us, or for room.
    sched_wake_all_locked(c->inbox[e->side ^ 1].readers);
    sched_wake_all_locked(c->inbox[e->side].writers);
    poll_wake_locked();
    sched_unlock(irq);
    while (dead) {
        Message* next = dead->next;
        free_message(dead);
        dead = next;
    }
    if (last) kfree(c);
    kfree(e);
}

u32 listener_poll(KObject* self) {
    Listener* l = (Listener*)self;
    return __atomic_load_n(&l->pending_count, __ATOMIC_RELAXED) ? poll::READ : 0;
}

void listener_destroy(KObject* self) {
    Listener* l = (Listener*)self;
    u64 irq = sched_lock();
    for (Listener** link = &g_listeners; *link; link = &(*link)->next)
        if (*link == l) {
            *link = l->next;
            break;
        }
    u32 n = l->pending_count;
    l->pending_count = 0;
    sched_unlock(irq);
    // Connections nobody accepted: closing the server ends tells the clients.
    for (u32 i = 0; i < n; i++) file_unref(l->pending[i]);
    kfree(l);
}

Result<File*> make_endpoint(Channel* c, u32 side) {
    Endpoint* e = (Endpoint*)kzalloc(sizeof(Endpoint));
    if (!e) return Error::NoMemory;
    e->kind = ObjectKind::PortEndpoint;
    e->destroy = endpoint_destroy;
    e->poll = endpoint_poll;
    e->channel = c;
    e->side = side;
    Result<File*> f = object_file(e);
    if (!f.ok()) kfree(e);
    return f;
}

bool name_ok(const char* name) {
    usize len = strlen(name);
    if (len == 0 || len > port::NAME_MAX) return false;
    for (usize i = 0; i < len; i++)
        if ((u8)name[i] <= 0x20 || (u8)name[i] >= 0x7F) return false;
    return true;
}

u64 ticks_of(u64 timeout_ms) { return (timeout_ms + SCHED_TICK_MS - 1) / SCHED_TICK_MS + 1; }

} // namespace

Result<File*> port_create(const char* name, u32 mode, const Credentials& cred, u32 pid) {
    if (!name_ok(name)) return Error::Invalid;
    Listener* l = (Listener*)kzalloc(sizeof(Listener));
    if (!l) return Error::NoMemory;
    l->kind = ObjectKind::PortListener;
    l->destroy = listener_destroy;
    l->poll = listener_poll;
    strlcpy(l->name, name, sizeof l->name);
    l->uid = cred.uid;
    l->gid = cred.gid;
    l->mode = mode & 0777;
    l->owner_pid = pid;
    Result<File*> f = object_file(l);
    if (!f.ok()) {
        kfree(l);
        return f.error();
    }
    u64 irq = sched_lock();
    bool taken = false;
    for (Listener* o = g_listeners; o; o = o->next) taken |= strcmp(o->name, name) == 0;
    if (!taken) {
        l->next = g_listeners;
        g_listeners = l;
    }
    sched_unlock(irq);
    if (taken) {
        file_unref(f.value());      // destroys the listener (it was never listed)
        return Error::Exists;
    }
    return f;
}

Result<File*> port_connect(const char* name, const Credentials& cred, u32 pid) {
    if (!name_ok(name)) return Error::NotFound;
    Channel* c = (Channel*)kzalloc(sizeof(Channel));
    if (!c) return Error::NoMemory;
    c->open[0] = c->open[1] = true;
    c->who[0] = {pid, cred.uid, cred.gid};          // side 0: the client
    Result<File*> client = make_endpoint(c, 0);
    if (!client.ok()) {
        kfree(c);
        return client.error();
    }
    Result<File*> server = make_endpoint(c, 1);
    if (!server.ok()) {
        c->open[1] = false;                          // so closing the client frees the channel
        file_unref(client.value());
        return server.error();
    }
    Error err = Error::None;
    u64 irq = sched_lock();
    Listener* l = g_listeners;
    while (l && strcmp(l->name, name) != 0) l = l->next;
    if (!l) {
        err = Error::NotFound;
    } else {
        // Connecting is writing to the port: check the write bit that
        // applies to this caller, as for a file.
        u32 bits = cred.uid == 0 ? 7 : cred.uid == l->uid ? (l->mode >> 6) & 7 : cred.gid == l->gid ? (l->mode >> 3) & 7
                                                                                                    : l->mode & 7;
        if (!(bits & 2)) err = Error::Access;
        else if (l->pending_count == port::BACKLOG) err = Error::Again;
        else {
            c->who[1] = {l->owner_pid, l->uid, l->gid};  // side 1: the server
            l->pending[l->pending_count++] = server.value();
            sched_wake_one_locked(l->acceptors);
            poll_wake_locked();
        }
    }
    sched_unlock(irq);
    if (err != Error::None) {
        file_unref(server.value());
        file_unref(client.value());
        return err;
    }
    return client;
}

Result<void> port_send(File* f, u8* data, usize len, File** fds, u32 nfds, bool nonblock) {
    Endpoint* e = (Endpoint*)file_object(f, ObjectKind::PortEndpoint);
    Message* m = e ? (Message*)kzalloc(sizeof(Message)) : nullptr;
    if (!m) {
        for (u32 i = 0; i < nfds; i++) file_unref(fds[i]);
        kfree(data);
        return e ? Error::NoMemory : Error::Invalid;
    }
    m->data = data;
    m->len = len;
    m->nfds = nfds;
    for (u32 i = 0; i < nfds; i++) m->fds[i] = fds[i];

    Channel* c = e->channel;
    u32 peer = e->side ^ 1;
    Error err = Error::None;
    // Files, devices and shared memory can be passed. Ports and event
    // queues cannot: a descriptor queued inside the object it refers to
    // (directly or through another port) would keep both alive for ever.
    for (u32 i = 0; i < nfds; i++)
        if (fds[i]->vnode->type == VType::Object && !file_object(fds[i], ObjectKind::Shm)) err = Error::Invalid;
    if (err != Error::None) {
        free_message(m);
        return err;
    }
    u64 irq = sched_lock();
    for (;;) {
        Queue& q = c->inbox[peer];
        if (!c->open[peer]) {
            err = Error::Pipe;
            break;
        }
        if (q.count < port::QUEUE_MESSAGES && q.bytes + len <= port::QUEUE_BYTES) {
            m->next = nullptr;
            if (q.tail) q.tail->next = m;
            else q.head = m;
            q.tail = m;
            q.count++;
            q.bytes += len;
            sched_wake_one_locked(q.readers);
            poll_wake_locked();
            break;
        }
        if (nonblock) {
            err = Error::Again;
            break;
        }
        if (sched_block_interruptible_locked(&q.writers, 0) == WaitResult::Interrupted) {
            err = Error::Interrupted;
            break;
        }
    }
    sched_unlock(irq);
    if (err != Error::None) {
        free_message(m);
        return err;
    }
    return {};
}

Result<void> port_recv(File* f, usize max_len, u32 max_fds, u64 timeout_ms, PortMessage* out) {
    memset(out, 0, sizeof *out);
    u64 deadline = timeout_ms == port::FOREVER ? 0 : sched_ticks() + ticks_of(timeout_ms);
    auto wait = [&](WaitQueue& wq) -> Error {
        if (timeout_ms == 0) return Error::Again;
        u64 ticks = 0;
        if (deadline) {
            u64 now = sched_ticks();
            if (now >= deadline) return Error::Timeout;
            ticks = deadline - now;
        }
        WaitResult r = sched_block_interruptible_locked(&wq, ticks);
        if (r == WaitResult::Interrupted) return Error::Interrupted;
        return Error::None;         // woken or timed out: look again (the loop notices the deadline)
    };

    if (Listener* l = (Listener*)file_object(f, ObjectKind::PortListener)) {
        if (max_fds < 1) return Error::TooBig;
        Error err = Error::None;
        u64 irq = sched_lock();
        while (!l->pending_count && err == Error::None) err = wait(l->acceptors);
        if (err == Error::None) {
            out->fds[0] = l->pending[0];
            out->nfds = 1;
            l->pending_count--;
            for (u32 i = 0; i < l->pending_count; i++) l->pending[i] = l->pending[i + 1];
        }
        sched_unlock(irq);
        if (err != Error::None) return err;
        return {};
    }

    Endpoint* e = (Endpoint*)file_object(f, ObjectKind::PortEndpoint);
    if (!e) return Error::Invalid;
    Channel* c = e->channel;
    Queue& q = c->inbox[e->side];
    Message* m = nullptr;
    Error err = Error::None;
    u64 irq = sched_lock();
    for (;;) {
        if (q.head) {
            if (q.head->len > max_len || q.head->nfds > max_fds) {
                err = Error::TooBig;
                break;
            }
            m = q.head;
            q.head = m->next;
            if (!q.head) q.tail = nullptr;
            q.count--;
            q.bytes -= m->len;
            sched_wake_all_locked(q.writers);
            poll_wake_locked();
            break;
        }
        if (!c->open[e->side ^ 1]) {
            err = Error::Pipe;
            break;
        }
        err = wait(q.readers);
        if (err != Error::None) break;
    }
    sched_unlock(irq);
    if (err != Error::None) return err;
    out->data = m->data;
    out->len = m->len;
    out->nfds = m->nfds;
    for (u32 i = 0; i < m->nfds; i++) out->fds[i] = m->fds[i];
    kfree(m);
    return {};
}

Result<PortPeer> port_peer(File* f) {
    Endpoint* e = (Endpoint*)file_object(f, ObjectKind::PortEndpoint);
    if (!e) return Error::Invalid;
    return e->channel->who[e->side ^ 1];
}
