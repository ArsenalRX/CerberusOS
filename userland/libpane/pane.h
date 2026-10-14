// libpane: what a program uses to put windows on the screen through the
// Pane window server (SPEC phase 12). A connection is a channel to the
// server's port; a window is a shared-memory buffer the program draws into,
// then tells the server which part changed (damage) and when to show it
// (commit). Input comes back as events, only for the program's own windows.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace pane {

struct Window {
    uint32_t id = 0;
    uint32_t* pixels = nullptr;     // width x height, `stride` pixels per row, 32-bit BGRX
    int width = 0, height = 0, stride = 0;
    int x = 0, y = 0;               // screen position of the content (from Configure)
    bool focused = false;
    int shm = -1;                   // the buffer's shared-memory descriptor
    size_t bytes = 0;
};

enum EventType {
    EVT_NONE = 0,
    EVT_KEY,                 // key, unicode, mods, pressed
    EVT_POINTER_MOTION,      // x, y
    EVT_POINTER_BUTTON,      // x, y, button, pressed
    EVT_POINTER_SCROLL,      // x, y, dz
    EVT_POINTER_ENTER,
    EVT_POINTER_LEAVE,
    EVT_FOCUS_IN,
    EVT_FOCUS_OUT,
    EVT_CLOSE,               // the user asked to close the window
    EVT_FRAME,               // the last commit is on screen: draw the next frame now
    EVT_CONFIGURE,           // x, y, w, h: the window moved or was resized
    EVT_DISCONNECTED,        // the server went away
};

struct Event {
    EventType type = EVT_NONE;
    Window* window = nullptr;
    int x = 0, y = 0, w = 0, h = 0;
    uint32_t key = 0, unicode = 0, mods = 0;
    int button = 0, pressed = 0, dz = 0;
};

class Connection {
public:
    // Connects to the server as `name`. False (and errno) if there is no
    // server or it refused.
    bool connect(const char* name);
    void close();
    bool ok() const { return channel_ >= 0; }
    int fd() const { return channel_; }          // for event_wait
    uint32_t screen_width() const { return screen_w_; }
    uint32_t screen_height() const { return screen_h_; }

    // Makes a decorated window with a buffer of the given size; null on
    // failure. The buffer is black until the program draws and commits.
    Window* create_window(int width, int height, const char* title);
    void destroy_window(Window* w);
    void set_title(Window* w, const char* title);
    // Marks a content rectangle as changed; commit shows every marked
    // rectangle at once. A window with no damage since the last commit is
    // not recomposited.
    void damage(Window* w, int x, int y, int width, int height);
    void commit(Window* w);

    // Waits up to timeout_ms (~0: forever) for the next event; false on
    // timeout. EVT_DISCONNECTED when the server is gone.
    bool next_event(Event* out, uint64_t timeout_ms = ~0ull);

private:
    bool send(const void* msg, size_t len, const int* fds = nullptr, int nfds = 0);
    bool recv(void* msg, size_t cap, uint64_t timeout_ms, int* fd_out = nullptr);
    Window* find(uint32_t id);

    int channel_ = -1;
    uint32_t serial_ = 0;
    uint32_t screen_w_ = 0, screen_h_ = 0;
    static constexpr int MAX_WINDOWS = 16;
    Window windows_[MAX_WINDOWS];
};

} // namespace pane
