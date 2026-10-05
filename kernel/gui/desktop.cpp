// Desktop compositor (kernel-hosted preview of Pane, SPEC §9/§11).
//
// Rendering model: a wallpaper surface, a back buffer, and the framebuffer.
// Every change records a damage rectangle; a frame recomposites only the
// damaged rectangles (wallpaper, then windows bottom-to-top with shadows and
// decorations, then the launcher menu and the panel) into the back buffer and
// copies just those rectangles to the framebuffer. A copy of what the screen
// shows is kept in RAM (`front`), and only pixels that differ from it are
// written: video memory is slow to write, very slow under some hypervisors,
// and a moving window with a plain background changes far fewer pixels than
// it covers. The cursor is blended from the back buffer straight onto the
// screen (never reading video memory) and restored the same way, so moving
// the mouse never triggers recomposition.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpuid.h>
#include <boot/bootinfo.h>
#include <arch/x86_64/power.h>
#include <drivers/bga.h>
#include <drivers/fbconsole.h>
#include <fs/vfs.h>
#include <drivers/lapic.h>
#include <arch/x86_64/smp.h>
#include <drivers/ps2kbd.h>
#include <lib/console.h>
#include <drivers/ps2mouse.h>
#include <drivers/refclock.h>
#include <drivers/rtc.h>
#include <gfx/gfx.h>
#include <gui/desktop.h>
#include <gui/terminal.h>
#include <lib/kprintf.h>
#include <lib/panic.h>
#include <lib/string.h>
#include <lib/version.h>
#include <drivers/ps2.h>
#include <mm/kheap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <sched/sched.h>

extern "C" const u8 _binary_fonts_aa_start[];
extern "C" const u8 _binary_fonts_aa_end[];

using namespace gfx;

namespace {

// ---------------------------------------------------------------- theme --
namespace theme {
constexpr Color WALL_TOP = rgb(11, 17, 32);
constexpr Color WALL_BOTTOM = rgb(26, 20, 51);
constexpr Color GLOW_BLUE = rgba(59, 130, 246, 110);
constexpr Color GLOW_VIOLET = rgba(139, 92, 246, 90);
constexpr Color SURFACE = rgb(30, 33, 44);
constexpr Color SURFACE_INACTIVE = rgb(26, 28, 38);
constexpr Color TITLE_TEXT = rgb(226, 232, 240);
constexpr Color TEXT = rgb(226, 232, 240);
constexpr Color TEXT_MUTED = rgb(148, 163, 184);
constexpr Color ACCENT = rgb(96, 165, 250);
constexpr Color ACCENT_DARK = rgb(37, 99, 235);
constexpr Color BORDER_FOCUS = rgba(96, 165, 250, 210);
constexpr Color BORDER = rgba(90, 96, 120, 200);
constexpr Color PANEL = rgba(14, 17, 27, 232);
constexpr Color PANEL_LINE = rgba(255, 255, 255, 22);
constexpr Color BTN_HOVER = rgba(255, 255, 255, 26);
constexpr Color BTN_CLOSE_HOVER = rgb(196, 43, 28);
constexpr Color BTN_GLYPH = rgb(226, 232, 240);
constexpr Color BTN_GLYPH_INACTIVE = rgb(120, 128, 146);
constexpr Color DOT_APP = rgb(34, 197, 94);
constexpr Color TASK_BG = rgba(255, 255, 255, 18);
constexpr Color TASK_FOCUS = rgba(96, 165, 250, 60);
constexpr Color MENU_BG = rgba(24, 27, 38, 245);
constexpr Color MENU_HOVER = rgba(96, 165, 250, 70);
constexpr Color SHADOW = rgba(0, 0, 0, 150);
constexpr Color CONTENT_BG = rgb(22, 24, 33);

constexpr int TITLE_H = 32;
constexpr int BORDER_W = 1;
constexpr int RADIUS = 12;
constexpr int SHADOW_PAD = 22;
constexpr int SHADOW_OFF = 5;
constexpr int SHADOW_BLUR = 9;
constexpr int PANEL_H = 56;         // the strip of screen kept for the taskbar
constexpr int BAR_H = 46;           // the bar itself when it floats
constexpr int RESIZE_BAND = 7;      // how far outside a window's edge the mouse still grabs it
constexpr int RESIZE_INSIDE = 4;    // and how far inside
constexpr int RESIZE_CORNER = 20;   // this close to a corner along an edge, both edges move
constexpr int BTN_W = 46;           // caption buttons: Windows-style, full title-bar height
constexpr int MENU_W = 330;
constexpr int MENU_ITEM_H = 38;
constexpr int MENU_TOP = 58;        // the search box sits above the list
constexpr int MENU_FOOT = 50;       // the power buttons below it
} // namespace theme

// --------------------------------------------------------------- settings --
// What can be changed in the Settings window. Kept in memory for now: there
// is nowhere per-user to store it until accounts exist (phase 15).
enum class BorderFx : u8 { None, Static, Breathing, Flashing, Rainbow, Chase, COUNT };
const char* const FX_NAMES[] = {"Off", "Static", "Breathing", "Flashing", "Rainbow", "Chase"};

struct AccentChoice {
    const char* name;
    Color c;
};
const AccentChoice ACCENTS[] = {
    {"Blue", rgb(96, 165, 250)},   {"Violet", rgb(167, 139, 250)}, {"Pink", rgb(244, 114, 182)},
    {"Red", rgb(248, 113, 113)},   {"Orange", rgb(251, 146, 60)},  {"Gold", rgb(250, 204, 21)},
    {"Green", rgb(74, 222, 128)},  {"Teal", rgb(45, 212, 191)},    {"Silver", rgb(226, 232, 240)},
};
constexpr int ACCENT_COUNT = sizeof ACCENTS / sizeof ACCENTS[0];
constexpr int WALLPAPER_COUNT = 3;
const char* const WALLPAPER_NAMES[WALLPAPER_COUNT] = {"Nebula", "Aurora", "Ember"};
constexpr int MAX_RADIUS = 16;

struct Prefs {
    int accent = 0;
    int wallpaper = 0;
    int radius = 12;                // window corners: 0 square, 6 soft, 12 round
    bool panel_glass = true;        // the taskbar lets the wallpaper through
    bool panel_floating = true;     // a rounded bar clear of the screen edges, or edge to edge
    bool clock_12h = false;
    bool clock_seconds = true;
    BorderFx fx = BorderFx::None;
    int border_color = -1;          // -1: the accent colour; else an index into ACCENTS
    int border_w = 2;               // pixels, 1..4
    int speed = 1;                  // 0 slow, 1 normal, 2 fast
    bool border_all = false;        // every window, or only the focused one
    bool glow = true;
};
Prefs g_prefs;

inline Color accent() { return ACCENTS[g_prefs.accent].c; }
inline Color accent_dark() {
    Color c = accent();
    return rgb((u8)(red_of(c) * 5 / 9), (u8)(green_of(c) * 5 / 9), (u8)(blue_of(c) * 5 / 9));
}
inline int radius() { return g_prefs.radius; }
inline bool fx_animated() { return g_prefs.fx >= BorderFx::Breathing; }

struct Mode {
    int w, h;
};
const Mode MODES[] = {{800, 600},  {1024, 768}, {1280, 720},  {1280, 800}, {1366, 768},
                      {1440, 900}, {1600, 900}, {1680, 1050}, {1920, 1080}};
constexpr int MODE_COUNT = sizeof MODES / sizeof MODES[0];

// Minimum time between frames. Input wakes the compositor at once, so a drag
// is drawn as soon as the mouse reports (at most ~160 frames per second).
constexpr u64 FRAME_US = 6000;
constexpr int MAX_WINDOWS = 12;
constexpr int MAX_DAMAGE = 24;

enum class Kind { Terminal, About, SystemMonitor, MemoryMap, Settings };

struct Window {
    bool used = false;
    u32 id = 0;
    Kind kind = Kind::About;
    char title[48] = {};
    Rect frame{0, 0, 0, 0};
    Rect restore{0, 0, 0, 0};
    bool maximised = false, minimised = false;
    u32* pixels = nullptr;
    Surface content;            // full screen-sized buffer; view via content_view()
    u32* shadow = nullptr;
    int shadow_w = 0, shadow_h = 0;
    bool needs_paint = true;
    int min_w = 240, min_h = 140;
};

// The applications the launcher offers. The search box matches what is
// typed against the name and the keywords.
struct App {
    const char* label;
    const char* keywords;
    Kind kind;
    Color dot;
};
const App APPS[] = {
    {"Terminal", "shell console command prompt", Kind::Terminal, rgb(96, 165, 250)},
    {"Settings", "theme colours colors wallpaper background border rgb display resolution screen appearance",
     Kind::Settings, rgb(167, 139, 250)},
    {"System Monitor", "cpu memory ram tasks performance", Kind::SystemMonitor, rgb(74, 222, 128)},
    {"Memory Map", "physical ram regions", Kind::MemoryMap, rgb(251, 146, 60)},
    {"About Cerberus", "version information", Kind::About, rgb(244, 114, 182)},
};
constexpr int APP_COUNT = sizeof APPS / sizeof APPS[0];
// Launcher items that are not applications.
constexpr int ITEM_NONE = -1, ITEM_POWER = -2, ITEM_RESTART = -3;

char g_search[24];                  // what has been typed into the launcher
int g_search_len = 0;
int g_found[APP_COUNT];             // indices into APPS that match it
int g_found_count = 0;

enum class Drag { None, Move, Resize };

struct State {
    bool active = false;
    int W = 0, H = 0;
    Surface fb, back, wall, scratch;
    u32* front = nullptr;       // what the framebuffer shows, cursor included
    // Anti-aliased fonts (kernel/gfx/fonts.bin): interface text, bold for
    // headings, fixed-width for the terminal and tables, large for wordmarks.
    Font font, bold, mono, display, display_big;

    Window windows[MAX_WINDOWS];
    int order[MAX_WINDOWS];
    int order_count = 0;
    int focus = -1;
    u32 next_id = 1;

    int mx = 0, my = 0;
    u8 buttons = 0;
    bool cursor_drawn = false;
    Rect cursor_drawn_rect{0, 0, 0, 0};

    Rect damage[MAX_DAMAGE];
    int damage_count = 0;
    bool damage_all = false;

    Drag drag = Drag::None;
    int drag_win = -1;
    int drag_sx = 0, drag_sy = 0;
    Rect drag_start{0, 0, 0, 0};
    u8 drag_edges = 0;          // 1 left, 2 right, 4 top, 8 bottom

    bool menu_open = false;
    bool super_down = false;    // Super is held...
    bool super_chord = false;   // ...and another key was pressed with it
    int menu_hover = -1;
    int press_task = -1;

    Rect task_rects[MAX_WINDOWS];
    int task_win[MAX_WINDOWS];
    int task_count = 0;

    Terminal term;
    u8* term_cells = nullptr;
    int term_win = -1;
    bool term_dirty = false;
    char term_in[256];
    volatile usize term_in_head = 0, term_in_tail = 0;

    u8 last_second = 255;
    u64 last_frame_us = 0;
    u64 last_paint_us = 0;
    GuiStats stats{};
} g;

// ------------------------------------------------------------- cursor art --
// Every shape lives in a 19x19 box. The arrow points with its top-left
// pixel; the four resize shapes (drawn by build_cursor) with their centre.
enum CursorShape { CUR_ARROW, CUR_RESIZE_H, CUR_RESIZE_V, CUR_RESIZE_NWSE, CUR_RESIZE_NESW, CUR_COUNT };
constexpr int CURSOR_W = 19, CURSOR_H = 19;
constexpr int CURSOR_ART_W = 12;
const char* const CURSOR_ART[CURSOR_H] = {
    "#           ",
    "##          ",
    "#o#         ",
    "#oo#        ",
    "#ooo#       ",
    "#oooo#      ",
    "#ooooo#     ",
    "#oooooo#    ",
    "#ooooooo#   ",
    "#oooooooo#  ",
    "#ooooooooo# ",
    "#oooooo#####",
    "#ooo#oo#    ",
    "#oo# #oo#   ",
    "#o#  #oo#   ",
    "##    #oo#  ",
    "#     #oo#  ",
    "       ##   ",
    "            ",
};
u32 g_cursor_px[CUR_COUNT][CURSOR_W * CURSOR_H];
int g_cursor_shape = CUR_ARROW;
inline int cursor_hot() { return g_cursor_shape == CUR_ARROW ? 0 : CURSOR_W / 2; }

// The launcher's mark: a "C" drawn as an open ring with a dot in its mouth.
// White with anti-aliased coverage in the alpha channel; built once.
constexpr int LOGO = 28;
u32 g_logo_px[LOGO * LOGO];
constexpr int THUMB_W = 128, THUMB_H = 80;
u32 g_thumb_px[WALLPAPER_COUNT][THUMB_W * THUMB_H];

// Controls of the Settings window, recorded as it is painted so a click can
// be matched to what it hit.
enum : u8 {
    ACT_TAB, ACT_ACCENT, ACT_WALL, ACT_RADIUS, ACT_GLASS, ACT_FLOAT, ACT_CLOCK12, ACT_SECONDS,
    ACT_FX, ACT_BCOLOR, ACT_BWIDTH, ACT_SPEED, ACT_BALL, ACT_GLOW, ACT_MODE,
};
struct Ctl {
    Rect r;
    u8 act;
    i16 val;
};
constexpr int MAX_CTLS = 96;
Ctl g_ctls[MAX_CTLS];
int g_ctl_count = 0;
int g_settings_tab = 0;
u64 g_last_anim_us = 0;
volatile u32 g_wanted_mode = 0;     // width << 16 | height, asked for by another thread; 0 = nothing

// ------------------------------------------------------------ allocation --
// Pixel buffers come from the kernel heap. Buffers this size take its large
// path, which prefers contiguous frames in the direct map, so they keep the
// 2 MiB TLB entries they had when they were carved from the frame allocator.
u32* alloc_pixels(usize count) { return (u32*)kmalloc(count * 4); }

void free_pixels(u32* pixels) { kfree(pixels); }

// ---------------------------------------------------------------- damage --
void damage(const Rect& r) {
    Rect c = r.intersect({0, 0, g.W, g.H});
    if (c.empty() || g.damage_all) return;
    for (int i = 0; i < g.damage_count; i++) {
        if (g.damage[i].overlaps(c) || g.damage[i].contains(c.x, c.y)) {
            g.damage[i] = g.damage[i].unite(c);
            return;
        }
    }
    if (g.damage_count < MAX_DAMAGE) {
        g.damage[g.damage_count++] = c;
        return;
    }
    Rect all = c;
    for (int i = 0; i < g.damage_count; i++) all = all.unite(g.damage[i]);
    g.damage[0] = all;
    g.damage_count = 1;
}

void damage_all() {
    g.damage_all = true;
    g.damage_count = 0;
}

// -------------------------------------------------------------- geometry --
Rect content_rect(const Window& w) {
    return {w.frame.x + theme::BORDER_W, w.frame.y + theme::BORDER_W + theme::TITLE_H,
            w.frame.w - 2 * theme::BORDER_W, w.frame.h - 2 * theme::BORDER_W - theme::TITLE_H};
}

Rect visual_bounds(const Window& w) {
    return {w.frame.x - theme::SHADOW_PAD, w.frame.y - theme::SHADOW_PAD, w.frame.w + 2 * theme::SHADOW_PAD,
            w.frame.h + 2 * theme::SHADOW_PAD + theme::SHADOW_OFF};
}

Rect shadow_rect(const Window& w) {
    return {w.frame.x - theme::SHADOW_PAD, w.frame.y - theme::SHADOW_PAD + theme::SHADOW_OFF,
            w.frame.w + 2 * theme::SHADOW_PAD, w.frame.h + 2 * theme::SHADOW_PAD};
}

Rect panel_rect() { return {0, g.H - theme::PANEL_H, g.W, theme::PANEL_H}; }
Rect work_area() { return {0, 0, g.W, g.H - theme::PANEL_H}; }
// The bar that is drawn inside the panel strip: floating clear of the
// edges with round corners, or docked edge to edge.
Rect bar_rect() {
    if (!g_prefs.panel_floating) return panel_rect();
    return {8, g.H - theme::BAR_H - 6, g.W - 16, theme::BAR_H};
}
int bar_mid() {
    Rect b = bar_rect();
    return b.y + b.h / 2;
}
Rect launcher_rect() { return {bar_rect().x + 5, bar_mid() - 19, 40, 38}; }
Rect search_rect() { return {bar_rect().x + 53, bar_mid() - 16, 190, 32}; }
Rect clock_rect() { return {bar_rect().right() - 236, g.H - theme::PANEL_H, 236, theme::PANEL_H}; }
Rect menu_rect() {
    int h = theme::MENU_TOP + APP_COUNT * theme::MENU_ITEM_H + 8 + theme::MENU_FOOT;
    return {8, g.H - theme::PANEL_H - h - 8, theme::MENU_W, h};
}
Rect menu_search_rect() {
    Rect mr = menu_rect();
    return {mr.x + 12, mr.y + 12, mr.w - 24, 34};
}
Rect menu_power_rect(bool restart) {
    Rect mr = menu_rect();
    Rect power{mr.right() - 12 - 104, mr.bottom() - 40, 104, 30};
    return restart ? Rect{power.x - 8 - 92, power.y, 92, 30} : power;
}
// What the launcher shows at (x, y): an index into g_found, ITEM_POWER,
// ITEM_RESTART or ITEM_NONE.
int menu_item_at(int x, int y) {
    Rect mr = menu_rect();
    if (menu_power_rect(false).contains(x, y)) return ITEM_POWER;
    if (menu_power_rect(true).contains(x, y)) return ITEM_RESTART;
    if (x < mr.x + 8 || x >= mr.right() - 8 || y < mr.y + theme::MENU_TOP) return ITEM_NONE;
    int i = (y - mr.y - theme::MENU_TOP) / theme::MENU_ITEM_H;
    return i < g_found_count ? i : ITEM_NONE;
}

bool matches(const char* text, const char* typed, int typed_len) {
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; };
    for (const char* at = text; *at; at++) {
        int i = 0;
        while (i < typed_len && at[i] && lower(at[i]) == lower(typed[i])) i++;
        if (i == typed_len) return true;
    }
    return typed_len == 0;
}

void search_update() {
    g_found_count = 0;
    for (int i = 0; i < APP_COUNT; i++)
        if (matches(APPS[i].label, g_search, g_search_len) || matches(APPS[i].keywords, g_search, g_search_len))
            g_found[g_found_count++] = i;
}
Rect cursor_rect(int x, int y) { return {x - cursor_hot(), y - cursor_hot(), CURSOR_W, CURSOR_H}; }

// Title-bar buttons, right to left: close, maximise, minimise.
Rect button_rect(const Window& w, int index) {
    return {w.frame.right() - theme::BORDER_W - (index + 1) * theme::BTN_W, w.frame.y + theme::BORDER_W, theme::BTN_W,
            theme::TITLE_H};
}

int button_at(const Window& w, int x, int y) {
    for (int i = 0; i < 3; i++)
        if (button_rect(w, i).contains(x, y)) return i;
    return -1;
}

Surface content_view(Window& w) {
    Rect cr = content_rect(w);
    return w.content.sub({0, 0, cr.w, cr.h});
}

// --------------------------------------------------------------- windows --
int window_index(const Window* w) { return (int)(w - g.windows); }

void raise_window(int idx) {
    int pos = -1;
    for (int i = 0; i < g.order_count; i++)
        if (g.order[i] == idx) pos = i;
    if (pos < 0) return;
    for (int i = pos; i < g.order_count - 1; i++) g.order[i] = g.order[i + 1];
    g.order[g.order_count - 1] = idx;
}

void set_focus(int idx) {
    if (g.focus == idx) return;
    if (g.focus >= 0 && g.windows[g.focus].used) {
        damage(visual_bounds(g.windows[g.focus]));
        g.windows[g.focus].needs_paint = true;
    }
    g.focus = idx;
    if (idx >= 0) {
        damage(visual_bounds(g.windows[idx]));
        g.windows[idx].needs_paint = true;
    }
    damage(panel_rect());
}

int top_visible_window() {
    for (int i = g.order_count - 1; i >= 0; i--) {
        Window& w = g.windows[g.order[i]];
        if (w.used && !w.minimised) return g.order[i];
    }
    return -1;
}

void terminal_fit(Window& w) {
    Rect cr = content_rect(w);
    // The cell grid is written by whoever prints (any thread, any CPU) under
    // the console lock; resizing and painting it take the same lock.
    console_lock();
    g.term.resize((cr.w - 12) / g.term.cell_w(), (cr.h - 12) / g.term.cell_h());
    console_unlock();
    w.needs_paint = true;
}

int create_window(Kind kind, const char* title, Rect frame) {
    int idx = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!g.windows[i].used) { idx = i; break; }
    if (idx < 0) return -1;
    Window& w = g.windows[idx];
    w = Window{};
    w.pixels = alloc_pixels((usize)g.W * g.H);
    if (!w.pixels) return -1;
    w.content = Surface(w.pixels, g.W, g.H, g.W);
    w.used = true;
    w.id = g.next_id++;
    w.kind = kind;
    strlcpy(w.title, title, sizeof w.title);
    w.frame = frame;
    w.needs_paint = true;
    g.order[g.order_count++] = idx;
    if (kind == Kind::Terminal) {
        g.term_win = idx;
        terminal_fit(w);
    }
    set_focus(idx);
    damage(visual_bounds(w));
    damage(panel_rect());
    return idx;
}

void destroy_window(int idx) {
    Window& w = g.windows[idx];
    if (!w.used) return;
    damage(visual_bounds(w));
    damage(panel_rect());
    free_pixels(w.pixels);
    free_pixels(w.shadow);
    w.used = false;
    for (int i = 0; i < g.order_count; i++) {
        if (g.order[i] == idx) {
            for (int j = i; j < g.order_count - 1; j++) g.order[j] = g.order[j + 1];
            g.order_count--;
            break;
        }
    }
    if (g.term_win == idx) g.term_win = -1;
    if (g.focus == idx) {
        g.focus = -1;
        set_focus(top_visible_window());
    }
}

void set_window_frame(Window& w, Rect nf) {
    if (nf.w < w.min_w) nf.w = w.min_w;
    if (nf.h < w.min_h) nf.h = w.min_h;
    // Keep the title bar reachable.
    if (nf.y < 0) nf.y = 0;
    if (nf.y > g.H - theme::PANEL_H - theme::TITLE_H) nf.y = g.H - theme::PANEL_H - theme::TITLE_H;
    if (nf.x < -nf.w + 80) nf.x = -nf.w + 80;
    if (nf.x > g.W - 80) nf.x = g.W - 80;
    if (nf.x == w.frame.x && nf.y == w.frame.y && nf.w == w.frame.w && nf.h == w.frame.h) return;
    damage(visual_bounds(w));
    bool resized = nf.w != w.frame.w || nf.h != w.frame.h;
    w.frame = nf;
    if (resized) {
        w.needs_paint = true;
        if (window_index(&w) == g.term_win) terminal_fit(w);
    }
    damage(visual_bounds(w));
}

void toggle_maximise(Window& w) {
    if (w.maximised) {
        w.maximised = false;
        set_window_frame(w, w.restore);
    } else {
        w.restore = w.frame;
        w.maximised = true;
        Rect wa = work_area();
        set_window_frame(w, {wa.x + 6, wa.y + 6, wa.w - 12, wa.h - 12});
    }
}

void minimise(Window& w) {
    w.minimised = true;
    damage(visual_bounds(w));
    damage(panel_rect());
    if (g.focus == window_index(&w)) {
        g.focus = -1;
        set_focus(top_visible_window());
    }
}

void restore(Window& w) {
    w.minimised = false;
    int idx = window_index(&w);
    raise_window(idx);
    set_focus(idx);
    damage(visual_bounds(w));
    damage(panel_rect());
}

void open_kind(Kind kind) {
    // Reuse an existing window of this kind (one terminal: one kernel shell).
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window& w = g.windows[i];
        if (w.used && w.kind == kind) {
            restore(w);
            return;
        }
    }
    int n = g.order_count;
    int x = 80 + (n % 5) * 40, y = 60 + (n % 5) * 36;
    switch (kind) {
    case Kind::Terminal: create_window(kind, "Terminal", {x, y, 720, 460}); break;
    case Kind::About: create_window(kind, "About Cerberus", {x + 120, y + 40, 460, 400}); break;
    case Kind::SystemMonitor: create_window(kind, "System Monitor", {x + 60, y + 20, 520, 360}); break;
    case Kind::MemoryMap: create_window(kind, "Memory Map", {x + 30, y + 10, 700, 480}); break;
    case Kind::Settings: {
        int ww = g.W - 60 < 700 ? g.W - 60 : 700, wh = g.H - theme::PANEL_H - 60 < 560 ? g.H - theme::PANEL_H - 60 : 560;
        int idx = create_window(kind, "Settings", {(g.W - ww) / 2, (g.H - theme::PANEL_H - wh) / 2, ww, wh});
        if (idx >= 0) {
            g.windows[idx].min_w = 620;
            g.windows[idx].min_h = 440;
        }
        break;
    }
    }
}

// ---------------------------------------------------------------- shadow --
void build_shadow(Window& w) {
    int sw = w.frame.w + 2 * theme::SHADOW_PAD, sh = w.frame.h + 2 * theme::SHADOW_PAD;
    if (w.shadow && (w.shadow_w != sw || w.shadow_h != sh)) {
        free_pixels(w.shadow);
        w.shadow = nullptr;
    }
    if (!w.shadow) {
        w.shadow = alloc_pixels((usize)sw * sh);
        if (!w.shadow) return;
        w.shadow_w = sw;
        w.shadow_h = sh;
    }
    Surface s(w.shadow, sw, sh, sw);
    memset(w.shadow, 0, (usize)sw * sh * 4);
    fill_rect_rounded(s, {theme::SHADOW_PAD, theme::SHADOW_PAD, w.frame.w, w.frame.h}, radius(), theme::SHADOW);
    blur_box(s, theme::SHADOW_BLUR, g.scratch.pixels);
    blur_box(s, theme::SHADOW_BLUR / 2 + 1, g.scratch.pixels);
}

// ---------------------------------------------------------------- paint ---
void paint_lines_header(Surface& s, const char* heading) {
    fill_rect(s, s.bounds(), theme::CONTENT_BG);
    draw_text(s, g.bold, 16, 14, heading, accent());
    draw_hline(s, 16, s.width - 17, 34, rgba(255, 255, 255, 30));
}

int paint_kv(Surface& s, int y, const char* key, const char* value) {
    draw_text(s, g.font, 16, y, key, theme::TEXT_MUTED);
    int x = 16 + 22 * 8;
    draw_text_ellipsis(s, g.font, x, y, value, s.width - x - 16, theme::TEXT);
    return y + 22;
}

void paint_about(Surface& s) {
    fill_rect(s, s.bounds(), theme::CONTENT_BG);
    fill_gradient_radial(s, s.bounds(), s.width - 60, 40, 260, with_alpha(accent(), 60), rgba(0, 0, 0, 0));
    int x = 22 + draw_text(s, g.display, 22, 12, "Cerberus", theme::TEXT);
    fill_circle_aa(s, x + 16, 12 + g.display.height * 5 / 8, 8, accent());
    draw_text(s, g.font, 26, 96, "A hybrid-kernel operating system, built from scratch.", theme::TEXT_MUTED);
    char line[96];
    int y = 130;
    ksnprintf(line, sizeof line, "%s (x86-64)", cerberus_version());
    y = paint_kv(s, y, "Version", line);
    y = paint_kv(s, y, "Built", cerberus_build_date());
    y = paint_kv(s, y, "Bootloader", g_boot_info.bootloader);
    char brand[49];
    cpuid_brand(brand);
    y = paint_kv(s, y, "CPU", brand);
    ksnprintf(line, sizeof line, "%lu cpu(s), %lu MiB RAM", (unsigned long)g_boot_info.cpu_count,
              (unsigned long)(g_boot_info.total_bytes() / MIB));
    y = paint_kv(s, y, "Machine", line);
    ksnprintf(line, sizeof line, "%dx%d, 32 bpp", g.W, g.H);
    y = paint_kv(s, y, "Display", line);
    y = paint_kv(s, y, "Language", "Spec (planned; compiler not started)");
    draw_text_ellipsis(s, g.font, 16, s.height - 30, "Alt+Tab switch, Alt+F4 close, Super menu, Super+T terminal",
                       s.width - 32, theme::TEXT_MUTED);
}

void paint_sysmon(Surface& s) {
    paint_lines_header(s, "System Monitor");
    char line[96];
    int y = 50;
    DateTime now = rtc_now();
    ksnprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u UTC", now.year, now.month, now.day, now.hour,
              now.minute, now.second);
    y = paint_kv(s, y, "Clock", line);
    u64 up = refclock_now_us() / 1000000;
    ksnprintf(line, sizeof line, "%lu:%02lu:%02lu (%lu ticks at %u Hz)", (unsigned long)(up / 3600),
              (unsigned long)(up % 3600 / 60), (unsigned long)(up % 60), (unsigned long)lapic_timer_ticks(),
              TIMER_HZ);
    y = paint_kv(s, y, "Uptime", line);
    char brand[49];
    cpuid_brand(brand);
    y = paint_kv(s, y, "CPU", brand);
    ksnprintf(line, sizeof line, "%u in use", smp_cpu_count());
    y = paint_kv(s, y, "Cores", line);

    PmmStats pm = pmm_stats();
    u64 used_mb = pm.used_frames * PAGE_SIZE / MIB, total_mb = pm.usable_frames * PAGE_SIZE / MIB;
    ksnprintf(line, sizeof line, "%lu / %lu MiB used", (unsigned long)used_mb, (unsigned long)total_mb);
    y = paint_kv(s, y, "Memory", line);
    Rect bar{16, y + 2, s.width - 32, 14};
    fill_rect_rounded(s, bar, 7, rgba(255, 255, 255, 25));
    int fillw = total_mb ? (int)((u64)bar.w * used_mb / total_mb) : 0;
    if (fillw > 0) fill_rect_rounded(s, {bar.x, bar.y, fillw < 14 ? 14 : fillw, bar.h}, 7, accent());
    y += 30;
    ksnprintf(line, sizeof line, "%lu frames, largest free run %lu MiB", (unsigned long)pm.free_frames,
              (unsigned long)(pm.largest_free_run * PAGE_SIZE / MIB));
    y = paint_kv(s, y, "Free", line);

    ksnprintf(line, sizeof line, "%lu frames, last %lu us, %lu of %lu px written", (unsigned long)g.stats.frames,
              (unsigned long)g.stats.last_frame_us, (unsigned long)g.stats.last_written_pixels,
              (unsigned long)g.stats.last_present_pixels);
    y = paint_kv(s, y, "Compositor", line);
    int open = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) open += g.windows[i].used;
    ksnprintf(line, sizeof line, "%d open, cursor %d,%d, %s", open, g.mx, g.my,
              ps2mouse_has_wheel() ? "wheel mouse" : "3-button mouse");
    y = paint_kv(s, y, "Windows", line);
    draw_text(s, g.font, 16, s.height - 30, "Refreshes every second.", theme::TEXT_MUTED);
}

void paint_memmap(Surface& s) {
    paint_lines_header(s, "Physical Memory Map (from the bootloader)");
    const BootInfo& bi = g_boot_info;
    int y = 48;
    char line[128];
    draw_text(s, g.mono, 16, y, "start            end              size       type", theme::TEXT_MUTED);
    y += 20;
    for (usize i = 0; i < bi.region_count && y < s.height - 40; i++) {
        const MemoryRegion& r = bi.regions[i];
        ksnprintf(line, sizeof line, "%016lx %016lx %8lu KiB %s", (unsigned long)r.base,
                  (unsigned long)(r.base + r.length - 1), (unsigned long)(r.length / KIB),
                  memory_type_name(r.type));
        Color c = r.type == MemoryType::Usable ? theme::TEXT : theme::TEXT_MUTED;
        draw_text(s, g.mono, 16, y, line, c);
        y += 18;
    }
    ksnprintf(line, sizeof line, "%lu regions, %lu MiB usable", (unsigned long)bi.region_count,
              (unsigned long)(bi.usable_bytes() / MIB));
    draw_text(s, g.font, 16, s.height - 30, line, accent());
}

// ------------------------------------------------------------- settings UI --
void ctl_add(const Rect& r, u8 act, int val) {
    if (g_ctl_count < MAX_CTLS) g_ctls[g_ctl_count++] = Ctl{r, act, (i16)val};
}

// A pill-shaped choice; lit when it is the current one. Wraps to the next
// row at the right edge. Advances x.
void chip(Surface& s, int& x, int& y, int left, const char* label, bool on, u8 act, int val) {
    int w = measure_text(g.font, label) + 28;
    if (x + w > s.width - 16 && x > left) {
        x = left;
        y += 38;
    }
    Rect r{x, y, w, 30};
    fill_rect_rounded(s, r, 15, on ? accent() : rgba(255, 255, 255, 20));
    draw_text(s, g.font, x + 14, y + (30 - g.font.height) / 2, label, on ? rgb(15, 23, 42) : theme::TEXT);
    ctl_add(r, act, val);
    x += w + 8;
}

int section(Surface& s, int x, int y, const char* title) {
    draw_text(s, g.bold, x, y, title, theme::TEXT);
    return y + 28;
}

// A round colour sample; ringed when chosen.
void swatch(Surface& s, int cx, int cy, Color c, bool on, u8 act, int val) {
    if (on) {
        fill_circle_aa(s, cx, cy, 16, rgb(255, 255, 255));
        fill_circle_aa(s, cx, cy, 14, theme::CONTENT_BG);
    }
    fill_circle_aa(s, cx, cy, 11, c);
    ctl_add({cx - 16, cy - 16, 32, 32}, act, val);
}

void paint_settings(Surface& s) {
    g_ctl_count = 0;
    fill_rect(s, s.bounds(), theme::CONTENT_BG);
    // Sidebar.
    constexpr int SIDE = 160;
    fill_rect(s, {0, 0, SIDE, s.height}, rgb(17, 19, 27));
    draw_vline(s, SIDE, 0, s.height - 1, rgba(255, 255, 255, 18));
    static const char* const TABS[] = {"Appearance", "Window borders", "Display"};
    for (int i = 0; i < 3; i++) {
        Rect r{8, 12 + i * 40, SIDE - 16, 34};
        if (i == g_settings_tab) {
            fill_rect_rounded(s, r, 8, rgba(255, 255, 255, 22));
            fill_rect_rounded(s, {r.x, r.y + 8, 3, r.h - 16}, 1, accent());
        }
        draw_text(s, g.font, r.x + 14, r.y + (r.h - g.font.height) / 2, TABS[i],
                  i == g_settings_tab ? theme::TEXT : theme::TEXT_MUTED);
        ctl_add(r, ACT_TAB, i);
    }
    draw_text(s, g.font, 16, s.height - 28, "Applies at once", theme::TEXT_MUTED);

    const int L = SIDE + 24;
    int x = L, y = 18;
    if (g_settings_tab == 0) {
        y = section(s, L, y, "Accent colour");
        for (int i = 0; i < ACCENT_COUNT; i++) swatch(s, L + 14 + i * 40, y + 14, ACCENTS[i].c, g_prefs.accent == i, ACT_ACCENT, i);
        y += 48;

        y = section(s, L, y, "Wallpaper");
        for (int i = 0; i < WALLPAPER_COUNT; i++) {
            Rect r{L + i * (THUMB_W + 14), y, THUMB_W, THUMB_H};
            Surface thumb(g_thumb_px[i], THUMB_W, THUMB_H, THUMB_W);
            blit(s, r.x, r.y, thumb);
            if (g_prefs.wallpaper == i) {
                stroke_rect(s, r.inset(-2), accent());
                stroke_rect(s, r.inset(-3), accent());
            } else {
                stroke_rect(s, r.inset(-1), rgba(255, 255, 255, 40));
            }
            draw_text(s, g.font, r.x + 2, r.bottom() + 6, WALLPAPER_NAMES[i],
                      g_prefs.wallpaper == i ? theme::TEXT : theme::TEXT_MUTED);
            ctl_add(r, ACT_WALL, i);
        }
        y += THUMB_H + 38;

        y = section(s, L, y, "Window corners");
        x = L;
        chip(s, x, y, L, "Square", g_prefs.radius == 0, ACT_RADIUS, 0);
        chip(s, x, y, L, "Soft", g_prefs.radius == 6, ACT_RADIUS, 6);
        chip(s, x, y, L, "Round", g_prefs.radius == 12, ACT_RADIUS, 12);
        y += 46;

        y = section(s, L, y, "Taskbar");
        x = L;
        chip(s, x, y, L, "Floating", g_prefs.panel_floating, ACT_FLOAT, 1);
        chip(s, x, y, L, "Docked", !g_prefs.panel_floating, ACT_FLOAT, 0);
        x += 16;
        chip(s, x, y, L, "Glass", g_prefs.panel_glass, ACT_GLASS, 1);
        chip(s, x, y, L, "Solid", !g_prefs.panel_glass, ACT_GLASS, 0);
        y += 46;

        y = section(s, L, y, "Clock");
        x = L;
        chip(s, x, y, L, "24-hour", !g_prefs.clock_12h, ACT_CLOCK12, 0);
        chip(s, x, y, L, "12-hour", g_prefs.clock_12h, ACT_CLOCK12, 1);
        x += 16;
        chip(s, x, y, L, "Seconds", g_prefs.clock_seconds, ACT_SECONDS, !g_prefs.clock_seconds);
    } else if (g_settings_tab == 1) {
        y = section(s, L, y, "Effect");
        for (int i = 0; i < (int)BorderFx::COUNT; i++) chip(s, x, y, L, FX_NAMES[i], (int)g_prefs.fx == i, ACT_FX, i);
        y += 46;

        y = section(s, L, y, "Colour");
        x = L;
        int row = y;
        chip(s, x, row, L, "Accent", g_prefs.border_color < 0, ACT_BCOLOR, -1);
        for (int i = 0; i < ACCENT_COUNT; i++)
            swatch(s, x + 18 + i * 38, y + 15, ACCENTS[i].c, g_prefs.border_color == i, ACT_BCOLOR, i);
        y += 40;
        draw_text(s, g.font, L, y, "Rainbow and Chase run through every colour.", theme::TEXT_MUTED);
        y += 30;

        y = section(s, L, y, "Thickness");
        x = L;
        static const char* const WIDTHS[] = {"1 px", "2 px", "3 px", "4 px"};
        for (int i = 1; i <= 4; i++) chip(s, x, y, L, WIDTHS[i - 1], g_prefs.border_w == i, ACT_BWIDTH, i);
        y += 46;

        y = section(s, L, y, "Speed");
        x = L;
        static const char* const SPEEDS[] = {"Slow", "Normal", "Fast"};
        for (int i = 0; i < 3; i++) chip(s, x, y, L, SPEEDS[i], g_prefs.speed == i, ACT_SPEED, i);
        y += 46;

        y = section(s, L, y, "Show on");
        x = L;
        chip(s, x, y, L, "Focused window", !g_prefs.border_all, ACT_BALL, 0);
        chip(s, x, y, L, "All windows", g_prefs.border_all, ACT_BALL, 1);
        x += 16;
        chip(s, x, y, L, "Glow", g_prefs.glow, ACT_GLOW, !g_prefs.glow);
        y += 46;
        draw_text(s, g.font, L, y, "The effect is shown live around this window.", theme::TEXT_MUTED);
    } else {
        y = section(s, L, y, "Screen resolution");
        char line[96];
        ksnprintf(line, sizeof line, "Now: %d x %d", g.W, g.H);
        draw_text(s, g.font, L, y, line, theme::TEXT_MUTED);
        y += 30;
        if (bga_available()) {
            for (int i = 0; i < MODE_COUNT; i++) {
                if (!bga_mode_fits((u32)MODES[i].w, (u32)MODES[i].h)) continue;
                ksnprintf(line, sizeof line, "%d x %d", MODES[i].w, MODES[i].h);
                chip(s, x, y, L, line, MODES[i].w == g.W && MODES[i].h == g.H, ACT_MODE, i);
            }
            y += 46;
            draw_text(s, g.font, L, y, "Windows are moved back onto the screen if they no longer fit.", theme::TEXT_MUTED);
        } else {
            draw_text(s, g.font, L, y, "This display keeps the resolution it started with.", theme::TEXT);
            y += 24;
            draw_text(s, g.font, L, y, "It can be changed here on the standard graphics adapter", theme::TEXT_MUTED);
            y += 20;
            draw_text(s, g.font, L, y, "of VirtualBox (VBoxVGA, VBoxSVGA) and QEMU.", theme::TEXT_MUTED);
        }
    }
}

void paint_window_content(Window& w) {
    Surface view = content_view(w);
    int idx = window_index(&w);
    Rect cr = content_rect(w);
    if (w.kind == Kind::Terminal) {
        // Clear the flag before painting: output that arrives from another
        // thread while this paint runs sets it again and is drawn next frame.
        console_lock();
        g.term_dirty = false;
        Rect d = g.term.paint(view, g.focus == idx, w.needs_paint);
        console_unlock();
        if (!d.empty()) damage(d.translated(cr.x, cr.y));
        w.needs_paint = false;
        return;
    }
    if (!w.needs_paint) return;
    switch (w.kind) {
    case Kind::About: paint_about(view); break;
    case Kind::SystemMonitor: paint_sysmon(view); break;
    case Kind::MemoryMap: paint_memmap(view); break;
    case Kind::Settings: paint_settings(view); break;
    default: break;
    }
    w.needs_paint = false;
    damage(cr);
}

// Windows-style caption button: a flat cell the height of the title bar with
// a thin glyph; hover lights the cell, and close turns red.
void draw_button(Surface& s, const Window& w, int index, bool hover, bool focused) {
    Rect r = button_rect(w, index);
    if (hover) {
        Color bg = index == 0 ? theme::BTN_CLOSE_HOVER : theme::BTN_HOVER;
        if (index == 0) {
            // The close button sits in the window's rounded top-right corner.
            fill_rect_rounded(s, r, radius() - theme::BORDER_W, bg);
            fill_rect(s, {r.x, r.y, r.w - radius(), r.h}, bg);
            fill_rect(s, {r.right() - radius(), r.y + radius(), radius(), r.h - radius()}, bg);
        } else {
            fill_rect(s, r, bg);
        }
    }
    Color gc = hover || focused ? theme::BTN_GLYPH : theme::BTN_GLYPH_INACTIVE;
    if (hover && index == 0) gc = rgb(255, 255, 255);
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    switch (index) {
    case 0:     // close: X
        draw_line_aa(s, cx - 5, cy - 5, cx + 5, cy + 5, 8, gc);
        draw_line_aa(s, cx + 5, cy - 5, cx - 5, cy + 5, 8, gc);
        break;
    case 1:     // maximise: a square; restore: two overlapping squares
        if (w.maximised) {
            stroke_rect(s, {cx - 5, cy - 3, 9, 9}, gc);
            draw_hline(s, cx - 3, cx + 5, cy - 5, gc);
            draw_vline(s, cx + 5, cy - 5, cy + 3, gc);
        } else {
            stroke_rect(s, {cx - 5, cy - 5, 11, 11}, gc);
        }
        break;
    case 2:     // minimise: a bar
        draw_hline(s, cx - 5, cx + 5, cy, gc);
        break;
    }
}

// ------------------------------------------------------------ border effects --
// h in 0..1535: once round the colour wheel at full brightness.
Color hue_color(u32 h) {
    h %= 1536;
    u8 f = (u8)(h & 255), q = (u8)(255 - f);
    switch (h >> 8) {
    case 0: return rgb(255, f, 0);
    case 1: return rgb(q, 255, 0);
    case 2: return rgb(0, 255, f);
    case 3: return rgb(0, q, 255);
    case 4: return rgb(f, 0, 255);
    default: return rgb(255, 0, q);
    }
}

// Anti-aliased coverage of pixel (x, y) by a rounded rectangle.
u8 rounded_coverage(const Rect& r, int R, int x, int y) {
    if (x < r.x || y < r.y || x >= r.right() || y >= r.bottom()) return 0;
    int dx = x - r.x < r.right() - 1 - x ? x - r.x : r.right() - 1 - x;
    int dy = y - r.y < r.bottom() - 1 - y ? y - r.y : r.bottom() - 1 - y;
    return corner_coverage(R, dx, dy);
}

// Draws a ring `bw` pixels wide just inside the rounded rectangle `outer`;
// colour_at(x, y) gives each pixel's colour, its alpha the strength.
template <typename F> void draw_ring(Surface& s, const Rect& outer, int R, int bw, F colour_at) {
    Rect inner = outer.inset(bw);
    int Ri = R - bw > 0 ? R - bw : 0;
    int band = (R > bw ? R : bw) + 1;
    Rect clip = outer.intersect(s.clip);
    for (int y = clip.y; y < clip.bottom(); y++) {
        bool full_row = y < outer.y + band || y >= outer.bottom() - band;
        u32* row = s.row(y);
        for (int x = clip.x; x < clip.right(); x++) {
            if (!full_row && x >= outer.x + bw && x < outer.right() - bw) {
                x = outer.right() - bw - 1;     // skip the middle of the row
                continue;
            }
            int a = (int)rounded_coverage(outer, R, x, y) - (int)rounded_coverage(inner, Ri, x, y);
            if (a <= 0) continue;
            Color c = colour_at(x, y);
            row[x] = mix(row[x], c | 0xFF000000u, (u8)(a * alpha_of(c) / 255));
        }
    }
}

void draw_fx_border(Surface& back, const Window& w, int R, bool focused) {
    static const u32 PERIOD_MS[3] = {5000, 2400, 1000};
    const u32 period = PERIOD_MS[g_prefs.speed];
    const u64 t = refclock_now_us() / 1000;
    const Color base = g_prefs.border_color < 0 ? accent() : ACCENTS[g_prefs.border_color].c;
    const Rect f = w.frame;
    const int perimeter = 2 * (f.w + f.h);
    const u32 phase = (u32)(t % period) * 1536 / period;
    u32 strength = focused ? 255 : 150;
    Color flat = base;
    switch (g_prefs.fx) {
    case BorderFx::Breathing: {
        // Up and down once per period, eased at both ends.
        u32 p = (u32)(t % period), half = period / 2;
        u32 tri = p < half ? p * 255 / half : (period - p) * 255 / half;
        u32 eased = tri * tri * (765 - 2 * tri) / (255 * 255);
        strength = strength * (45 + eased * 210 / 255) / 255;
        break;
    }
    case BorderFx::Flashing:
        if ((t % (period / 2)) >= period / 4) strength = strength * 40 / 255;
        break;
    case BorderFx::Rainbow: flat = hue_color(phase); break;
    default: break;
    }
    bool chase = g_prefs.fx == BorderFx::Chase;
    // Position along the outline, clockwise from the top-left corner.
    auto colour_at = [&](int x, int y) -> Color {
        if (!chase) return flat;
        int dl = x - f.x, dr = f.right() - 1 - x, dt = y - f.y, db = f.bottom() - 1 - y;
        int pos;
        if (dt <= dl && dt <= dr && dt <= db) pos = dl;
        else if (dr <= dl && dr <= db) pos = f.w + dt;
        else if (db <= dl) pos = f.w + f.h + dr;
        else pos = 2 * f.w + f.h + db;
        if (pos < 0) pos = 0;
        return hue_color((u32)((u64)pos * 1536 / (u64)perimeter) + 1536 - phase);
    };
    if (g_prefs.glow) {
        for (int k = 5; k >= 1; k--) {
            u8 a = (u8)(strength * (66 - k * 11) / 255);
            draw_ring(back, f.inset(-k), R + k, 1, [&](int x, int y) { return with_alpha(colour_at(x, y), a); });
        }
    }
    draw_ring(back, f, R, g_prefs.border_w, [&](int x, int y) { return with_alpha(colour_at(x, y), (u8)strength); });
}

void draw_window(Surface& back, Window& w) {
    bool focused = g.focus == window_index(&w);
    // Shadow (cached; scaled while resizing to stay responsive).
    Rect sr = shadow_rect(w);
    int sw = w.frame.w + 2 * theme::SHADOW_PAD, sh = w.frame.h + 2 * theme::SHADOW_PAD;
    if (!w.shadow || ((w.shadow_w != sw || w.shadow_h != sh) && g.drag != Drag::Resize)) build_shadow(w);
    if (w.shadow) {
        Surface ss(w.shadow, w.shadow_w, w.shadow_h, w.shadow_w);
        if (w.shadow_w == sw && w.shadow_h == sh) blit_alpha(back, sr.x, sr.y, ss, focused ? 255 : 170);
        else blit_scaled(back, sr, ss);
    }

    // Remember what is under the bottom corners so the content can be masked.
    const int R = radius();
    u32 saved_l[MAX_RADIUS][MAX_RADIUS], saved_r[MAX_RADIUS][MAX_RADIUS];
    for (int dy = 0; dy < R; dy++) {
        int y = w.frame.bottom() - 1 - dy;
        for (int dx = 0; dx < R; dx++) {
            int xl = w.frame.x + dx, xr = w.frame.right() - 1 - dx;
            saved_l[dy][dx] = back.clip.contains(xl, y) ? back.row(y)[xl] : 0;
            saved_r[dy][dx] = back.clip.contains(xr, y) ? back.row(y)[xr] : 0;
        }
    }

    fill_rect_rounded(back, w.frame, R, focused ? theme::SURFACE : theme::SURFACE_INACTIVE);
    Rect cr = content_rect(w);
    Surface view = content_view(w);
    blit(back, cr.x, cr.y, view);
    // Blend the content's bottom corners back into what was underneath,
    // weighted by the anti-aliased corner coverage.
    for (int dy = 0; dy < R; dy++) {
        int y = w.frame.bottom() - 1 - dy;
        for (int dx = 0; dx < R; dx++) {
            u8 cov = corner_coverage(R, dx, dy);
            if (cov == 255) continue;
            int xl = w.frame.x + dx, xr = w.frame.right() - 1 - dx;
            if (back.clip.contains(xl, y)) back.row(y)[xl] = mix(saved_l[dy][dx], back.row(y)[xl], cov);
            if (back.clip.contains(xr, y)) back.row(y)[xr] = mix(saved_r[dy][dx], back.row(y)[xr], cov);
        }
    }
    draw_hline(back, cr.x, cr.right() - 1, cr.y - 1, rgba(255, 255, 255, 18));

    // Title text and buttons, then the outline over everything.
    int text_w = w.frame.w - 14 - 3 * theme::BTN_W - 8;
    draw_text_ellipsis(back, g.font, w.frame.x + 14, w.frame.y + theme::BORDER_W + (theme::TITLE_H - g.font.height) / 2,
                       w.title, text_w, focused ? theme::TITLE_TEXT : theme::TEXT_MUTED);
    bool hover_bar = w.frame.contains(g.mx, g.my) && g.my < w.frame.y + theme::TITLE_H + theme::BORDER_W;
    int hb = hover_bar && g.drag == Drag::None ? button_at(w, g.mx, g.my) : -1;
    for (int i = 0; i < 3; i++) draw_button(back, w, i, hb == i, focused);
    if (g_prefs.fx != BorderFx::None && (focused || g_prefs.border_all)) draw_fx_border(back, w, R, focused);
    else stroke_rect_rounded(back, w.frame, R, focused ? with_alpha(accent(), 210) : theme::BORDER);
}

// An application's icon: a rounded tile in its colour with its initial.
void draw_app_icon(Surface& s, int x, int y, int size, Kind kind) {
    Color c = theme::DOT_APP;
    char letter[2] = {'?', 0};
    for (const App& app : APPS)
        if (app.kind == kind) {
            c = app.dot;
            letter[0] = app.label[0];
        }
    fill_rect_rounded(s, {x, y, size, size}, size / 3, c);
    fill_rect_rounded(s, {x, y, size, size / 2}, size / 3, rgba(255, 255, 255, 34));     // a soft highlight
    int tw = measure_text(g.bold, letter);
    draw_text(s, g.bold, x + (size - tw) / 2, y + (size - g.bold.height) / 2, letter, rgb(15, 23, 42));
}

void draw_panel(Surface& back) {
    Rect pr = panel_rect();
    Rect bar = bar_rect();
    const int mid = bar_mid();
    const bool floating = g_prefs.panel_floating;
    const int R = floating ? 16 : 0;
    Color glass = g_prefs.panel_glass ? rgba(16, 19, 30, 214) : rgb(16, 19, 30);
    if (floating) {
        // A soft shadow so the bar reads as lifted off the wallpaper.
        fill_rect_rounded(back, bar.inset(-2).translated(0, 3), R + 2, rgba(0, 0, 0, 46));
        fill_rect_rounded(back, bar.inset(-1).translated(0, 2), R + 1, rgba(0, 0, 0, 60));
    }
    fill_rect_rounded(back, bar, R, glass);
    if (floating) {
        stroke_rect_rounded(back, bar, R, rgba(255, 255, 255, 30));
        draw_hline(back, bar.x + R, bar.right() - 1 - R, bar.y + 1, rgba(255, 255, 255, 26));
    } else {
        draw_hline(back, 0, g.W - 1, pr.y, theme::PANEL_LINE);
    }

    // Launcher button: the Cerberus mark.
    Rect lr = launcher_rect();
    bool lhover = lr.contains(g.mx, g.my) || g.menu_open;
    fill_rect_rounded(back, lr, 12, lhover ? accent() : accent_dark());
    fill_rect_rounded(back, {lr.x, lr.y, lr.w, lr.h / 2}, 12, rgba(255, 255, 255, lhover ? 40 : 24));
    stroke_rect_rounded(back, lr, 12, rgba(255, 255, 255, 46));
    Surface logo(g_logo_px, LOGO, LOGO, LOGO);
    blit_alpha(back, lr.x + (lr.w - LOGO) / 2, lr.y + (lr.h - LOGO) / 2, logo);

    // Search box: opens the launcher with the keyboard in its search field.
    Rect sr = search_rect();
    bool shover = sr.contains(g.mx, g.my);
    fill_rect_rounded(back, sr, sr.h / 2, rgba(255, 255, 255, shover || g.menu_open ? 30 : 15));
    stroke_rect_rounded(back, sr, sr.h / 2, g.menu_open ? with_alpha(accent(), 150) : rgba(255, 255, 255, 22));
    int gx = sr.x + 18, gy = sr.y + sr.h / 2 - 1;
    fill_circle_aa(back, gx, gy, 6, theme::TEXT_MUTED);
    fill_circle_aa(back, gx, gy, 4, rgb(38, 42, 54));
    draw_line_aa(back, gx + 4, gy + 4, gx + 8, gy + 8, 28, theme::TEXT_MUTED);
    if (g.menu_open && g_search_len) draw_text_ellipsis(back, g.font, sr.x + 35, sr.y + (sr.h - g.font.height) / 2, g_search, sr.w - 46, theme::TEXT);
    else draw_text(back, g.font, sr.x + 35, sr.y + (sr.h - g.font.height) / 2, "Search apps", theme::TEXT_MUTED);

    // Task buttons share the room there is: full width while it lasts,
    // narrower as windows are opened or the screen gets smaller.
    g.task_count = 0;
    int x = sr.right() + 12;
    int shown = 0;
    for (int i = 0; i < g.order_count; i++) shown += g.windows[g.order[i]].used;
    int room = clock_rect().x - 10 - x;
    int task_w = shown ? (room - 6 * (shown - 1)) / shown : 176;
    if (task_w > 176) task_w = 176;
    if (task_w < 40) task_w = 40;
    // In the order the windows were opened, not the stacking order, so a
    // button does not move when its window is clicked.
    for (int idx = 0; idx < MAX_WINDOWS; idx++) {
        Window& w = g.windows[idx];
        if (!w.used) continue;
        Rect tr{x, mid - 18, task_w, 36};
        if (tr.right() > clock_rect().x - 8) break;
        bool focused = g.focus == idx;
        bool hover = tr.contains(g.mx, g.my);
        if (focused) fill_rect_rounded(back, tr, 10, with_alpha(accent(), hover ? 74 : 54));
        else if (hover) fill_rect_rounded(back, tr, 10, rgba(255, 255, 255, 30));
        else if (!w.minimised) fill_rect_rounded(back, tr, 10, rgba(255, 255, 255, 12));
        draw_app_icon(back, tr.x + 8, tr.y + 7, 22, w.kind);
        if (tr.w > 76)
            draw_text_ellipsis(back, g.font, tr.x + 38, tr.y + (tr.h - g.font.height) / 2, w.title, tr.w - 46,
                               w.minimised ? theme::TEXT_MUTED : theme::TEXT);
        // The running mark: a wide accent pill under the focused window, a
        // short grey one under the others.
        int pw = focused ? 20 : 6;
        if (!w.minimised || focused)
            fill_rect_rounded(back, {tr.x + (tr.w - pw) / 2, tr.bottom() - 4, pw, 3}, 1, focused ? accent() : rgba(255, 255, 255, 90));
        g.task_rects[g.task_count] = tr;
        g.task_win[g.task_count] = idx;
        g.task_count++;
        x += tr.w + 6;
    }

    // Right-hand side: memory meter, then the clock over the date.
    DateTime now = rtc_now();
    char buf[64];
    u32 hour = now.hour;
    const char* suffix = "";
    if (g_prefs.clock_12h) {
        suffix = hour >= 12 ? " PM" : " AM";
        hour = hour % 12 ? hour % 12 : 12;
    }
    if (g_prefs.clock_seconds) ksnprintf(buf, sizeof buf, "%02u:%02u:%02u%s", hour, now.minute, now.second, suffix);
    else ksnprintf(buf, sizeof buf, "%02u:%02u%s", hour, now.minute, suffix);
    const int right = bar.right() - 16;
    int tw = measure_text(g.bold, buf);
    draw_text(back, g.bold, right - tw, mid - g.bold.height + 1, buf, theme::TEXT);
    ksnprintf(buf, sizeof buf, "%04u-%02u-%02u", now.year, now.month, now.day);
    tw = measure_text(g.font, buf);
    draw_text(back, g.font, right - tw, mid + 2, buf, theme::TEXT_MUTED);

    PmmStats pm = pmm_stats();
    u32 pct = pm.usable_frames ? (u32)(pm.used_frames * 100 / pm.usable_frames) : 0;
    const int sep = right - 104;
    draw_vline(back, sep, mid - 12, mid + 12, rgba(255, 255, 255, 28));
    Rect meter{sep - 16 - 64, mid + 5, 64, 5};
    ksnprintf(buf, sizeof buf, "%u%%", pct);
    draw_text(back, g.font, meter.x, mid - g.font.height + 1, "RAM", theme::TEXT_MUTED);
    tw = measure_text(g.font, buf);
    draw_text(back, g.font, meter.right() - tw, mid - g.font.height + 1, buf, theme::TEXT);
    fill_rect_rounded(back, meter, 2, rgba(255, 255, 255, 30));
    int fill = (int)(meter.w * pct / 100);
    if (fill < 5) fill = 5;
    fill_rect_rounded(back, {meter.x, meter.y, fill, meter.h}, 2, pct > 85 ? rgb(248, 113, 113) : accent());
}

void draw_menu(Surface& back) {
    Rect mr = menu_rect();
    fill_rect_rounded(back, mr.translated(0, 4), 14, rgba(0, 0, 0, 90));
    fill_rect_rounded(back, mr, 14, theme::MENU_BG);
    stroke_rect_rounded(back, mr, 14, theme::BORDER);

    // Search field.
    Rect sr = menu_search_rect();
    fill_rect_rounded(back, sr, 10, rgba(255, 255, 255, 18));
    stroke_rect_rounded(back, sr, 10, with_alpha(accent(), 150));
    int gx = sr.x + 17, gy = sr.y + sr.h / 2 - 1;
    fill_circle_aa(back, gx, gy, 6, theme::TEXT_MUTED);
    fill_circle_aa(back, gx, gy, 4, rgb(40, 43, 56));
    draw_line_aa(back, gx + 4, gy + 4, gx + 8, gy + 8, 28, theme::TEXT_MUTED);
    int tx = sr.x + 34, ty = sr.y + (sr.h - g.font.height) / 2;
    if (g_search_len) tx = draw_text_ellipsis(back, g.font, tx, ty, g_search, sr.w - 52, theme::TEXT);
    else draw_text(back, g.font, tx + 6, ty, "Type to search apps", theme::TEXT_MUTED);
    fill_rect(back, {tx + 1, sr.y + 8, 2, sr.h - 16}, accent());       // the text cursor

    // Matching applications. With something typed, Enter takes the first.
    int lit = g.menu_hover >= 0 ? g.menu_hover : (g_search_len ? 0 : -1);
    for (int i = 0; i < g_found_count; i++) {
        const App& app = APPS[g_found[i]];
        Rect ir{mr.x + 8, mr.y + theme::MENU_TOP + i * theme::MENU_ITEM_H, mr.w - 16, theme::MENU_ITEM_H};
        if (i == lit) fill_rect_rounded(back, ir, 8, with_alpha(accent(), 70));
        draw_app_icon(back, ir.x + 8, ir.y + 7, 24, app.kind);
        draw_text(back, g.font, ir.x + 42, ir.y + (ir.h - g.font.height) / 2, app.label, theme::TEXT);
    }
    if (!g_found_count)
        draw_text(back, g.font, mr.x + 22, mr.y + theme::MENU_TOP + 10, "No app matches that.", theme::TEXT_MUTED);

    // Power.
    draw_hline(back, mr.x + 12, mr.right() - 13, mr.bottom() - theme::MENU_FOOT, rgba(255, 255, 255, 22));
    draw_text(back, g.font, mr.x + 18, mr.bottom() - 34, "Cerberus", theme::TEXT_MUTED);
    for (int restart = 0; restart < 2; restart++) {
        Rect r = menu_power_rect(restart);
        bool hover = g.menu_hover == (restart ? ITEM_RESTART : ITEM_POWER);
        fill_rect_rounded(back, r, 8, hover ? (restart ? rgba(255, 255, 255, 40) : rgb(196, 43, 28)) : rgba(255, 255, 255, 18));
        const char* label = restart ? "Restart" : "Power off";
        draw_text(back, g.font, r.x + (r.w - measure_text(g.font, label)) / 2, r.y + (r.h - g.font.height) / 2, label,
                  theme::TEXT);
    }
}

// Framebuffer pixels are written with plain 32-bit stores, never `rep movsb`:
// VirtualBox's Hyper-V backend emulates string instructions that touch video
// memory one byte per exit and effectively never finishes a frame. Only
// pixels that differ from what the screen already shows are written.
inline void present_px(int x, int y, u32 v) {
    u32* f = g.front + (isize)y * g.W + x;
    if (*f == v) return;
    *f = v;
    g.fb.row(y)[x] = v;
    g.stats.last_written_pixels++;
}

inline void copy_to_fb(int x, int y, int w) {
    const u32* src = g.back.row(y) + x;
    u32* f = g.front + (isize)y * g.W + x;
    u32* dst = g.fb.row(y) + x;
    for (int i = 0; i < w; i++) {
        if (f[i] == src[i]) continue;
        f[i] = src[i];
        dst[i] = src[i];
        g.stats.last_written_pixels++;
    }
}

// The cursor blended over the back buffer, written to the screen.
void draw_cursor_on_fb() {
    Rect whole = cursor_rect(g.mx, g.my);
    Rect cr = whole.intersect({0, 0, g.W, g.H});
    const u32* shape = g_cursor_px[g_cursor_shape];
    for (int y = cr.y; y < cr.bottom(); y++) {
        for (int x = cr.x; x < cr.right(); x++) {
            u32 c = shape[(y - whole.y) * CURSOR_W + (x - whole.x)];
            u32 under = g.back.row(y)[x];
            u8 a = alpha_of(c);
            present_px(x, y, a == 0 ? under : a == 255 ? c : mix(under, c | 0xFF000000u, a));
        }
    }
    g.cursor_drawn = true;
    g.cursor_drawn_rect = cr;
}

// Copies a back-buffer rectangle to the framebuffer, re-drawing the cursor
// if it overlaps.
void present(const Rect& r) {
    Rect c = r.intersect({0, 0, g.W, g.H});
    if (c.empty()) return;
    for (int y = c.y; y < c.bottom(); y++) copy_to_fb(c.x, y, c.w);
    g.stats.last_present_pixels += (u64)c.w * c.h;
    if (c.overlaps(cursor_rect(g.mx, g.my))) draw_cursor_on_fb();
}

void compose(const Rect& r) {
    Rect c = r.intersect({0, 0, g.W, g.H});
    if (c.empty()) return;
    g.back.clip = c;
    Surface wv = g.wall.sub(c);
    blit(g.back, c.x, c.y, wv);
    for (int i = 0; i < g.order_count; i++) {
        Window& w = g.windows[g.order[i]];
        if (!w.used || w.minimised) continue;
        if (!visual_bounds(w).overlaps(c)) continue;
        draw_window(g.back, w);
    }
    if (g.menu_open && menu_rect().translated(0, 4).overlaps(c)) draw_menu(g.back);
    if (panel_rect().overlaps(c)) draw_panel(g.back);
    g.back.clip = g.back.bounds();
    present(c);
}

void flush_frame() {
    u64 t0 = refclock_now_us();
    g.stats.last_present_pixels = 0;
    g.stats.last_written_pixels = 0;
    if (g.damage_all) {
        compose({0, 0, g.W, g.H});
    } else {
        for (int i = 0; i < g.damage_count; i++) compose(g.damage[i]);
    }
    g.damage_all = false;
    g.damage_count = 0;
    g.stats.frames++;
    g.stats.last_frame_us = refclock_now_us() - t0;
    if (g.stats.last_frame_us > g.stats.worst_frame_us) {
        g.stats.worst_frame_us = g.stats.last_frame_us;
        g.stats.worst_written_pixels = g.stats.last_written_pixels;
    }
    g.last_frame_us = refclock_now_us();
}

// ----------------------------------------------------------------- input --
void move_cursor(int nx, int ny) {
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx >= g.W) nx = g.W - 1;
    if (ny >= g.H) ny = g.H - 1;
    if (nx == g.mx && ny == g.my) return;
    Rect old = cursor_rect(g.mx, g.my);
    g.mx = nx;
    g.my = ny;
    // Restore what was under the cursor, then draw it at the new place.
    Rect c = old.intersect({0, 0, g.W, g.H});
    for (int y = c.y; y < c.bottom(); y++) copy_to_fb(c.x, y, c.w);
    draw_cursor_on_fb();
}

void set_cursor_shape(int shape) {
    if (shape == g_cursor_shape) return;
    Rect c = cursor_rect(g.mx, g.my).intersect({0, 0, g.W, g.H});
    g_cursor_shape = shape;
    for (int y = c.y; y < c.bottom(); y++) copy_to_fb(c.x, y, c.w);
    draw_cursor_on_fb();
}

// The cursor that shows which way an edge or corner can be dragged.
int resize_shape(u8 edges) {
    bool horizontal = edges & 3, vertical = edges & 12;
    if (horizontal && vertical) return ((edges & 1) != 0) == ((edges & 4) != 0) ? CUR_RESIZE_NWSE : CUR_RESIZE_NESW;
    return horizontal ? CUR_RESIZE_H : vertical ? CUR_RESIZE_V : CUR_ARROW;
}

struct Hit {
    enum What { Nothing, Title, Button, Content, Edge, Panel, Launcher, Task, Menu } what = Nothing;
    int win = -1;
    int button = -1;
    u8 edges = 0;
    int item = -1;
};

Hit hit_test(int x, int y) {
    Hit h;
    if (g.menu_open) {
        Rect mr = menu_rect();
        if (mr.contains(x, y)) {
            h.what = Hit::Menu;
            h.item = menu_item_at(x, y);
            return h;
        }
    }
    if (panel_rect().contains(x, y)) {
        h.what = Hit::Panel;
        if (launcher_rect().contains(x, y)) h.what = Hit::Launcher;
        if (search_rect().contains(x, y)) {
            h.what = Hit::Launcher;
            h.item = 1;             // the search box: always opens, never closes
        }
        for (int i = 0; i < g.task_count; i++) {
            if (g.task_rects[i].contains(x, y)) {
                h.what = Hit::Task;
                h.win = g.task_win[i];
            }
        }
        return h;
    }
    for (int i = g.order_count - 1; i >= 0; i--) {
        Window& w = g.windows[g.order[i]];
        if (!w.used || w.minimised) continue;
        Rect outer = w.frame.inset(-theme::RESIZE_BAND);
        if (!outer.contains(x, y)) continue;
        h.win = g.order[i];
        u8 edges = 0;
        if (x < w.frame.x + theme::RESIZE_INSIDE) edges |= 1;
        if (x >= w.frame.right() - theme::RESIZE_INSIDE) edges |= 2;
        if (y < w.frame.y + theme::RESIZE_INSIDE) edges |= 4;
        if (y >= w.frame.bottom() - theme::RESIZE_INSIDE) edges |= 8;
        // Near a corner, an edge grab moves both edges: corners are easy to hit.
        if (edges) {
            bool near_left = x < w.frame.x + theme::RESIZE_CORNER, near_right = x >= w.frame.right() - theme::RESIZE_CORNER;
            bool near_top = y < w.frame.y + theme::RESIZE_CORNER, near_bottom = y >= w.frame.bottom() - theme::RESIZE_CORNER;
            if (edges & 12) edges |= near_left ? 1 : near_right ? 2 : 0;
            if (edges & 3) edges |= near_top ? 4 : near_bottom ? 8 : 0;
        }
        if (!w.maximised && edges) {
            int b = button_at(w, x, y);
            if (b < 0) {
                h.what = Hit::Edge;
                h.edges = edges;
                return h;
            }
        }
        if (!w.frame.contains(x, y)) continue;
        if (y < w.frame.y + theme::BORDER_W + theme::TITLE_H) {
            int b = button_at(w, x, y);
            if (b >= 0) {
                h.what = Hit::Button;
                h.button = b;
            } else {
                h.what = Hit::Title;
            }
        } else {
            h.what = Hit::Content;
        }
        return h;
    }
    return h;
}

void close_menu() {
    if (!g.menu_open) return;
    g.menu_open = false;
    damage(search_rect());
    damage(menu_rect().inset(-8).translated(0, 4));
    damage(launcher_rect());
}

void open_menu() {
    g.menu_open = true;
    g.menu_hover = -1;
    g_search_len = 0;
    g_search[0] = 0;
    search_update();
    damage(search_rect());
    damage(menu_rect().inset(-8).translated(0, 4));
    damage(launcher_rect());
}

void refresh_everything() {
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g.windows[i].used) g.windows[i].needs_paint = true;
    damage_all();
}

void render_wallpaper(Surface& s, int style, bool decorate);
bool set_resolution(int nw, int nh);

void settings_apply(u8 act, int val) {
    switch (act) {
    case ACT_TAB: g_settings_tab = val; break;
    case ACT_ACCENT: g_prefs.accent = val; break;
    case ACT_WALL:
        g_prefs.wallpaper = val;
        render_wallpaper(g.wall, val, true);
        break;
    case ACT_RADIUS:
        g_prefs.radius = val;
        // Shadows follow the corner shape: drop them so they are rebuilt.
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!g.windows[i].used || !g.windows[i].shadow) continue;
            free_pixels(g.windows[i].shadow);
            g.windows[i].shadow = nullptr;
        }
        break;
    case ACT_GLASS: g_prefs.panel_glass = val; break;
    case ACT_FLOAT: g_prefs.panel_floating = val; break;
    case ACT_CLOCK12: g_prefs.clock_12h = val; break;
    case ACT_SECONDS: g_prefs.clock_seconds = val; break;
    case ACT_FX: g_prefs.fx = (BorderFx)val; break;
    case ACT_BCOLOR: g_prefs.border_color = val; break;
    case ACT_BWIDTH: g_prefs.border_w = val; break;
    case ACT_SPEED: g_prefs.speed = val; break;
    case ACT_BALL: g_prefs.border_all = val; break;
    case ACT_GLOW: g_prefs.glow = val; break;
    case ACT_MODE:
        if (!set_resolution(MODES[val].w, MODES[val].h)) kprintf("gui: could not switch to %dx%d\n", MODES[val].w, MODES[val].h);
        break;
    default: return;
    }
    refresh_everything();
}

// A click inside the Settings window, in its content's coordinates.
void settings_click(int x, int y) {
    for (int i = 0; i < g_ctl_count; i++) {
        if (!g_ctls[i].r.contains(x, y)) continue;
        settings_apply(g_ctls[i].act, g_ctls[i].val);
        return;
    }
}

void menu_activate(int item) {
    if (item == ITEM_POWER || item == ITEM_RESTART) {
        if (item == ITEM_POWER && !power_can_power_off()) return;
        close_menu();
        kprintf("gui: %s requested from the launcher\n", item == ITEM_POWER ? "power off" : "restart");
        vfs_sync();
        if (item == ITEM_RESTART) power_reboot();
        power_off();
        return;
    }
    if (item < 0 || item >= g_found_count) return;
    Kind k = APPS[g_found[item]].kind;
    close_menu();
    open_kind(k);
}

void on_press(int button) {
    Hit h = hit_test(g.mx, g.my);
    if (button != 0) {
        if (h.what == Hit::Nothing && g.menu_open) close_menu();
        return;
    }
    if (g.menu_open && h.what != Hit::Menu && h.what != Hit::Launcher) close_menu();
    switch (h.what) {
    case Hit::Menu:
        menu_activate(h.item);
        break;
    case Hit::Launcher:
        if (g.menu_open && h.item != 1) close_menu();
        else if (!g.menu_open) open_menu();
        break;
    case Hit::Task: {
        Window& w = g.windows[h.win];
        if (w.minimised || g.focus != h.win) restore(w);
        else minimise(w);
        break;
    }
    case Hit::Title:
        raise_window(h.win);
        set_focus(h.win);
        damage(visual_bounds(g.windows[h.win]));
        g.drag = Drag::Move;
        g.drag_win = h.win;
        g.drag_sx = g.mx;
        g.drag_sy = g.my;
        g.drag_start = g.windows[h.win].frame;
        break;
    case Hit::Button: {
        Window& w = g.windows[h.win];
        raise_window(h.win);
        set_focus(h.win);
        if (h.button == 0) destroy_window(h.win);
        else if (h.button == 1) toggle_maximise(w);
        else minimise(w);
        break;
    }
    case Hit::Content:
        if (g.focus != h.win || g.order[g.order_count - 1] != h.win) {
            raise_window(h.win);
            set_focus(h.win);
            damage(visual_bounds(g.windows[h.win]));
        }
        if (g.windows[h.win].kind == Kind::Settings) {
            Rect cr = content_rect(g.windows[h.win]);
            settings_click(g.mx - cr.x, g.my - cr.y);
        }
        break;
    case Hit::Edge:
        raise_window(h.win);
        set_focus(h.win);
        damage(visual_bounds(g.windows[h.win]));
        g.drag = Drag::Resize;
        g.drag_win = h.win;
        g.drag_sx = g.mx;
        g.drag_sy = g.my;
        g.drag_start = g.windows[h.win].frame;
        g.drag_edges = h.edges;
        break;
    default: break;
    }
}

void on_release(int button) {
    if (button != 0) return;
    if (g.drag == Drag::Resize && g.drag_win >= 0 && g.windows[g.drag_win].used) {
        build_shadow(g.windows[g.drag_win]);
        damage(visual_bounds(g.windows[g.drag_win]));
    }
    g.drag = Drag::None;
    g.drag_win = -1;
    Hit h = hit_test(g.mx, g.my);
    set_cursor_shape(h.what == Hit::Edge ? resize_shape(h.edges) : CUR_ARROW);
}

void on_motion() {
    if (g.drag == Drag::None) {
        Hit over = hit_test(g.mx, g.my);
        set_cursor_shape(over.what == Hit::Edge ? resize_shape(over.edges) : CUR_ARROW);
    }
    if (g.drag != Drag::None && g.drag_win >= 0 && g.windows[g.drag_win].used) {
        Window& w = g.windows[g.drag_win];
        int dx = g.mx - g.drag_sx, dy = g.my - g.drag_sy;
        if (g.drag == Drag::Move) {
            if (w.maximised) return;
            set_window_frame(w, g.drag_start.translated(dx, dy));
        } else {
            Rect nf = g.drag_start;
            if (g.drag_edges & 1) { nf.x += dx; nf.w -= dx; }
            if (g.drag_edges & 2) nf.w += dx;
            if (g.drag_edges & 4) { nf.y += dy; nf.h -= dy; }
            if (g.drag_edges & 8) nf.h += dy;
            if (nf.w < w.min_w) { if (g.drag_edges & 1) nf.x = g.drag_start.right() - w.min_w; nf.w = w.min_w; }
            if (nf.h < w.min_h) { if (g.drag_edges & 4) nf.y = g.drag_start.bottom() - w.min_h; nf.h = w.min_h; }
            set_window_frame(w, nf);
        }
        return;
    }
    // Hover feedback: menu items, panel buttons, title buttons.
    if (g.menu_open) {
        Rect mr = menu_rect();
        int hover = mr.contains(g.mx, g.my) ? menu_item_at(g.mx, g.my) : ITEM_NONE;
        // The mouse leaving the list does not undo a choice made with the keys.
        if (hover == ITEM_NONE && g.menu_hover >= 0 && !mr.contains(g.mx, g.my)) hover = g.menu_hover;
        if (hover != g.menu_hover) {
            g.menu_hover = hover;
            damage(mr);
        }
    }
    static Hit::What last_what = Hit::Nothing;
    static int last_win = -1, last_button = -1;
    Hit h = hit_test(g.mx, g.my);
    bool panel_now = h.what == Hit::Panel || h.what == Hit::Launcher || h.what == Hit::Task;
    bool panel_before = last_what == Hit::Panel || last_what == Hit::Launcher || last_what == Hit::Task;
    if (panel_now || panel_before) damage(panel_rect());
    int hb = -1;
    if (h.win >= 0 && (h.what == Hit::Title || h.what == Hit::Button)) hb = h.button;
    bool title_now = h.what == Hit::Title || h.what == Hit::Button;
    bool title_before = last_what == Hit::Title || last_what == Hit::Button;
    if ((title_now != title_before) || (h.win != last_win) || (hb != last_button)) {
        if (last_win >= 0 && g.windows[last_win].used)
            damage({g.windows[last_win].frame.x, g.windows[last_win].frame.y, g.windows[last_win].frame.w,
                    theme::TITLE_H + 2});
        if (h.win >= 0 && g.windows[h.win].used)
            damage({g.windows[h.win].frame.x, g.windows[h.win].frame.y, g.windows[h.win].frame.w, theme::TITLE_H + 2});
    }
    last_what = h.what;
    last_win = h.win;
    last_button = hb;
}

void term_input_push(char c) {
    // One writer (the compositor) and one reader (the shell), possibly on
    // different CPUs: the slot is filled before the index is published.
    usize next = (g.term_in_head + 1) % sizeof g.term_in;
    if (next == g.term_in_tail) return;
    g.term_in[g.term_in_head] = c;
    __atomic_store_n(&g.term_in_head, next, __ATOMIC_RELEASE);
}

void terminal_scroll(int lines, bool to_bottom = false) {
    console_lock();
    if (to_bottom) g.term.scroll_to_bottom();
    else g.term.scroll(lines);
    console_unlock();
    g.term_dirty = true;
}

void cycle_focus() {
    if (g.order_count < 2) return;
    // Raise the bottom-most visible window (classic Alt+Tab rotation).
    for (int i = 0; i < g.order_count; i++) {
        Window& w = g.windows[g.order[i]];
        if (w.used && !w.minimised && g.order[i] != g.focus) {
            restore(w);
            return;
        }
    }
}

void process_keyboard() {
    KeyEvent e;
    while (ps2kbd_poll_event(&e)) {
        // Super opens the menu when released on its own, as on Windows; used
        // with another key (Super+T, Super+M) it is only a modifier.
        if (e.key == key::SUPER) {
            if (e.pressed && !g.super_down) {
                g.super_down = true;
                g.super_chord = false;
            } else if (!e.pressed) {
                if (g.super_down && !g.super_chord) {
                    if (g.menu_open) close_menu();
                    else open_menu();
                }
                g.super_down = false;
            }
            continue;
        }
        if (!e.pressed) continue;
        if (g.super_down) g.super_chord = true;
        bool alt = e.mods & mod::ALT, super_ = e.mods & mod::SUPER, shift = e.mods & mod::SHIFT;
        if (alt && e.key == key::F1 + 3) {                 // Alt+F4
            if (g.focus >= 0) destroy_window(g.focus);
            continue;
        }
        if (alt && e.key == key::TAB) { cycle_focus(); continue; }
        if (super_ && e.ascii == 't') { close_menu(); open_kind(Kind::Terminal); continue; }
        if (super_ && e.ascii == 'm') { if (g.focus >= 0) toggle_maximise(g.windows[g.focus]); continue; }
        if (e.key == key::ESCAPE && g.menu_open) { close_menu(); continue; }
        if (g.menu_open) {
            int sel = g.menu_hover >= 0 ? g.menu_hover : -1;
            if (e.key == key::DOWN && g_found_count) sel = (sel + 1) % g_found_count;
            else if (e.key == key::UP && g_found_count) sel = (sel < 0 ? 0 : sel + g_found_count - 1) % g_found_count;
            else if (e.key == key::ENTER) {
                // Enter takes the highlighted application, or the first match.
                menu_activate(sel >= 0 ? sel : 0);
                continue;
            } else if (e.key == key::BACKSPACE) {
                if (g_search_len) g_search[--g_search_len] = 0;
                search_update();
                sel = -1;
            } else if (e.ascii >= 32 && e.ascii < 127 && !alt && !super_) {
                if (g_search_len < (int)sizeof g_search - 1) {
                    g_search[g_search_len++] = e.ascii;
                    g_search[g_search_len] = 0;
                }
                search_update();
                sel = -1;
            }
            g.menu_hover = sel;
            damage(menu_rect());
            damage(search_rect());
            continue;       // the open menu takes every key
        }
        if (g.focus < 0 || g.focus != g.term_win) continue;
        // Shift+Page Up/Down scroll the terminal's history by a screen.
        if (shift && (e.key == key::PAGE_UP || e.key == key::PAGE_DOWN)) {
            int page = g.term.rows() > 2 ? g.term.rows() - 2 : 1;
            terminal_scroll(e.key == key::PAGE_UP ? page : -page);
            continue;
        }
        if (e.ascii) {
            if (g.term.scrolled_back()) terminal_scroll(0, true);      // typing returns to the bottom
            term_input_push(e.ascii);
        }
    }
}

void process_mouse() {
    MouseEvent e;
    while (ps2mouse_poll(&e)) {
        if (e.dx || e.dy) {
            move_cursor(g.mx + e.dx, g.my + e.dy);
            on_motion();
        }
        if (e.dz) {
            Hit h = hit_test(g.mx, g.my);
            if (h.what == Hit::Content && h.win >= 0 && h.win == g.term_win) terminal_scroll(e.dz * 3);
        }
        u8 changed = e.buttons ^ g.buttons;
        g.buttons = e.buttons;
        for (int b = 0; b < 3; b++) {
            if (!(changed & (1 << b))) continue;
            if (e.buttons & (1 << b)) on_press(b);
            else on_release(b);
        }
    }
}

// ------------------------------------------------------------- wallpaper --
// The background is computed per pixel with 8 extra bits per channel and
// ordered dithering: a dark, slow gradient drawn straight into 8 bits shows
// as visible bands.
// `style` picks one of the built-in wallpapers; they scale to any size (the
// Settings window's previews are the same code at 128x80). `decorate` adds
// the wordmark and the version line of the real desktop.
void render_wallpaper(Surface& s, int style, bool decorate) {
    static const u8 BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    // A glow: centre and radius in 1/60ths of the width and height.
    struct GlowSpec {
        int cx60, cy60, r60;
        Color c;
    };
    struct Style {
        Color top, bottom;
        GlowSpec glows[3];
    };
    static const Style STYLES[WALLPAPER_COUNT] = {
        // Nebula: deep blue with a blue and a violet light.
        {rgb(11, 17, 32), rgb(26, 20, 51),
         {{48, 12, 30, rgba(59, 130, 246, 110)}, {10, 50, 24, rgba(139, 92, 246, 90)}, {0, 0, 0, 0}}},
        // Aurora: night green with drifting teal.
        {rgb(3, 16, 22), rgb(6, 26, 38),
         {{14, 14, 30, rgba(16, 185, 129, 120)}, {46, 24, 24, rgba(34, 211, 238, 85)}, {30, 62, 28, rgba(99, 102, 241, 75)}}},
        // Ember: dusk with orange and rose.
        {rgb(24, 10, 16), rgb(40, 14, 34),
         {{12, 48, 30, rgba(249, 115, 22, 120)}, {48, 44, 24, rgba(236, 72, 153, 95)}, {30, 8, 20, rgba(250, 204, 21, 45)}}},
    };
    const Style& st = STYLES[style >= 0 && style < WALLPAPER_COUNT ? style : 0];
    const int W = s.width, H = s.height;
    struct Glow {
        int cx, cy;
        i64 r2;
        Color c;
    };
    Glow glows[3];
    int glow_count = 0;
    for (const GlowSpec& gs : st.glows) {
        if (!gs.r60) continue;
        i64 r = (i64)W * gs.r60 / 60;
        glows[glow_count++] = Glow{W * gs.cx60 / 60, H * gs.cy60 / 60, r * r, gs.c};
    }
    const Color top = st.top, bottom = st.bottom;
    for (int y = 0; y < H; y++) {
        // Vertical gradient, channels scaled by 256.
        i64 t = H > 1 ? (i64)y * 65536 / (H - 1) : 0;
        i64 base[3] = {
            red_of(top) * 256 + ((i64)(red_of(bottom) - red_of(top)) * t >> 8),
            green_of(top) * 256 + ((i64)(green_of(bottom) - green_of(top)) * t >> 8),
            blue_of(top) * 256 + ((i64)(blue_of(bottom) - blue_of(top)) * t >> 8),
        };
        u32* row = s.row(y);
        for (int x = 0; x < W; x++) {
            i64 ch[3] = {base[0], base[1], base[2]};
            for (int i = 0; i < glow_count; i++) {
                const Glow& gl = glows[i];
                i64 d2 = (i64)(x - gl.cx) * (x - gl.cx) + (i64)(y - gl.cy) * (y - gl.cy);
                if (d2 >= gl.r2) continue;
                // Strength falls off with the squared distance; 0..alpha*256.
                i64 a = (i64)alpha_of(gl.c) * 256 * (gl.r2 - d2) / gl.r2;
                ch[0] += (red_of(gl.c) * 256 - ch[0]) * a / (255 * 256);
                ch[1] += (green_of(gl.c) * 256 - ch[1]) * a / (255 * 256);
                ch[2] += (blue_of(gl.c) * 256 - ch[2]) * a / (255 * 256);
            }
            i64 d = BAYER[y & 3][x & 3] * 16 + 8;
            u32 r = (u32)((ch[0] + d) >> 8), gr = (u32)((ch[1] + d) >> 8), bl = (u32)((ch[2] + d) >> 8);
            row[x] = rgb((u8)(r > 255 ? 255 : r), (u8)(gr > 255 ? 255 : gr), (u8)(bl > 255 ? 255 : bl));
        }
    }
    if (!decorate) return;
    // Faint wordmark.
    const char* mark = "Cerberus";
    const Font& big = W >= 1100 ? g.display_big : g.display;
    int mw = measure_text(big, mark);
    draw_text(s, big, (W - mw) / 2, (H - theme::PANEL_H - big.height) / 2, mark, rgba(255, 255, 255, 22));
    char line[64];
    ksnprintf(line, sizeof line, "Cerberus %s  |  preview desktop", cerberus_version());
    int tw = measure_text(g.font, line);
    draw_text(s, g.font, W - tw - 16, H - theme::PANEL_H - 26, line, rgba(255, 255, 255, 70));
}

// The launcher mark, sampled 4x4 per pixel. Distances are in eighths of a
// pixel from the centre: a ring, opened on the right, with a dot in the gap.
void build_logo() {
    constexpr int C = LOGO * 4;                     // the centre, in eighths
    constexpr int OUTER = 96, INNER = 54, GAP = 34, DOT_X = 75, DOT_R = 25;
    for (int y = 0; y < LOGO; y++)
        for (int x = 0; x < LOGO; x++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int dx = x * 8 + sx * 2 + 1 - C, dy = y * 8 + sy * 2 + 1 - C;
                    int d2 = dx * dx + dy * dy;
                    bool ring = d2 <= OUTER * OUTER && d2 >= INNER * INNER && !(dx > 0 && dy > -GAP && dy < GAP);
                    bool dot = (dx - DOT_X) * (dx - DOT_X) + dy * dy <= DOT_R * DOT_R;
                    hits += ring || dot;
                }
            g_logo_px[y * LOGO + x] = rgba(255, 255, 255, (u8)(hits * 255 / 16));
        }
}

// Changes the screen resolution (where the display adapter can): new
// buffers first, so a failure leaves everything as it was.
bool set_resolution(int nw, int nh) {
    if (nw == g.W && nh == g.H) return true;
    if (!bga_mode_fits((u32)nw, (u32)nh)) return false;
    usize px = (usize)nw * nh;
    int sw = nw + 2 * theme::SHADOW_PAD, sh = nh + 2 * theme::SHADOW_PAD;
    u32* back = alloc_pixels(px);
    u32* wall = alloc_pixels(px);
    u32* front = alloc_pixels(px);
    u32* scratch = alloc_pixels((usize)sw * sh);
    u32* content[MAX_WINDOWS] = {};
    bool ok = back && wall && front && scratch;
    for (int i = 0; i < MAX_WINDOWS && ok; i++) {
        if (!g.windows[i].used) continue;
        content[i] = alloc_pixels(px);
        ok = content[i] != nullptr;
    }
    if (ok) ok = bga_set_mode((u32)nw, (u32)nh).ok();
    if (!ok) {
        free_pixels(back);
        free_pixels(wall);
        free_pixels(front);
        free_pixels(scratch);
        for (u32* p : content) free_pixels(p);
        return false;
    }
    free_pixels(g.back.pixels);
    free_pixels(g.wall.pixels);
    free_pixels(g.scratch.pixels);
    free_pixels(g.front);
    const FramebufferInfo& fb = g_boot_info.framebuffer;
    g.W = nw;
    g.H = nh;
    g.fb = Surface((u32*)fb.address, nw, nh, (int)(fb.pitch / 4));
    g.back = Surface(back, nw, nh, nw);
    g.wall = Surface(wall, nw, nh, nw);
    g.scratch = Surface(scratch, sw, sh, sw);
    memset(front, 0, px * 4);       // nothing on the new screen is known: write every pixel
    g.front = front;
    g.damage_count = 0;
    Rect wa = work_area();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window& w = g.windows[i];
        if (!w.used) continue;
        free_pixels(w.pixels);
        w.pixels = content[i];
        w.content = Surface(w.pixels, nw, nh, nw);
        Rect f = w.frame;
        if (w.maximised) {
            f = {wa.x + 6, wa.y + 6, wa.w - 12, wa.h - 12};
        } else {
            if (f.w > wa.w - 16) f.w = wa.w - 16;
            if (f.h > wa.h - 16) f.h = wa.h - 16;
            if (f.w < w.min_w) f.w = w.min_w;
            if (f.h < w.min_h) f.h = w.min_h;
            if (f.x + f.w > wa.w - 8) f.x = wa.w - 8 - f.w;
            if (f.y + f.h > wa.h - 8) f.y = wa.h - 8 - f.h;
            if (f.x < 8) f.x = 8;
            if (f.y < 8) f.y = 8;
        }
        w.frame = f;
        w.needs_paint = true;
        if (i == g.term_win) terminal_fit(w);
    }
    render_wallpaper(g.wall, g_prefs.wallpaper, true);
    if (g.mx >= nw) g.mx = nw - 1;
    if (g.my >= nh) g.my = nh - 1;
    g.cursor_drawn = false;
    damage_all();
    kprintf("gui: resolution changed to %dx%d\n", nw, nh);
    return true;
}

void build_cursor() {
    const u32 BLACK = rgba(0, 0, 0, 230), WHITE = rgb(255, 255, 255);
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_ART_W; x++) {
            char c = CURSOR_ART[y][x];
            g_cursor_px[CUR_ARROW][y * CURSOR_W + x] = c == '#' ? BLACK : c == 'o' ? WHITE : 0;
        }
    }
    // The resize shapes: a white two-headed arrow through the centre along
    // (dx, dy), then a black outline around it.
    const int dirs[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
    for (int s = 0; s < 4; s++) {
        u32* px = g_cursor_px[CUR_RESIZE_H + s];
        int dx = dirs[s][0], dy = dirs[s][1];
        bool diagonal = dx && dy;
        int reach = diagonal ? 5 : 7, mid = CURSOR_W / 2;
        auto put = [&](int x, int y) {
            if (x >= 1 && y >= 1 && x < CURSOR_W - 1 && y < CURSOR_H - 1) px[y * CURSOR_W + x] = WHITE;
        };
        for (int t = -reach; t <= reach; t++) put(mid + t * dx, mid + t * dy);
        for (int end = -1; end <= 1; end += 2) {
            int tx = mid + end * reach * dx, ty = mid + end * reach * dy;
            for (int a = 0; a <= 4; a++)
                for (int b = 0; a + b <= 4; b++) {
                    if (diagonal) {
                        put(tx - end * a * dx, ty - end * b * dy);
                    } else if (b <= a && a <= 3) {
                        // a steps back from the tip, b steps to either side
                        put(tx - end * a * dx + b * dy, ty - end * a * dy + b * dx);
                        put(tx - end * a * dx - b * dy, ty - end * a * dy - b * dx);
                    }
                }
        }
        for (int y = 0; y < CURSOR_H; y++)
            for (int x = 0; x < CURSOR_W; x++) {
                if (px[y * CURSOR_W + x]) continue;
                bool edge = false;
                for (int oy = -1; oy <= 1; oy++)
                    for (int ox = -1; ox <= 1; ox++) {
                        int nx = x + ox, ny = y + oy;
                        if (nx >= 0 && ny >= 0 && nx < CURSOR_W && ny < CURSOR_H && px[ny * CURSOR_W + nx] == WHITE) edge = true;
                    }
                if (edge) px[y * CURSOR_W + x] = BLACK;
            }
    }
}

} // namespace

// ------------------------------------------------------------- public API --
bool gui_init() {
    const FramebufferInfo& fb = g_boot_info.framebuffer;
    if (!fb.present || fb.bpp != 32) {
        kprintf("gui: no 32bpp framebuffer, staying in text mode\n");
        return false;
    }
    g.W = (int)fb.width;
    g.H = (int)fb.height;
    g.fb = Surface((u32*)fb.address, g.W, g.H, (int)(fb.pitch / 4));
    {
        int level = 0;
        u64 leaf = vmm_kernel_leaf((vaddr_t)fb.address, &level);
        kprintf("gui: framebuffer pte %#lx (level %d), PAT %#lx\n", (unsigned long)leaf, level,
                (unsigned long)rdmsr(msr::PAT));
    }
    usize blob_size = (usize)(_binary_fonts_aa_end - _binary_fonts_aa_start);
    g.font = font_from_blob(_binary_fonts_aa_start, blob_size, "ui");
    g.bold = font_from_blob(_binary_fonts_aa_start, blob_size, "ui-bold");
    g.mono = font_from_blob(_binary_fonts_aa_start, blob_size, "mono");
    g.display = font_from_blob(_binary_fonts_aa_start, blob_size, "display");
    g.display_big = font_from_blob(_binary_fonts_aa_start, blob_size, "display-big");
    if (!g.font.valid() || !g.bold.valid() || !g.mono.valid() || !g.display.valid() || !g.display_big.valid()) {
        kprintf("gui: fonts invalid\n");
        return false;
    }

    u32* back = alloc_pixels((usize)g.W * g.H);
    u32* wall = alloc_pixels((usize)g.W * g.H);
    usize scratch_px = (usize)(g.W + 2 * theme::SHADOW_PAD) * (g.H + 2 * theme::SHADOW_PAD);
    u32* scratch = alloc_pixels(scratch_px);
    u32* front = alloc_pixels((usize)g.W * g.H);
    constexpr int TERM_COLS = 200, TERM_HISTORY = 1000;
    u8* cells = (u8*)kmalloc((usize)TERM_COLS * TERM_HISTORY);
    if (!back || !wall || !scratch || !front || !cells) {
        kprintf("gui: out of memory for screen buffers\n");
        return false;
    }
    g.back = Surface(back, g.W, g.H, g.W);
    g.wall = Surface(wall, g.W, g.H, g.W);
    g.scratch = Surface(scratch, g.W + 2 * theme::SHADOW_PAD, g.H + 2 * theme::SHADOW_PAD, g.W + 2 * theme::SHADOW_PAD);
    // Zero is never an opaque pixel, so the first frame writes every pixel.
    memset(front, 0, (usize)g.W * g.H * 4);
    g.front = front;
    g.term_cells = cells;
    g.term.init(g.term_cells, TERM_COLS, TERM_HISTORY, g.mono);

    render_wallpaper(g.wall, g_prefs.wallpaper, true);
    for (int i = 0; i < WALLPAPER_COUNT; i++) {
        Surface thumb(g_thumb_px[i], THUMB_W, THUMB_H, THUMB_W);
        render_wallpaper(thumb, i, false);
    }
    build_logo();
    search_update();
    build_cursor();
    g.mx = g.W / 2;
    g.my = g.H / 2;

    fbconsole_disable();
    g.active = true;
    create_window(Kind::About, "About Cerberus", {g.W - 520, 90, 460, 400});
    create_window(Kind::Terminal, "Terminal", {56, 56, 740, 470});
    damage_all();
    flush_frame();
    kprintf("gui: desktop up at %dx%d, %d windows, first frame %lu us (%lu px to the framebuffer)\n", g.W, g.H,
            g.order_count, (unsigned long)g.stats.last_frame_us, (unsigned long)g.stats.last_written_pixels);
    return true;
}

bool gui_active() { return g.active; }

void gui_terminal_putc(char c) {
    if (!g.active) return;
    g.term.putc(c);
    g.term_dirty = true;
}

int gui_terminal_getc() {
    if (__atomic_load_n(&g.term_in_head, __ATOMIC_ACQUIRE) == g.term_in_tail) return -1;
    char c = g.term_in[g.term_in_tail];
    __atomic_store_n(&g.term_in_tail, (g.term_in_tail + 1) % sizeof g.term_in, __ATOMIC_RELEASE);
    return (unsigned char)c;
}

bool gui_request_resolution(u32 width, u32 height) {
    if (!g.active || !bga_mode_fits(width, height) || width > 0xFFFF || height > 0xFFFF) return false;
    __atomic_store_n(&g_wanted_mode, width << 16 | height, __ATOMIC_RELEASE);
    return true;
}

void gui_screen_size(u32* width, u32* height) {
    *width = (u32)g.W;
    *height = (u32)g.H;
}

void gui_pump() {
    if (!g.active) return;
    if (u32 wanted = __atomic_exchange_n(&g_wanted_mode, 0, __ATOMIC_ACQ_REL)) {
        if (!set_resolution((int)(wanted >> 16), (int)(wanted & 0xFFFF))) kprintf("gui: could not change the resolution\n");
        refresh_everything();
    }
    process_keyboard();
    process_mouse();

    DateTime now = rtc_now();
    if (now.second != g.last_second) {
        g.last_second = now.second;
        damage(clock_rect());
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (g.windows[i].used && g.windows[i].kind == Kind::SystemMonitor) g.windows[i].needs_paint = true;
    }

    u64 now_us = refclock_now_us();
    // An animated border redraws about 30 times a second; nothing else on
    // an idle desktop moves.
    if (fx_animated() && now_us - g_last_anim_us >= 33000) {
        g_last_anim_us = now_us;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            const Window& w = g.windows[i];
            if (w.used && !w.minimised && (g_prefs.border_all || i == g.focus)) damage(w.frame.inset(-6));
        }
    }
    if (now_us - g.last_frame_us < FRAME_US) return;

    for (int i = 0; i < g.order_count; i++) {
        Window& w = g.windows[g.order[i]];
        if (!w.used || w.minimised) continue;
        if (w.needs_paint || (w.kind == Kind::Terminal && g.term_dirty)) paint_window_content(w);
    }
    if (g.damage_all || g.damage_count) flush_frame();
}

namespace {

WaitQueue g_compositor_wake;

// PS/2 interrupt context: new key or mouse data is waiting.
void input_arrived() { g_compositor_wake.wake_one(); }

// The compositor's own thread. It sleeps until input arrives or the next
// timer tick, whichever is first, so an idle desktop costs one cheap pass
// per tick and input is handled as soon as its interrupt returns.
void compositor_main(void*) {
    for (;;) {
        gui_pump();
        g_compositor_wake.wait_ticks(1);
    }
}

} // namespace

bool gui_start_compositor() {
    Result<Thread*> t = kthread_create(compositor_main, nullptr, "compositor", prio::INTERACTIVE, nullptr, true);
    if (!t.ok()) return false;
    ps2_set_input_hook(input_arrived);
    return true;
}

GuiStats gui_stats() {
    GuiStats s = g.stats;
    s.windows = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) s.windows += g.windows[i].used;
    return s;
}

// Called from the shell's thread; a frame racing with it only makes the
// next reading one frame late.
void gui_reset_worst() {
    g.stats.worst_frame_us = 0;
    g.stats.worst_written_pixels = 0;
}

void gui_emergency_text_mode() {
    if (!g.active) return;
    g.active = false;
    fbconsole_enable();
}
