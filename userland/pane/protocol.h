// The Pane window-server protocol (docs/SPEC.md §9): fixed-size messages
// over a port named "pane". Every message begins with a Header; `length` is
// the size of the whole message and must equal the size of the struct for
// its type, or the sender is disconnected (a bad message never crashes the
// server). Window buffers travel as shared-memory descriptors attached to an
// AttachBuffer message. Shared by the server (userland/pane) and the client
// library (userland/libpane); programs use libpane, never this directly.
//
// Protocol version 1 (2026-10-13). Numbers never change; new messages are
// appended to their block.
#pragma once

#include <stdint.h>

namespace pane {

constexpr uint32_t VERSION = 1;
constexpr uint32_t MAX_TITLE = 64;
constexpr uint32_t MAX_NAME = 32;
constexpr uint32_t MAX_WINDOWS_PER_CLIENT = 16;

enum Type : uint32_t {
    // client -> server
    CONNECT = 1,            // Connect
    CREATE_WINDOW = 2,      // CreateWindow
    DESTROY_WINDOW = 3,     // Header only
    ATTACH_BUFFER = 4,      // AttachBuffer + one shared-memory descriptor
    DAMAGE = 5,             // Damage
    COMMIT = 6,             // Header only: present the attached buffer
    SET_TITLE = 7,          // SetTitle
    SET_GEOMETRY = 8,       // Geometry
    RAISE = 9,              // Header only

    // server -> client
    CONNECTED = 100,        // Connected
    WINDOW_CREATED = 101,   // Header only; h.window is the new id, h.serial the request's serial
    CONFIGURE = 102,        // Geometry: the window's new place and size
    KEY = 103,              // Key
    POINTER_MOTION = 104,   // Pointer
    POINTER_BUTTON = 105,   // Pointer
    POINTER_SCROLL = 106,   // Pointer (dz)
    POINTER_ENTER = 107,    // Pointer
    POINTER_LEAVE = 108,    // Header only
    FOCUS_IN = 109,         // Header only
    FOCUS_OUT = 110,        // Header only
    CLOSE_REQUEST = 111,    // Header only
    FRAME = 112,            // Header only: the committed buffer is on screen; draw the next one
    ERROR = 113,            // Error
};

// Window flags (CreateWindow).
constexpr uint32_t WIN_DECORATED = 1 << 0;   // title bar and border drawn by the server
constexpr uint32_t WIN_RESIZABLE = 1 << 1;

// Error codes (Error.code).
constexpr int32_t ERR_PROTOCOL = 1, ERR_NO_WINDOW = 2, ERR_LIMIT = 3, ERR_BUFFER = 4;

struct Header {
    uint32_t type;
    uint32_t length;        // bytes, including this header
    uint32_t serial;        // the sender's running count; echoed in replies
    uint32_t window;        // window id (1..) or 0
};

struct Connect {
    Header h;
    uint32_t version;
    char name[MAX_NAME];    // the program's name, NUL-terminated
};

struct CreateWindow {
    Header h;
    uint32_t width, height; // of the content, pixels
    uint32_t flags;         // WIN_*
    char title[MAX_TITLE];
};

struct AttachBuffer {
    Header h;
    uint32_t width, height; // the buffer's shape
    uint32_t stride;        // pixels per row
    uint32_t format;        // 0: 32-bit BGRX (the only one)
};

struct Damage {
    Header h;
    int32_t x, y, width, height;    // content-relative
};

struct SetTitle {
    Header h;
    char title[MAX_TITLE];
};

struct Geometry {
    Header h;
    int32_t x, y, width, height;    // screen position of the content, and its size
    uint32_t state;         // bit 0 focused
};

struct Connected {
    Header h;
    uint32_t client_id;
    uint32_t screen_w, screen_h;
    uint32_t scale;         // 100 = 1:1 (HiDPI later; here from day one, SPEC phase 12 additions)
};

struct Key {
    Header h;
    uint32_t code;          // key code (the keyboard driver's)
    uint32_t unicode;       // character typed, 0 if none
    uint32_t mods;          // 1 shift, 2 ctrl, 4 alt, 8 super, 16 caps, 32 num
    uint32_t pressed;       // 1 press, 0 release, 2 repeat
};

struct Pointer {
    Header h;
    int32_t x, y;           // content-relative
    int32_t gx, gy;         // screen
    uint32_t button;        // 0 left, 1 right, 2 middle (button messages)
    uint32_t pressed;
    int32_t dz;             // scroll steps (scroll messages)
};

struct Error {
    Header h;
    int32_t code;           // ERR_*
};

// The largest message; receive buffers are this size.
union Message {
    Header h;
    Connect connect;
    CreateWindow create;
    AttachBuffer attach;
    Damage damage;
    SetTitle title;
    Geometry geometry;
    Connected connected;
    Key key;
    Pointer pointer;
    Error error;
};

// The size a message of `type` must have; 0 for an unknown type.
inline uint32_t expected_length(uint32_t type) {
    switch (type) {
    case CONNECT: return sizeof(Connect);
    case CREATE_WINDOW: return sizeof(CreateWindow);
    case ATTACH_BUFFER: return sizeof(AttachBuffer);
    case DAMAGE: return sizeof(Damage);
    case SET_TITLE: return sizeof(SetTitle);
    case SET_GEOMETRY: case CONFIGURE: return sizeof(Geometry);
    case CONNECTED: return sizeof(Connected);
    case KEY: return sizeof(Key);
    case POINTER_MOTION: case POINTER_BUTTON: case POINTER_SCROLL: case POINTER_ENTER: return sizeof(Pointer);
    case ERROR: return sizeof(Error);
    case DESTROY_WINDOW: case COMMIT: case RAISE: case WINDOW_CREATED: case POINTER_LEAVE:
    case FOCUS_IN: case FOCUS_OUT: case CLOSE_REQUEST: case FRAME:
        return sizeof(Header);
    default: return 0;
    }
}

} // namespace pane
