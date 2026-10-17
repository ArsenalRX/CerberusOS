// Pane, the window server (SPEC phase 12, §9), as a user program. It owns
// the screen through a shared mapping of /dev/fb0 and reads the keyboard
// and mouse from /dev/input/*; programs connect to the port "pane", draw
// into shared-memory buffers and tell Pane what changed. Pane composites
// only the damaged parts of the screen into a back buffer and writes those
// to the framebuffer.
//
// Slices so far (0.0.6b): decorated windows with rounded corners and a
// shadow; move by the title bar; resize at the edges and corners; stacking
// and focus by click; Alt+Tab; Super+arrows snap left/right/maximise/
// restore; keyboard to the focused window, pointer events to the window
// under it; damage-driven compositing; frame callbacks; a client only ever
// names its own windows and a bad message disconnects that client, never
// Pane (SPEC §5A phase 12). `pane --test` starts /bin/panetest and checks
// the acceptance points; `pane --demo` shows four windows; Ctrl+Alt+Q
// leaves (the kernel desktop comes back).
#include <cerberus.h>
#include <gfx/gfx.h>
#include "protocol.h"

using namespace pane;
using namespace gfx;

namespace {

constexpr int MAX_CLIENTS = 16, MAX_WINS = 64, MAX_DAMAGE = 48;
constexpr int TITLE_H = 28, BORDER = 1, CLOSE_W = 36, RADIUS = 10, SHADOW = 12, GRIP = 7;
constexpr int MIN_W = 120, MIN_H = 48;
constexpr int CURSOR_W = 12, CURSOR_H = 19;
constexpr uint64_t FRAME_MS = 8;

struct Client {
    bool used = false;
    int ch = -1;
    uint32_t id = 0;
    char name[MAX_NAME] = {};
    uint32_t serial = 0;
    bool connected = false;         // sent Connect
};

struct Win {
    bool used = false;
    int client = -1;
    uint32_t id = 0;
    int x = 0, y = 0, w = 0, h = 0; // content on screen
    uint32_t flags = 0;
    char title[MAX_TITLE] = {};
    uint32_t* px = nullptr;         // the client's buffer, mapped here
    size_t bytes = 0;
    int bw = 0, bh = 0, stride = 0;
    int shm = -1;
    Rect pending{0, 0, 0, 0};       // damage since the last commit (content-relative)
    bool want_frame = false;        // tell the client when it is shown
    bool ever_committed = false;
    bool snapped = false;           // geometry set by a snap; `saved` restores it
    Rect saved{0, 0, 0, 0};
};

Client g_clients[MAX_CLIENTS];
Win g_wins[MAX_WINS];
int g_order[MAX_WINS];              // bottom .. top
int g_norder = 0;
int g_focus = -1;
uint32_t g_next_client = 1, g_next_window = 1;

int g_W = 0, g_H = 0;
uint32_t* g_fb = nullptr;           // the screen (write-combined)
int g_fb_stride = 0;                // pixels
int g_fbfd = -1, g_kbd = -1, g_mouse = -1, g_listen = -1, g_ev = -1;
Surface g_back;                     // composed scene
Surface g_wall;                     // wallpaper, rendered once
Font g_font, g_bold;

Rect g_dmg[MAX_DAMAGE];
int g_ndmg = 0;
bool g_dmg_all = false;
bool g_frame_armed = false;

int g_mx = 0, g_my = 0;
uint8_t g_buttons = 0;
int g_hover = -1;                   // window under the pointer
int g_drag = -1;                    // window being moved by its title bar
int g_drag_dx = 0, g_drag_dy = 0;
int g_resize = -1;                  // window being resized
int g_resize_edges = 0;             // bit 0 left, 1 right, 2 top, 3 bottom
Rect g_resize_start{0, 0, 0, 0};
int g_resize_mx = 0, g_resize_my = 0;

bool g_test = false;
bool g_demo = false;                // --demo: start /bin/panetest demo and keep running
bool g_log = false;                 // --test or --demo: window management goes to the log
bool g_quit = false;
long g_frames = 0;
long g_last_commit_px = -1;         // pixels recomposited by the frame after the last commit
bool g_commit_frame = false;

extern "C" const uint8_t _binary_fonts_aa_start[];
extern "C" const uint8_t _binary_fonts_aa_end[];

// ------------------------------------------------------------ geometry --
bool decorated(const Win& w) { return w.flags & WIN_DECORATED; }
Rect frame_rect(const Win& w) {
    if (!decorated(w)) return {w.x, w.y, w.w, w.h};
    return {w.x - BORDER, w.y - TITLE_H - BORDER, w.w + 2 * BORDER, w.h + TITLE_H + 2 * BORDER};
}
Rect outer_rect(const Win& w) { return frame_rect(w).inset(-SHADOW); }   // with the shadow
Rect title_rect(const Win& w) { return {w.x, w.y - TITLE_H, w.w, TITLE_H}; }
Rect close_rect(const Win& w) { return {w.x + w.w - CLOSE_W, w.y - TITLE_H, CLOSE_W, TITLE_H}; }
Rect cursor_rect() { return {g_mx, g_my, CURSOR_W, CURSOR_H}; }

void add_damage(Rect r) {
    r = r.intersect({0, 0, g_W, g_H});
    if (r.empty() || g_dmg_all) return;
    if (g_ndmg == MAX_DAMAGE) {
        g_dmg_all = true;
        g_ndmg = 0;
        return;
    }
    g_dmg[g_ndmg++] = r;
}

int order_index(int win) {
    for (int i = 0; i < g_norder; i++)
        if (g_order[i] == win) return i;
    return -1;
}

void raise_window(int win) {
    int i = order_index(win);
    if (i < 0 || i == g_norder - 1) return;
    for (; i < g_norder - 1; i++) g_order[i] = g_order[i + 1];
    g_order[g_norder - 1] = win;
    add_damage(outer_rect(g_wins[win]));
}

// The resize edges the point touches on a decorated, resizable window.
int edges_at(const Win& w, int x, int y) {
    if (!decorated(w) || !(w.flags & WIN_RESIZABLE)) return 0;
    Rect f = frame_rect(w);
    if (!f.inset(-GRIP).contains(x, y)) return 0;
    int e = 0;
    if (x < f.x + GRIP) e |= 1;
    if (x >= f.right() - GRIP) e |= 2;
    if (y < f.y + GRIP) e |= 4;
    if (y >= f.bottom() - GRIP) e |= 8;
    return e;
}

int window_at(int x, int y, bool* in_title, bool* in_close, int* edges) {
    for (int i = g_norder - 1; i >= 0; i--) {
        const Win& w = g_wins[g_order[i]];
        int e = edges_at(w, x, y);
        if (!e && !frame_rect(w).contains(x, y)) continue;
        if (in_title) *in_title = decorated(w) && !e && title_rect(w).contains(x, y);
        if (in_close) *in_close = decorated(w) && !e && close_rect(w).contains(x, y);
        if (edges) *edges = e;
        return g_order[i];
    }
    return -1;
}

// ------------------------------------------------------------- sending --
bool send_to(int client, const void* msg, size_t len) {
    Client& c = g_clients[client];
    if (!c.used || c.ch < 0) return false;
    Header* h = (Header*)msg;
    h->length = (uint32_t)len;
    h->serial = ++c.serial;
    // Never wait on a client: a stuck program must not stop the screen.
    return port_try_send(c.ch, msg, len, nullptr, 0) >= 0;
}
void send_header(int client, uint32_t type, uint32_t window) {
    Header h{type, sizeof h, 0, window};
    send_to(client, &h, sizeof h);
}
void send_error(int client, uint32_t window, int32_t code) {
    Error e{};
    e.h = Header{ERROR, sizeof e, 0, window};
    e.code = code;
    send_to(client, &e, sizeof e);
}
void send_configure(const Win& w) {
    Geometry g{};
    g.h = Header{CONFIGURE, sizeof g, 0, w.id};
    g.x = w.x;
    g.y = w.y;
    g.width = w.w;
    g.height = w.h;
    g.state = (&w - g_wins) == g_focus ? 1 : 0;
    send_to(w.client, &g, sizeof g);
}
void send_pointer(const Win& w, uint32_t type, int gx, int gy, uint32_t button, uint32_t pressed, int dz) {
    Pointer p{};
    p.h = Header{type, sizeof p, 0, w.id};
    p.x = gx - w.x;
    p.y = gy - w.y;
    p.gx = gx;
    p.gy = gy;
    p.button = button;
    p.pressed = pressed;
    p.dz = dz;
    send_to(w.client, &p, sizeof p);
}

void set_focus(int win) {
    if (win == g_focus) return;
    if (g_focus >= 0 && g_wins[g_focus].used) {
        send_header(g_wins[g_focus].client, FOCUS_OUT, g_wins[g_focus].id);
        add_damage(outer_rect(g_wins[g_focus]));
    }
    g_focus = win;
    if (win >= 0) {
        send_header(g_wins[win].client, FOCUS_IN, g_wins[win].id);
        add_damage(outer_rect(g_wins[win]));
    }
}

void set_geometry(Win& w, int x, int y, int nw, int nh) {
    add_damage(outer_rect(w));
    w.x = x;
    w.y = y;
    w.w = nw;
    w.h = nh;
    if (w.y < TITLE_H + BORDER) w.y = TITLE_H + BORDER;
    add_damage(outer_rect(w));
    send_configure(w);
}

void destroy_window(int win) {
    Win& w = g_wins[win];
    if (!w.used) return;
    add_damage(outer_rect(w));
    int i = order_index(win);
    if (i >= 0) {
        for (; i < g_norder - 1; i++) g_order[i] = g_order[i + 1];
        g_norder--;
    }
    if (w.px) shm_unmap(w.px);
    if (w.shm >= 0) close(w.shm);
    bool was_focus = g_focus == win;
    if (g_hover == win) g_hover = -1;
    if (g_drag == win) g_drag = -1;
    if (g_resize == win) g_resize = -1;
    w = Win{};
    if (was_focus) {
        g_focus = -1;
        if (g_norder) set_focus(g_order[g_norder - 1]);
    }
}

void drop_client(int ci) {
    Client& c = g_clients[ci];
    if (!c.used) return;
    for (int i = 0; i < MAX_WINS; i++)
        if (g_wins[i].used && g_wins[i].client == ci) destroy_window(i);
    if (g_ev >= 0) event_ctl(g_ev, EVENT_DEL, c.ch, nullptr);
    close(c.ch);
    if (g_test) printf("pane: client %u (%s) left\n", c.id, c.name);
    c = Client{};
}

// ------------------------------------------------------------- drawing --
// Source-over of a translucent colour onto a rectangle (libgfx fills are
// opaque; the shadow needs blending).
void blend_rect(Surface& s, const Rect& r, Color c) {
    Rect rr = r.intersect(s.clip);
    if (rr.empty()) return;
    uint32_t a = alpha_of(c);
    if (!a) return;
    for (int y = rr.y; y < rr.bottom(); y++) {
        uint32_t* row = s.row(y);
        for (int x = rr.x; x < rr.right(); x++) row[x] = mix(row[x], c | 0xFF000000u, (uint8_t)a);
    }
}

void draw_cursor(Surface& s) {
    static const char* const SHAPE[CURSOR_H] = {
        "B           ", "BB          ", "BWB         ", "BWWB        ", "BWWWB       ", "BWWWWB      ",
        "BWWWWWB     ", "BWWWWWWB    ", "BWWWWWWWB   ", "BWWWWWWWWB  ", "BWWWWWWWWWB ", "BWWWWWWBBBBB",
        "BWWWBWWB    ", "BWWB BWWB   ", "BWB  BWWB   ", "BB    BWWB  ", "B     BWWB  ", "       BWWB ", "        BB  ",
    };
    for (int y = 0; y < CURSOR_H; y++)
        for (int x = 0; x < CURSOR_W; x++) {
            char c = SHAPE[y][x];
            if (c == ' ') continue;
            int px = g_mx + x, py = g_my + y;
            if (!s.clip.contains(px, py)) continue;
            s.row(py)[px] = c == 'B' ? 0xFF000000 : 0xFFFFFFFF;
        }
}

// The shadow: rings of falling opacity outside the frame, rounded like it.
void draw_shadow(Surface& s, const Rect& f) {
    for (int i = SHADOW; i >= 1; i--) {
        Rect ring = f.inset(-i).translated(0, 2);
        uint8_t a = (uint8_t)(6 + (SHADOW - i) * 4);
        // top and bottom bands, then the sides, so each pixel is blended once per ring
        blend_rect(s, {ring.x, ring.y, ring.w, 1}, rgba(0, 0, 0, a));
        blend_rect(s, {ring.x, ring.bottom() - 1, ring.w, 1}, rgba(0, 0, 0, a));
        blend_rect(s, {ring.x, ring.y + 1, 1, ring.h - 2}, rgba(0, 0, 0, a));
        blend_rect(s, {ring.right() - 1, ring.y + 1, 1, ring.h - 2}, rgba(0, 0, 0, a));
    }
}

// The scene under a window's corners is kept before the window is drawn,
// then blended back by the corner's coverage: rounded corners over any
// background, square content buffers included.
struct CornerSave {
    uint32_t px[4][RADIUS * RADIUS];
    Rect r[4];
};

void save_corners(const Surface& s, const Rect& f, CornerSave& cs) {
    cs.r[0] = {f.x, f.y, RADIUS, RADIUS};
    cs.r[1] = {f.right() - RADIUS, f.y, RADIUS, RADIUS};
    cs.r[2] = {f.x, f.bottom() - RADIUS, RADIUS, RADIUS};
    cs.r[3] = {f.right() - RADIUS, f.bottom() - RADIUS, RADIUS, RADIUS};
    for (int k = 0; k < 4; k++)
        for (int y = 0; y < RADIUS; y++)
            for (int x = 0; x < RADIUS; x++) {
                int px = cs.r[k].x + x, py = cs.r[k].y + y;
                cs.px[k][y * RADIUS + x] = s.clip.contains(px, py) ? s.row(py)[px] : 0;
            }
}

void restore_corners(Surface& s, const CornerSave& cs) {
    for (int k = 0; k < 4; k++)
        for (int y = 0; y < RADIUS; y++)
            for (int x = 0; x < RADIUS; x++) {
                int px = cs.r[k].x + x, py = cs.r[k].y + y;
                if (!s.clip.contains(px, py)) continue;
                int dx = (k & 1) ? RADIUS - 1 - x : x;
                int dy = (k & 2) ? RADIUS - 1 - y : y;
                uint8_t cov = corner_coverage(RADIUS, dx, dy);
                if (cov == 255) continue;
                uint32_t under = cs.px[k][y * RADIUS + x];
                s.row(py)[px] = cov ? mix(under, s.row(py)[px], cov) : under;
            }
}

void draw_window(Surface& s, const Win& w, bool focused) {
    Rect f = frame_rect(w);
    CornerSave* cs = nullptr;
    static CornerSave saved;
    if (decorated(w)) {
        draw_shadow(s, f);
        cs = &saved;
        save_corners(s, f, *cs);
        Rect t = title_rect(w);
        Color bar = focused ? rgb(0x2b, 0x31, 0x44) : rgb(0x1f, 0x23, 0x31);
        fill_rect(s, f, bar);
        fill_rect(s, t, bar);
    }
    // content: the client's buffer where it covers the window, dark elsewhere
    Rect content{w.x, w.y, w.w, w.h};
    if (w.px && w.bw > 0) {
        Surface buffer((uint32_t*)w.px, w.bw, w.bh, w.stride);
        Rect shown = Rect{w.x, w.y, w.bw < w.w ? w.bw : w.w, w.bh < w.h ? w.bh : w.h}.intersect(s.clip);
        if (!shown.empty()) blit(s, shown.x, shown.y, buffer.sub({shown.x - w.x, shown.y - w.y, shown.w, shown.h}));
        if (w.bw < w.w) fill_rect(s, {w.x + w.bw, w.y, w.w - w.bw, w.h}, rgb(0x10, 0x12, 0x1c));
        if (w.bh < w.h) fill_rect(s, {w.x, w.y + w.bh, w.w, w.h - w.bh}, rgb(0x10, 0x12, 0x1c));
    } else {
        fill_rect(s, content, rgb(0x10, 0x12, 0x1c));
    }
    if (decorated(w)) {
        Rect t = title_rect(w);
        stroke_rect_rounded(s, f, RADIUS, focused ? rgb(0x5a, 0xa2, 0xff) : rgb(0x3a, 0x40, 0x55));
        draw_text_ellipsis(s, g_bold, t.x + 12, t.y + 6, w.title, w.w - CLOSE_W - 20,
                           focused ? rgb(0xf1, 0xf3, 0xf9) : rgb(0xaa, 0xb1, 0xc5));
        Rect c = close_rect(w);
        int cx = c.x + c.w / 2, cy = c.y + c.h / 2;
        Color x = focused ? rgb(0xf1, 0xf3, 0xf9) : rgb(0x8a, 0x91, 0xa5);
        draw_line_aa(s, cx - 5, cy - 5, cx + 5, cy + 5, 20, x);
        draw_line_aa(s, cx + 5, cy - 5, cx - 5, cy + 5, 20, x);
        restore_corners(s, *cs);
    }
}

void compose_rect(const Rect& r) {
    Surface s = g_back;
    s.clip = r;
    Surface wsub = g_wall.sub(r);
    blit(s, r.x, r.y, wsub);
    for (int i = 0; i < g_norder; i++) {
        const Win& w = g_wins[g_order[i]];
        if (!outer_rect(w).overlaps(r)) continue;
        draw_window(s, w, g_order[i] == g_focus);
    }
    if (cursor_rect().overlaps(r)) draw_cursor(s);
}

void present_rect(const Rect& r) {
    for (int y = r.y; y < r.bottom(); y++) {
        const uint32_t* src = g_back.row(y) + r.x;
        uint32_t* dst = g_fb + (size_t)y * g_fb_stride + r.x;
        memcpy(dst, src, (size_t)r.w * 4);
    }
}

void frame() {
    g_frame_armed = false;
    long px = 0;
    if (g_dmg_all) {
        Rect all{0, 0, g_W, g_H};
        compose_rect(all);
        present_rect(all);
        px = (long)g_W * g_H;
    } else {
        for (int i = 0; i < g_ndmg; i++) {
            compose_rect(g_dmg[i]);
            present_rect(g_dmg[i]);
            px += (long)g_dmg[i].w * g_dmg[i].h;
        }
    }
    g_dmg_all = false;
    g_ndmg = 0;
    g_frames++;
    if (g_commit_frame) {
        g_last_commit_px = px;
        g_commit_frame = false;
    }
    for (int i = 0; i < MAX_WINS; i++)
        if (g_wins[i].used && g_wins[i].want_frame) {
            g_wins[i].want_frame = false;
            send_header(g_wins[i].client, FRAME, g_wins[i].id);
        }
}

void arm_frame() {
    if (g_frame_armed) return;
    struct event e{};
    e.events = EVENT_TIMER;
    e.timeout_ms = FRAME_MS;
    e.data = 5;                     // the frame timer (1-4 are the other watches)
    event_ctl(g_ev, EVENT_DEL, EVENT_FD_TIMER, nullptr);
    if (event_ctl(g_ev, EVENT_ADD, EVENT_FD_TIMER, &e) == 0) g_frame_armed = true;
}

// --------------------------------------------------------------- input --
void snap(Win& w, int side) {
    // side: 0 left half, 1 right half, 2 maximise, 3 restore
    if (side == 3) {
        if (!w.snapped) return;
        w.snapped = false;
        set_geometry(w, w.saved.x, w.saved.y, w.saved.w, w.saved.h);
        if (g_log) printf("pane: window %u restored: %dx%d at %d,%d\n", w.id, w.w, w.h, w.x, w.y);
        return;
    }
    if (!w.snapped) w.saved = {w.x, w.y, w.w, w.h};
    w.snapped = true;
    int top = TITLE_H + BORDER + 4;
    int usable_h = g_H - top - BORDER - 4;
    if (side == 2) set_geometry(w, BORDER + 4, top, g_W - 2 * (BORDER + 4), usable_h);
    else if (side == 0) set_geometry(w, BORDER + 4, top, g_W / 2 - 2 * (BORDER + 4), usable_h);
    else set_geometry(w, g_W / 2 + BORDER + 4, top, g_W / 2 - 2 * (BORDER + 4), usable_h);
    if (g_log)
        printf("pane: window %u snapped %s: %dx%d at %d,%d\n", w.id, side == 2 ? "maximised" : side == 0 ? "left" : "right",
               w.w, w.h, w.x, w.y);
}

void move_pointer(int nx, int ny) {
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > g_W - 1) nx = g_W - 1;
    if (ny > g_H - 1) ny = g_H - 1;
    if (nx == g_mx && ny == g_my) return;
    add_damage(cursor_rect());
    g_mx = nx;
    g_my = ny;
    add_damage(cursor_rect());
    if (g_resize >= 0 && g_wins[g_resize].used) {
        Win& w = g_wins[g_resize];
        Rect r = g_resize_start;
        int dx = g_mx - g_resize_mx, dy = g_my - g_resize_my;
        if (g_resize_edges & 1) { r.x += dx; r.w -= dx; }
        if (g_resize_edges & 2) r.w += dx;
        if (g_resize_edges & 4) { r.y += dy; r.h -= dy; }
        if (g_resize_edges & 8) r.h += dy;
        if (r.w < MIN_W) { if (g_resize_edges & 1) r.x -= MIN_W - r.w; r.w = MIN_W; }
        if (r.h < MIN_H) { if (g_resize_edges & 4) r.y -= MIN_H - r.h; r.h = MIN_H; }
        if (r.x != w.x || r.y != w.y || r.w != w.w || r.h != w.h) {
            add_damage(outer_rect(w));
            w.x = r.x;
            w.y = r.y;
            w.w = r.w;
            w.h = r.h;
            add_damage(outer_rect(w));
        }
        return;
    }
    if (g_drag >= 0 && g_wins[g_drag].used) {
        Win& w = g_wins[g_drag];
        add_damage(outer_rect(w));
        w.x = g_mx - g_drag_dx;
        w.y = g_my - g_drag_dy;
        if (w.y < TITLE_H + BORDER) w.y = TITLE_H + BORDER;
        add_damage(outer_rect(w));
        return;
    }
    int under = window_at(g_mx, g_my, nullptr, nullptr, nullptr);
    if (under != g_hover) {
        if (g_hover >= 0 && g_wins[g_hover].used) send_header(g_wins[g_hover].client, POINTER_LEAVE, g_wins[g_hover].id);
        g_hover = under;
        if (under >= 0) send_pointer(g_wins[under], POINTER_ENTER, g_mx, g_my, 0, 0, 0);
    }
    if (under >= 0 && Rect{g_wins[under].x, g_wins[under].y, g_wins[under].w, g_wins[under].h}.contains(g_mx, g_my))
        send_pointer(g_wins[under], POINTER_MOTION, g_mx, g_my, 0, 0, 0);
}

void button(uint32_t b, bool pressed) {
    bool in_title = false, in_close = false;
    int edges = 0;
    int under = window_at(g_mx, g_my, &in_title, &in_close, &edges);
    if (pressed) {
        if (under >= 0) {
            raise_window(under);
            set_focus(under);
            if (b == 0 && in_close) {
                send_header(g_wins[under].client, CLOSE_REQUEST, g_wins[under].id);
                return;
            }
            if (b == 0 && edges) {
                Win& w = g_wins[under];
                g_resize = under;
                g_resize_edges = edges;
                g_resize_start = {w.x, w.y, w.w, w.h};
                g_resize_mx = g_mx;
                g_resize_my = g_my;
                w.snapped = false;
                return;
            }
            if (b == 0 && in_title) {
                Win& w = g_wins[under];
                if (w.snapped) {
                    // dragging a snapped window gives it its old size back under the pointer
                    int rel = w.w ? (g_mx - w.x) * w.saved.w / w.w : 0;
                    w.snapped = false;
                    set_geometry(w, g_mx - rel, g_my + TITLE_H / 2, w.saved.w, w.saved.h);
                }
                g_drag = under;
                g_drag_dx = g_mx - w.x;
                g_drag_dy = g_my - w.y;
                return;
            }
        }
    } else if (b == 0 && (g_drag >= 0 || g_resize >= 0)) {
        int win = g_drag >= 0 ? g_drag : g_resize;
        if (g_wins[win].used) send_configure(g_wins[win]);
        if (g_log && g_wins[win].used)
            printf("pane: window %u %s: %dx%d at %d,%d\n", g_wins[win].id, g_resize >= 0 ? "resized" : "moved",
                   g_wins[win].w, g_wins[win].h, g_wins[win].x, g_wins[win].y);
        g_drag = g_resize = -1;
        return;
    }
    if (under >= 0 && !in_title && !edges) send_pointer(g_wins[under], POINTER_BUTTON, g_mx, g_my, b, pressed, 0);
}

void read_mouse() {
    struct input_event ev[32];
    long n = read(g_mouse, ev, sizeof ev);
    if (n <= 0) return;
    int count = (int)(n / (long)sizeof(input_event));
    int dx = 0, dy = 0;
    int ax = -1, ay = -1;
    auto flush_motion = [&]() {
        if (ax >= 0 || ay >= 0) {
            move_pointer(ax >= 0 ? (int)((int64_t)ax * (g_W - 1) / 65535) : g_mx,
                         ay >= 0 ? (int)((int64_t)ay * (g_H - 1) / 65535) : g_my);
            ax = ay = -1;
        } else if (dx || dy) {
            move_pointer(g_mx + dx, g_my + dy);
        }
        dx = dy = 0;
    };
    for (int i = 0; i < count; i++) {
        const input_event& e = ev[i];
        if (e.type == EV_REL && e.code == REL_X) dx += e.value;
        else if (e.type == EV_REL && e.code == REL_Y) dy += e.value;
        else if (e.type == EV_ABS && e.code == ABS_X) ax = e.value;
        else if (e.type == EV_ABS && e.code == ABS_Y) ay = e.value;
        else {
            flush_motion();
            if (e.type == EV_BUTTON) {
                uint8_t bit = (uint8_t)(1 << e.code);
                g_buttons = e.value ? (uint8_t)(g_buttons | bit) : (uint8_t)(g_buttons & ~bit);
                button((uint32_t)e.code, e.value != 0);
            } else if (e.type == EV_REL && e.code == REL_WHEEL) {
                int under = window_at(g_mx, g_my, nullptr, nullptr, nullptr);
                if (under >= 0) send_pointer(g_wins[under], POINTER_SCROLL, g_mx, g_my, 0, 0, e.value);
            }
        }
    }
    flush_motion();
}

void alt_tab() {
    if (g_norder < 2) return;
    // the window just below the top comes to the top (cycles through all)
    int next = g_order[g_norder - 2];
    raise_window(next);
    set_focus(next);
    if (g_log) printf("pane: alt-tab: window %u on top\n", g_wins[next].id);
}

void read_keyboard() {
    struct input_event ev[32];
    long n = read(g_kbd, ev, sizeof ev);
    if (n <= 0) return;
    int count = (int)(n / (long)sizeof(input_event));
    for (int i = 0; i < count; i++) {
        const input_event& e = ev[i];
        if (e.type != EV_KEY) continue;
        bool ctrl = e.mods & 2, alt = e.mods & 4, super = e.mods & 8;
        bool press = e.value == 1;
        if (press && ctrl && alt && (e.unicode == 'q' || e.unicode == 'Q' || e.unicode == 0x11)) {
            g_quit = true;          // Ctrl+Alt+Q: back to the kernel desktop
            return;
        }
        if (press && alt && e.code == KEY_F1 + 3 && g_focus >= 0) {   // Alt+F4
            send_header(g_wins[g_focus].client, CLOSE_REQUEST, g_wins[g_focus].id);
            continue;
        }
        if (press && alt && e.code == KEY_TAB) {                           // Alt+Tab
            alt_tab();
            continue;
        }
        // Super+arrows: up maximises, left/right snap, down restores
        // (the keypad arrows without Num Lock count too)
        bool keypad_arrow = !(e.mods & 32) && (e.code == 0x48 || e.code == 0x4B || e.code == 0x4D || e.code == 0x50);
        uint16_t arrow = keypad_arrow
                         ? (uint16_t)(e.code | 0x100) : e.code;
        if (press && super && g_focus >= 0 && g_wins[g_focus].used &&
            (arrow == KEY_UP || arrow == KEY_DOWN || arrow == KEY_LEFT || arrow == KEY_RIGHT)) {
            Win& w = g_wins[g_focus];
            if (arrow == KEY_LEFT) snap(w, 0);
            else if (arrow == KEY_RIGHT) snap(w, 1);
            else if (arrow == KEY_UP) snap(w, 2);
            else snap(w, 3);
            continue;
        }
        if (g_focus < 0 || !g_wins[g_focus].used) continue;
        Key k{};
        k.h = Header{KEY, sizeof k, 0, g_wins[g_focus].id};
        k.code = e.code;
        k.unicode = e.unicode;
        k.mods = e.mods;
        k.pressed = (uint32_t)e.value;
        send_to(g_wins[g_focus].client, &k, sizeof k);
    }
}

// ------------------------------------------------------------- clients --
int find_window(int ci, uint32_t id) {
    if (id == 0) return -1;
    for (int i = 0; i < MAX_WINS; i++)
        if (g_wins[i].used && g_wins[i].id == id && g_wins[i].client == ci) return i;
    return -1;
}

void place_new_window(Win& w) {
    int n = g_norder;
    w.x = 80 + (n % 8) * 32;
    w.y = TITLE_H + 60 + (n % 8) * 32;
    if (w.x + w.w > g_W) w.x = g_W > w.w ? (g_W - w.w) / 2 : 0;
    if (w.y + w.h > g_H) w.y = g_H > w.h ? (g_H - w.h) / 2 : TITLE_H;
    if (w.y < TITLE_H + BORDER) w.y = TITLE_H + BORDER;
}

void handle_message(int ci, const Message& m, uint32_t len, int fd) {
    Client& c = g_clients[ci];
    if (len < sizeof(Header) || m.h.length != len || expected_length(m.h.type) != len) {
        if (g_test) printf("pane: client %u sent a bad message (type %u, %u bytes): disconnected\n", c.id, m.h.type, len);
        if (fd >= 0) close(fd);
        drop_client(ci);
        return;
    }
    if (!c.connected && m.h.type != CONNECT) {
        if (fd >= 0) close(fd);
        drop_client(ci);
        return;
    }
    switch (m.h.type) {
    case CONNECT: {
        if (m.connect.version != VERSION) {
            send_error(ci, 0, ERR_PROTOCOL);
            drop_client(ci);
            break;
        }
        memcpy(c.name, m.connect.name, MAX_NAME);
        c.name[MAX_NAME - 1] = 0;
        c.connected = true;
        Connected r{};
        r.h = Header{CONNECTED, sizeof r, 0, 0};
        r.client_id = c.id;
        r.screen_w = (uint32_t)g_W;
        r.screen_h = (uint32_t)g_H;
        r.scale = 100;
        send_to(ci, &r, sizeof r);
        if (g_test) printf("pane: client connected: %s (id %u)\n", c.name, c.id);
        break;
    }
    case CREATE_WINDOW: {
        int count = 0, slot = -1;
        for (int i = 0; i < MAX_WINS; i++) {
            if (g_wins[i].used && g_wins[i].client == ci) count++;
            if (!g_wins[i].used && slot < 0) slot = i;
        }
        if (slot < 0 || count >= (int)MAX_WINDOWS_PER_CLIENT || m.create.width < 1 || m.create.height < 1 ||
            m.create.width > 8192 || m.create.height > 8192) {
            send_error(ci, 0, ERR_LIMIT);
            break;
        }
        Win& w = g_wins[slot];
        w = Win{};
        w.used = true;
        w.client = ci;
        w.id = g_next_window++;
        w.w = (int)m.create.width;
        w.h = (int)m.create.height;
        w.flags = m.create.flags & (WIN_DECORATED | WIN_RESIZABLE);
        memcpy(w.title, m.create.title, MAX_TITLE);
        w.title[MAX_TITLE - 1] = 0;
        place_new_window(w);
        g_order[g_norder++] = slot;
        Header h{WINDOW_CREATED, sizeof h, m.h.serial, w.id};      // the request's serial, for matching
        port_try_send(c.ch, &h, sizeof h, nullptr, 0);
        send_configure(w);
        set_focus(slot);
        add_damage(outer_rect(w));
        if (g_test) printf("pane: window %u created for %s: %dx%d at %d,%d\n", w.id, c.name, w.w, w.h, w.x, w.y);
        break;
    }
    case ATTACH_BUFFER: {
        int wi = find_window(ci, m.h.window);
        if (wi < 0 || fd < 0) {
            if (fd >= 0) close(fd);
            send_error(ci, m.h.window, wi < 0 ? ERR_NO_WINDOW : ERR_BUFFER);
            break;
        }
        Win& w = g_wins[wi];
        uint32_t bw = m.attach.width, bh = m.attach.height, stride = m.attach.stride;
        if (m.attach.format != 0 || bw < 1 || bh < 1 || bw > 8192 || bh > 8192 || stride < bw || stride > 8192) {
            close(fd);
            send_error(ci, w.id, ERR_BUFFER);
            break;
        }
        size_t need = (size_t)stride * bh * 4;
        void* px = shm_map(fd, PROT_READ);
        if (px == MAP_FAILED) {
            close(fd);
            send_error(ci, w.id, ERR_BUFFER);
            break;
        }
        // The block must be at least as large as the shape claims: a short
        // one would have Pane read past its end.
        struct stat st;
        if (fstat(fd, &st) < 0 || (size_t)st.st_size < need) {
            shm_unmap(px);
            close(fd);
            send_error(ci, w.id, ERR_BUFFER);
            break;
        }
        if (g_log && w.px) printf("pane: window %u has a new %ux%u buffer\n", w.id, bw, bh);
        if (w.px) shm_unmap(w.px);
        if (w.shm >= 0) close(w.shm);
        w.px = (uint32_t*)px;
        w.bytes = need;
        w.bw = (int)bw;
        w.bh = (int)bh;
        w.stride = (int)stride;
        w.shm = fd;
        break;
    }
    case DAMAGE: {
        int wi = find_window(ci, m.h.window);
        if (wi < 0) {
            send_error(ci, m.h.window, ERR_NO_WINDOW);
            break;
        }
        Win& w = g_wins[wi];
        Rect r = Rect{m.damage.x, m.damage.y, m.damage.width, m.damage.height}.intersect({0, 0, w.w, w.h});
        if (r.empty()) break;
        w.pending = w.pending.empty() ? r : w.pending.unite(r);
        break;
    }
    case COMMIT: {
        int wi = find_window(ci, m.h.window);
        if (wi < 0) {
            send_error(ci, m.h.window, ERR_NO_WINDOW);
            break;
        }
        Win& w = g_wins[wi];
        if (!w.pending.empty()) add_damage(w.pending.translated(w.x, w.y));
        else if (!w.ever_committed) add_damage(Rect{w.x, w.y, w.w, w.h});
        w.pending = {0, 0, 0, 0};
        w.ever_committed = true;
        w.want_frame = true;
        g_commit_frame = true;
        break;
    }
    case SET_TITLE: {
        int wi = find_window(ci, m.h.window);
        if (wi < 0) break;
        memcpy(g_wins[wi].title, m.title.title, MAX_TITLE);
        g_wins[wi].title[MAX_TITLE - 1] = 0;
        add_damage(title_rect(g_wins[wi]));
        break;
    }
    case SET_GEOMETRY: {
        int wi = find_window(ci, m.h.window);
        if (wi < 0) break;
        Win& w = g_wins[wi];
        int nw = m.geometry.width > 0 ? m.geometry.width : w.w;
        int nh = m.geometry.height > 0 ? m.geometry.height : w.h;
        if (nw > 8192 || nh > 8192) break;
        set_geometry(w, m.geometry.x, m.geometry.y, nw, nh);
        break;
    }
    case RAISE: {
        int wi = find_window(ci, m.h.window);
        if (wi >= 0) raise_window(wi);
        break;
    }
    case DESTROY_WINDOW: {
        int wi = find_window(ci, m.h.window);
        if (wi >= 0) destroy_window(wi);
        break;
    }
    default:
        // a server-to-client type from a client: not ours to accept
        send_error(ci, 0, ERR_PROTOCOL);
        drop_client(ci);
        break;
    }
    if (fd >= 0 && m.h.type != ATTACH_BUFFER) close(fd);
}

void accept_client() {
    int fds[1];
    int nfds = 1;
    uint8_t dummy[8];
    long n = port_recv(g_listen, dummy, sizeof dummy, fds, &nfds, 0);
    if (n < 0 || nfds < 1) return;
    int ch = fds[0];
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS && slot < 0; i++)
        if (!g_clients[i].used) slot = i;
    if (slot < 0) {
        close(ch);
        return;
    }
    Client& c = g_clients[slot];
    c = Client{};
    c.used = true;
    c.ch = ch;
    c.id = g_next_client++;
    struct event e{};
    e.events = EVENT_READ;
    e.data = 0x100 + (uint64_t)slot;
    if (event_ctl(g_ev, EVENT_ADD, ch, &e) < 0) {
        close(ch);
        c = Client{};
    }
}

void client_ready(int ci, uint32_t events) {
    Client& c = g_clients[ci];
    if (!c.used) return;
    if (events & EVENT_READ) {
        for (int k = 0; k < 8 && c.used; k++) {       // a few messages per wake-up, then others get a turn
            Message m;
            int fds[1];
            int nfds = 1;
            long n = port_recv(c.ch, &m, sizeof m, fds, &nfds, 0);
            if (n < 0 && (errno == EAGAIN || errno == ETIMEDOUT)) break;     // nothing more queued
            if (n < 0) {
                drop_client(ci);
                return;
            }
            handle_message(ci, m, (uint32_t)n, nfds ? fds[0] : -1);
        }
    }
    if ((events & EVENT_HUP) && c.used) drop_client(ci);
}

// --------------------------------------------------------------- setup --
bool open_screen() {
    g_fbfd = open("/dev/fb0", O_RDWR);
    if (g_fbfd < 0) {
        printf("pane: cannot open /dev/fb0: %s\n", strerror(errno));
        return false;
    }
    struct fb_info info;
    if (ioctl(g_fbfd, FBIO_GET_INFO, &info) < 0 || info.bpp != 32) {
        printf("pane: /dev/fb0 is not a 32-bit screen\n");
        return false;
    }
    size_t bytes = (size_t)info.pitch * info.height;
    void* fb = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, g_fbfd, 0);
    if (fb == MAP_FAILED) {
        printf("pane: cannot map the screen: %s\n", strerror(errno));
        return false;
    }
    g_W = (int)info.width;
    g_H = (int)info.height;
    g_fb = (uint32_t*)fb;
    g_fb_stride = (int)(info.pitch / 4);
    size_t px = (size_t)g_W * g_H;
    uint32_t* back = (uint32_t*)malloc(px * 4);
    uint32_t* wall = (uint32_t*)malloc(px * 4);
    if (!back || !wall) {
        printf("pane: out of memory for %dx%d\n", g_W, g_H);
        return false;
    }
    g_back = Surface(back, g_W, g_H, g_W);
    g_wall = Surface(wall, g_W, g_H, g_W);
    fill_gradient_vertical(g_wall, g_wall.bounds(), rgb(0x0c, 0x12, 0x2e), rgb(0x2a, 0x14, 0x4a));
    draw_text(g_wall, g_font, g_W - 230, g_H - 30, "Cerberus  |  Pane (phase 12)", rgba(255, 255, 255, 110));
    g_mx = g_W / 2;
    g_my = g_H / 2;
    printf("pane: screen %dx%d, pitch %u, mapped shared\n", g_W, g_H, info.pitch);
    return true;
}

bool open_input() {
    g_kbd = open("/dev/input/kbd0", O_RDONLY);
    g_mouse = open("/dev/input/mouse0", O_RDONLY);
    if (g_kbd < 0 || g_mouse < 0) {
        printf("pane: cannot open the input devices: %s\n", strerror(errno));
        return false;
    }
    return true;
}

bool load_fonts() {
    size_t size = (size_t)(_binary_fonts_aa_end - _binary_fonts_aa_start);
    g_font = font_from_blob(_binary_fonts_aa_start, size, "ui");
    g_bold = font_from_blob(_binary_fonts_aa_start, size, "ui-bold");
    return g_font.valid() && g_bold.valid();
}

int start_test_client() {
    int pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        const char* argv[] = {"/bin/panetest", g_demo ? "demo" : nullptr, nullptr};
        const char* envp[] = {nullptr};
        execve("/bin/panetest", (char* const*)argv, (char* const*)envp);
        exit(127);
    }
    return pid;
}

bool verify_test() {
    if (g_frames < 2) {
        printf("pane: FAIL: only %ld frame(s)\n", g_frames);
        return false;
    }
    if (g_last_commit_px != 100) {
        printf("pane: FAIL: the 10x10 damage recomposited %ld px, not 100\n", g_last_commit_px);
        return false;
    }
    printf("pane: 10x10 damage recomposited %ld px\n", g_last_commit_px);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    g_test = argc > 1 && strcmp(argv[1], "--test") == 0;
    g_demo = argc > 1 && strcmp(argv[1], "--demo") == 0;
    g_log = g_test || g_demo;
    if (!load_fonts()) {
        printf("pane: the fonts blob is damaged\n");
        return 1;
    }
    if (!open_screen() || !open_input()) return 1;
    g_listen = port_create("pane", 0666);
    if (g_listen < 0) {
        printf("pane: cannot create the port: %s (is another pane running?)\n", strerror(errno));
        return 1;
    }
    g_ev = event_create(0);
    if (g_ev < 0) return 1;
    struct event e{};
    e.events = EVENT_READ;
    e.data = 1;
    event_ctl(g_ev, EVENT_ADD, g_listen, &e);
    e.data = 2;
    event_ctl(g_ev, EVENT_ADD, g_kbd, &e);
    e.data = 3;
    event_ctl(g_ev, EVENT_ADD, g_mouse, &e);
    bool content_checked = false;
    if (g_test || g_demo) {
        e.events = EVENT_CHILD;
        e.data = 4;
        event_ctl(g_ev, EVENT_ADD, EVENT_FD_CHILD, &e);
        if (start_test_client() < 0) {
            printf("pane: cannot start /bin/panetest\n");
            return 1;
        }
    }
    g_dmg_all = true;
    arm_frame();

    while (!g_quit) {
        struct event out[16];
        int n = event_wait(g_ev, out, 16, 1000);
        if (n < 0 && errno != EINTR) break;
        for (int i = 0; i < n; i++) {
            uint64_t d = out[i].data;
            if (d == 1) accept_client();
            else if (d == 2) read_keyboard();
            else if (d == 3) read_mouse();
            else if (d == 4) {
                int status = 0;
                while (waitpid(-1, &status, 1) > 0) {}
                if (g_test) {
                    if (g_ndmg || g_dmg_all) frame();
                    bool ok = verify_test() && content_checked;
                    if (!content_checked) printf("pane: FAIL: the window's content was never seen on screen\n");
                    printf("pane: %s\n", ok ? "PASS" : "FAIL");
                    return ok ? 0 : 1;
                }
            } else if (d == 5 || (out[i].events & EVENT_TIMER)) {
                frame();
            } else if (d >= 0x100 && d < 0x100 + MAX_CLIENTS) {
                client_ready((int)(d - 0x100), out[i].events);
            }
        }
        if (g_test && !content_checked) {
            // after the red square's frame, look at the composed scene
            for (int i = 0; i < MAX_WINS; i++) {
                const Win& w = g_wins[i];
                if (!w.used || !w.ever_committed || !w.px) continue;
                if (g_last_commit_px == 100) {
                    uint32_t blue = g_back.row(w.y + 100)[w.x + 100] & 0xFFFFFF;
                    uint32_t red = g_back.row(w.y + 25)[w.x + 25] & 0xFFFFFF;
                    if (blue == 0x2040C0 && red == 0xE03030) {
                        printf("pane: content verified on screen at %d,%d\n", w.x, w.y);
                        content_checked = true;
                    } else {
                        printf("pane: FAIL: expected blue 2040c0 and red e03030, saw %06x and %06x\n", blue, red);
                    }
                }
            }
        }
        if (g_test && g_frames == 1 && g_last_commit_px > 0 && !content_checked)
            printf("pane: full commit composited %ld px\n", g_last_commit_px);
        if ((g_ndmg || g_dmg_all) && !g_frame_armed) arm_frame();
    }
    for (int i = 0; i < MAX_CLIENTS; i++) drop_client(i);
    printf("pane: %ld frames, bye\n", g_frames);
    return 0;
}
