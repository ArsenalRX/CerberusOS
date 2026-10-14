// libpane (see pane.h): the client side of userland/pane/protocol.h.
#include "pane.h"
#include "../pane/protocol.h"
#include <cerberus.h>

namespace pane {

bool Connection::send(const void* msg, size_t len, const int* fds, int nfds) {
    if (channel_ < 0) return false;
    if (port_send(channel_, msg, len, fds, nfds) < 0) {
        close();
        return false;
    }
    return true;
}

bool Connection::recv(void* msg, size_t cap, uint64_t timeout_ms, int* fd_out) {
    if (channel_ < 0) return false;
    int fds[1];
    int nfds = fd_out ? 1 : 0;
    long n = port_recv(channel_, msg, cap, fds, &nfds, timeout_ms);
    if (n < 0) {
        if (errno == EAGAIN || errno == ETIMEDOUT) return false;
        close();
        return false;
    }
    if (fd_out) *fd_out = nfds ? fds[0] : -1;
    Header* h = (Header*)msg;
    if ((size_t)n < sizeof(Header) || h->length != (uint32_t)n || expected_length(h->type) != h->length) {
        // the server spoke garbage: treat it as gone
        close();
        return false;
    }
    return true;
}

bool Connection::connect(const char* name) {
    close();
    int ch = port_connect("pane");
    if (ch < 0) return false;
    channel_ = ch;
    Connect c{};
    c.h.type = CONNECT;
    c.h.length = sizeof c;
    c.h.serial = ++serial_;
    c.version = VERSION;
    strlcpy(c.name, name ? name : "program", sizeof c.name);
    if (!send(&c, sizeof c)) return false;
    Message m;
    if (!recv(&m, sizeof m, 5000) || m.h.type != CONNECTED) {
        close();
        return false;
    }
    screen_w_ = m.connected.screen_w;
    screen_h_ = m.connected.screen_h;
    return true;
}

void Connection::close() {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows_[i].id) {
            if (windows_[i].pixels) shm_unmap(windows_[i].pixels);
            if (windows_[i].shm >= 0) ::close(windows_[i].shm);
            windows_[i] = Window{};
        }
    if (channel_ >= 0) ::close(channel_);
    channel_ = -1;
}

Window* Connection::find(uint32_t id) {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (windows_[i].id == id) return &windows_[i];
    return nullptr;
}

Window* Connection::create_window(int width, int height, const char* title) {
    if (channel_ < 0 || width < 1 || height < 1 || width > 8192 || height > 8192) return nullptr;
    Window* w = nullptr;
    for (int i = 0; i < MAX_WINDOWS && !w; i++)
        if (!windows_[i].id) w = &windows_[i];
    if (!w) return nullptr;

    CreateWindow c{};
    c.h.type = CREATE_WINDOW;
    c.h.length = sizeof c;
    c.h.serial = ++serial_;
    c.width = (uint32_t)width;
    c.height = (uint32_t)height;
    c.flags = WIN_DECORATED;
    strlcpy(c.title, title ? title : "", sizeof c.title);
    if (!send(&c, sizeof c)) return nullptr;
    // The reply comes in order; events for other windows may precede it
    // and are kept for next_event by the server's ordering (we only have
    // one reader, so wait for the created message and remember nothing
    // else: the server sends WINDOW_CREATED before any event for it).
    Message m;
    for (;;) {
        if (!recv(&m, sizeof m, 5000)) return nullptr;
        if (m.h.type == WINDOW_CREATED && m.h.serial == c.h.serial) break;
        if (m.h.type == ERROR) return nullptr;
    }
    size_t bytes = (size_t)width * (size_t)height * 4;
    int shm = shm_create(bytes);
    if (shm < 0) return nullptr;
    void* px = shm_map(shm, PROT_READ | PROT_WRITE);
    if (px == MAP_FAILED) {
        ::close(shm);
        return nullptr;
    }
    AttachBuffer a{};
    a.h.type = ATTACH_BUFFER;
    a.h.length = sizeof a;
    a.h.serial = ++serial_;
    a.h.window = m.h.window;
    a.width = (uint32_t)width;
    a.height = (uint32_t)height;
    a.stride = (uint32_t)width;
    a.format = 0;
    if (!send(&a, sizeof a, &shm, 1)) {
        shm_unmap(px);
        ::close(shm);
        return nullptr;
    }
    w->id = m.h.window;
    w->pixels = (uint32_t*)px;
    w->width = width;
    w->height = height;
    w->stride = width;
    w->shm = shm;
    w->bytes = bytes;
    return w;
}

void Connection::destroy_window(Window* w) {
    if (!w || !w->id) return;
    Header h{DESTROY_WINDOW, sizeof(Header), ++serial_, w->id};
    send(&h, sizeof h);
    if (w->pixels) shm_unmap(w->pixels);
    if (w->shm >= 0) ::close(w->shm);
    *w = Window{};
}

void Connection::set_title(Window* w, const char* title) {
    if (!w || !w->id) return;
    SetTitle t{};
    t.h = Header{SET_TITLE, sizeof t, ++serial_, w->id};
    strlcpy(t.title, title ? title : "", sizeof t.title);
    send(&t, sizeof t);
}

void Connection::damage(Window* w, int x, int y, int width, int height) {
    if (!w || !w->id) return;
    Damage d{};
    d.h = Header{DAMAGE, sizeof d, ++serial_, w->id};
    d.x = x;
    d.y = y;
    d.width = width;
    d.height = height;
    send(&d, sizeof d);
}

void Connection::commit(Window* w) {
    if (!w || !w->id) return;
    Header h{COMMIT, sizeof(Header), ++serial_, w->id};
    send(&h, sizeof h);
}

bool Connection::next_event(Event* out, uint64_t timeout_ms) {
    *out = Event{};
    if (channel_ < 0) {
        out->type = EVT_DISCONNECTED;
        return true;
    }
    Message m;
    if (!recv(&m, sizeof m, timeout_ms)) {
        if (channel_ < 0) {
            out->type = EVT_DISCONNECTED;
            return true;
        }
        return false;
    }
    out->window = find(m.h.window);
    switch (m.h.type) {
    case KEY:
        out->type = EVT_KEY;
        out->key = m.key.code;
        out->unicode = m.key.unicode;
        out->mods = m.key.mods;
        out->pressed = (int)m.key.pressed;
        break;
    case POINTER_MOTION: case POINTER_BUTTON: case POINTER_SCROLL: case POINTER_ENTER:
        out->type = m.h.type == POINTER_MOTION ? EVT_POINTER_MOTION
                  : m.h.type == POINTER_BUTTON ? EVT_POINTER_BUTTON
                  : m.h.type == POINTER_SCROLL ? EVT_POINTER_SCROLL : EVT_POINTER_ENTER;
        out->x = m.pointer.x;
        out->y = m.pointer.y;
        out->button = (int)m.pointer.button;
        out->pressed = (int)m.pointer.pressed;
        out->dz = m.pointer.dz;
        break;
    case POINTER_LEAVE: out->type = EVT_POINTER_LEAVE; break;
    case FOCUS_IN:
        out->type = EVT_FOCUS_IN;
        if (out->window) out->window->focused = true;
        break;
    case FOCUS_OUT:
        out->type = EVT_FOCUS_OUT;
        if (out->window) out->window->focused = false;
        break;
    case CLOSE_REQUEST: out->type = EVT_CLOSE; break;
    case FRAME: out->type = EVT_FRAME; break;
    case CONFIGURE:
        out->type = EVT_CONFIGURE;
        out->x = m.geometry.x;
        out->y = m.geometry.y;
        out->w = m.geometry.width;
        out->h = m.geometry.height;
        if (out->window) {
            out->window->x = m.geometry.x;
            out->window->y = m.geometry.y;
        }
        break;
    case ERROR:
        out->type = EVT_NONE;            // the server refused something; nothing to do
        break;
    default:
        out->type = EVT_NONE;
        break;
    }
    return true;
}

} // namespace pane
