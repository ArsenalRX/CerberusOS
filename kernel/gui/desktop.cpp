// Desktop compositor (kernel-hosted preview of Pane, SPEC §9/§11).
//
// Rendering model: a wallpaper surface, a back buffer, and the framebuffer.
// Every change records a damage rectangle; a frame recomposites only the
// damaged rectangles (wallpaper, then windows bottom-to-top with shadows and
// decorations, then the launcher menu and the panel) into the back buffer and
// copies just those rectangles to the framebuffer. The cursor is painted on
// the framebuffer last and restored from the back buffer when it moves, so
// moving the mouse never triggers recomposition.
#include <arch/x86_64/cpuid.h>
#include <boot/bootinfo.h>
#include <drivers/fbconsole.h>
#include <drivers/lapic.h>
#include <drivers/ps2kbd.h>
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
#include <mm/kheap.h>
#include <mm/pmm.h>

extern "C" const u8 _binary_font_8x16_start[];
extern "C" const u8 _binary_font_16x32_start[];

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
constexpr Color BTN_CLOSE = rgb(239, 68, 68);
constexpr Color BTN_MAX = rgb(34, 197, 94);
constexpr Color BTN_MIN = rgb(234, 179, 8);
constexpr Color BTN_GLYPH = rgba(0, 0, 0, 160);
constexpr Color TASK_BG = rgba(255, 255, 255, 18);
constexpr Color TASK_FOCUS = rgba(96, 165, 250, 60);
constexpr Color MENU_BG = rgba(24, 27, 38, 245);
constexpr Color MENU_HOVER = rgba(96, 165, 250, 70);
constexpr Color SHADOW = rgba(0, 0, 0, 150);
constexpr Color CONTENT_BG = rgb(22, 24, 33);

constexpr int TITLE_H = 32;
constexpr int BORDER_W = 1;
constexpr int RADIUS = 10;
constexpr int SHADOW_PAD = 22;
constexpr int SHADOW_OFF = 5;
constexpr int SHADOW_BLUR = 9;
constexpr int PANEL_H = 40;
constexpr int RESIZE_BAND = 6;
constexpr int BTN_R = 7;
constexpr int BTN_GAP = 24;
constexpr int MENU_W = 260;
constexpr int MENU_ITEM_H = 38;
} // namespace theme

constexpr u64 FRAME_US = 16000;
constexpr int MAX_WINDOWS = 12;
constexpr int MAX_DAMAGE = 24;

enum class Kind { Terminal, About, SystemMonitor, MemoryMap };

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

struct MenuItem {
    const char* label;
    Kind kind;
};
const MenuItem MENU_ITEMS[] = {
    {"Terminal", Kind::Terminal},
    {"System Monitor", Kind::SystemMonitor},
    {"Memory Map", Kind::MemoryMap},
    {"About Lumen", Kind::About},
};
constexpr int MENU_COUNT = sizeof MENU_ITEMS / sizeof MENU_ITEMS[0];

enum class Drag { None, Move, Resize };

struct State {
    bool active = false;
    int W = 0, H = 0;
    Surface fb, back, wall, scratch;
    Font font, font_big;

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
constexpr int CURSOR_W = 12, CURSOR_H = 19;
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
u32 g_cursor_px[CURSOR_W * CURSOR_H];
Surface g_cursor;

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
Rect launcher_rect() { return {8, g.H - theme::PANEL_H + 6, 96, theme::PANEL_H - 12}; }
Rect clock_rect() { return {g.W - 250, g.H - theme::PANEL_H, 250, theme::PANEL_H}; }
Rect menu_rect() {
    int h = MENU_COUNT * theme::MENU_ITEM_H + 16;
    return {8, g.H - theme::PANEL_H - h - 8, theme::MENU_W, h};
}
Rect cursor_rect(int x, int y) { return {x, y, CURSOR_W, CURSOR_H}; }

// Title-bar button centres, right to left: close, maximise, minimise.
Point button_centre(const Window& w, int index) {
    int cx = w.frame.right() - theme::BORDER_W - 16 - index * theme::BTN_GAP;
    int cy = w.frame.y + theme::BORDER_W + theme::TITLE_H / 2;
    return {cx, cy};
}

int button_at(const Window& w, int x, int y) {
    for (int i = 0; i < 3; i++) {
        Point c = button_centre(w, i);
        int dx = x - c.x, dy = y - c.y;
        if (dx * dx + dy * dy <= (theme::BTN_R + 3) * (theme::BTN_R + 3)) return i;
    }
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
    g.term.resize((cr.w - 12) / g.term.cell_w(), (cr.h - 12) / g.term.cell_h());
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
    case Kind::About: create_window(kind, "About Lumen", {x + 120, y + 40, 460, 400}); break;
    case Kind::SystemMonitor: create_window(kind, "System Monitor", {x + 60, y + 20, 520, 360}); break;
    case Kind::MemoryMap: create_window(kind, "Memory Map", {x + 30, y + 10, 700, 480}); break;
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
    fill_rect_rounded(s, {theme::SHADOW_PAD, theme::SHADOW_PAD, w.frame.w, w.frame.h}, theme::RADIUS, theme::SHADOW);
    blur_box(s, theme::SHADOW_BLUR, g.scratch.pixels);
    blur_box(s, theme::SHADOW_BLUR / 2 + 1, g.scratch.pixels);
}

// ---------------------------------------------------------------- paint ---
void paint_lines_header(Surface& s, const char* heading) {
    fill_rect(s, s.bounds(), theme::CONTENT_BG);
    draw_text(s, g.font, 16, 14, heading, theme::ACCENT);
    draw_hline(s, 16, s.width - 17, 34, rgba(255, 255, 255, 30));
}

int paint_kv(Surface& s, int y, const char* key, const char* value) {
    draw_text(s, g.font, 16, y, key, theme::TEXT_MUTED);
    draw_text(s, g.font, 16 + 22 * 8, y, value, theme::TEXT);
    return y + 22;
}

void paint_about(Surface& s) {
    fill_rect(s, s.bounds(), theme::CONTENT_BG);
    fill_gradient_radial(s, s.bounds(), s.width - 60, 40, 260, rgba(96, 165, 250, 60), rgba(0, 0, 0, 0));
    // Wordmark: 16x32 font at 2x.
    const char* mark = "Lumen";
    int x = 24;
    for (int i = 0; mark[i]; i++) {
        draw_char_scaled(s, g.font_big, x, 22, mark[i], 2, theme::TEXT);
        x += g.font_big.width * 2;
    }
    fill_circle_aa(s, x + 18, 54, 8, theme::ACCENT);
    draw_text(s, g.font, 26, 96, "A hybrid-kernel operating system, built from scratch.", theme::TEXT_MUTED);
    char line[96];
    int y = 130;
    ksnprintf(line, sizeof line, "%s (x86-64)", lumen_version());
    y = paint_kv(s, y, "Version", line);
    y = paint_kv(s, y, "Built", lumen_build_date());
    y = paint_kv(s, y, "Bootloader", g_boot_info.bootloader);
    char brand[49];
    cpuid_brand(brand);
    y = paint_kv(s, y, "CPU", brand);
    ksnprintf(line, sizeof line, "%lu cpu(s), %lu MiB RAM", (unsigned long)g_boot_info.cpu_count,
              (unsigned long)(g_boot_info.total_bytes() / MIB));
    y = paint_kv(s, y, "Machine", line);
    ksnprintf(line, sizeof line, "%dx%d, 32 bpp", g.W, g.H);
    y = paint_kv(s, y, "Display", line);
    y = paint_kv(s, y, "Language", "Spec (compiler in progress)");
    draw_text(s, g.font, 16, s.height - 30, "Alt+Tab switch, Alt+F4 close, Super = menu, Super+T terminal",
              theme::TEXT_MUTED);
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
    ksnprintf(line, sizeof line, "%lu online (1 in use until phase 8)", (unsigned long)g_boot_info.cpu_count);
    y = paint_kv(s, y, "Cores", line);

    PmmStats pm = pmm_stats();
    u64 used_mb = pm.used_frames * PAGE_SIZE / MIB, total_mb = pm.usable_frames * PAGE_SIZE / MIB;
    ksnprintf(line, sizeof line, "%lu / %lu MiB used", (unsigned long)used_mb, (unsigned long)total_mb);
    y = paint_kv(s, y, "Memory", line);
    Rect bar{16, y + 2, s.width - 32, 14};
    fill_rect_rounded(s, bar, 7, rgba(255, 255, 255, 25));
    int fillw = total_mb ? (int)((u64)bar.w * used_mb / total_mb) : 0;
    if (fillw > 0) fill_rect_rounded(s, {bar.x, bar.y, fillw < 14 ? 14 : fillw, bar.h}, 7, theme::ACCENT);
    y += 30;
    ksnprintf(line, sizeof line, "%lu frames, largest free run %lu MiB", (unsigned long)pm.free_frames,
              (unsigned long)(pm.largest_free_run * PAGE_SIZE / MIB));
    y = paint_kv(s, y, "Free", line);

    ksnprintf(line, sizeof line, "%lu frames, last %lu us, %lu px copied", (unsigned long)g.stats.frames,
              (unsigned long)g.stats.last_frame_us, (unsigned long)g.stats.last_present_pixels);
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
    draw_text(s, g.font, 16, y, "start            end              size       type", theme::TEXT_MUTED);
    y += 20;
    for (usize i = 0; i < bi.region_count && y < s.height - 40; i++) {
        const MemoryRegion& r = bi.regions[i];
        ksnprintf(line, sizeof line, "%016lx %016lx %8lu KiB %s", (unsigned long)r.base,
                  (unsigned long)(r.base + r.length - 1), (unsigned long)(r.length / KIB),
                  memory_type_name(r.type));
        Color c = r.type == MemoryType::Usable ? theme::TEXT : theme::TEXT_MUTED;
        draw_text(s, g.font, 16, y, line, c);
        y += 18;
    }
    ksnprintf(line, sizeof line, "%lu regions, %lu MiB usable", (unsigned long)bi.region_count,
              (unsigned long)(bi.usable_bytes() / MIB));
    draw_text(s, g.font, 16, s.height - 30, line, theme::ACCENT);
}

void paint_window_content(Window& w) {
    Surface view = content_view(w);
    int idx = window_index(&w);
    Rect cr = content_rect(w);
    if (w.kind == Kind::Terminal) {
        Rect d = g.term.paint(view, g.focus == idx, w.needs_paint);
        if (!d.empty()) damage(d.translated(cr.x, cr.y));
        w.needs_paint = false;
        g.term_dirty = false;
        return;
    }
    if (!w.needs_paint) return;
    switch (w.kind) {
    case Kind::About: paint_about(view); break;
    case Kind::SystemMonitor: paint_sysmon(view); break;
    case Kind::MemoryMap: paint_memmap(view); break;
    default: break;
    }
    w.needs_paint = false;
    damage(cr);
}

void draw_button(Surface& s, Point c, Color fill, int glyph, bool hover) {
    fill_circle_aa(s, c.x, c.y, theme::BTN_R, fill);
    if (!hover) return;
    Color gc = theme::BTN_GLYPH;
    switch (glyph) {
    case 0:     // close: x
        draw_line(s, c.x - 3, c.y - 3, c.x + 3, c.y + 3, gc);
        draw_line(s, c.x + 3, c.y - 3, c.x - 3, c.y + 3, gc);
        break;
    case 1:     // maximise: square
        stroke_rect(s, {c.x - 3, c.y - 3, 7, 7}, gc);
        break;
    case 2:     // minimise: bar
        draw_hline(s, c.x - 3, c.x + 3, c.y, gc);
        break;
    }
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
    constexpr int R = theme::RADIUS;
    u32 saved_l[R][R], saved_r[R][R];
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
    // Mask the bottom corners back to the rounded shape.
    for (int dy = 0; dy < R; dy++) {
        int inset = corner_inset(R, dy);
        int y = w.frame.bottom() - 1 - dy;
        for (int dx = 0; dx < inset; dx++) {
            int xl = w.frame.x + dx, xr = w.frame.right() - 1 - dx;
            if (back.clip.contains(xl, y)) back.row(y)[xl] = saved_l[dy][dx];
            if (back.clip.contains(xr, y)) back.row(y)[xr] = saved_r[dy][dx];
        }
    }
    stroke_rect_rounded(back, w.frame, R, focused ? theme::BORDER_FOCUS : theme::BORDER);
    draw_hline(back, cr.x, cr.right() - 1, cr.y - 1, rgba(255, 255, 255, 18));

    // Title text and buttons.
    int text_w = w.frame.w - 16 - 3 * theme::BTN_GAP - 24;
    draw_text_ellipsis(back, g.font, w.frame.x + 14, w.frame.y + theme::BORDER_W + (theme::TITLE_H - g.font.height) / 2,
                       w.title, text_w, focused ? theme::TITLE_TEXT : theme::TEXT_MUTED);
    bool hover_bar = w.frame.contains(g.mx, g.my) && g.my < w.frame.y + theme::TITLE_H + theme::BORDER_W;
    int hb = hover_bar ? button_at(w, g.mx, g.my) : -1;
    Color dim = rgba(120, 124, 140, 255);
    draw_button(back, button_centre(w, 0), focused || hover_bar ? theme::BTN_CLOSE : dim, 0, hb == 0);
    draw_button(back, button_centre(w, 1), focused || hover_bar ? theme::BTN_MAX : dim, 1, hb == 1);
    draw_button(back, button_centre(w, 2), focused || hover_bar ? theme::BTN_MIN : dim, 2, hb == 2);
}

void draw_panel(Surface& back) {
    Rect pr = panel_rect();
    fill_rect(back, pr, theme::PANEL);
    draw_hline(back, 0, g.W - 1, pr.y, theme::PANEL_LINE);

    // Launcher button.
    Rect lr = launcher_rect();
    bool lhover = lr.contains(g.mx, g.my) || g.menu_open;
    fill_rect_rounded(back, lr, 8, lhover ? theme::ACCENT : theme::ACCENT_DARK);
    fill_circle_aa(back, lr.x + 16, lr.y + lr.h / 2, 5, rgb(255, 255, 255));
    draw_text(back, g.font, lr.x + 30, lr.y + (lr.h - g.font.height) / 2, "Lumen", rgb(255, 255, 255));

    // Task buttons.
    g.task_count = 0;
    int x = lr.right() + 12;
    for (int i = 0; i < g.order_count; i++) {
        Window& w = g.windows[g.order[i]];
        if (!w.used) continue;
        Rect tr{x, pr.y + 6, 170, pr.h - 12};
        if (tr.right() > clock_rect().x - 8) break;
        bool focused = g.focus == g.order[i];
        Color bg = focused ? theme::TASK_FOCUS : (w.minimised ? rgba(255, 255, 255, 8) : theme::TASK_BG);
        if (tr.contains(g.mx, g.my)) bg = rgba(255, 255, 255, 40);
        fill_rect_rounded(back, tr, 8, bg);
        if (focused) fill_rect_rounded(back, {tr.x + 10, tr.bottom() - 4, tr.w - 20, 2}, 1, theme::ACCENT);
        fill_circle_aa(back, tr.x + 14, tr.y + tr.h / 2, 4, w.kind == Kind::Terminal ? theme::ACCENT : theme::BTN_MAX);
        draw_text_ellipsis(back, g.font, tr.x + 26, tr.y + (tr.h - g.font.height) / 2, w.title, tr.w - 34,
                           w.minimised ? theme::TEXT_MUTED : theme::TEXT);
        g.task_rects[g.task_count] = tr;
        g.task_win[g.task_count] = g.order[i];
        g.task_count++;
        x += tr.w + 6;
    }

    // Clock, date, memory.
    DateTime now = rtc_now();
    char buf[64];
    ksnprintf(buf, sizeof buf, "%02u:%02u:%02u", now.hour, now.minute, now.second);
    int tw = measure_text(g.font, buf);
    draw_text(back, g.font, g.W - 16 - tw, pr.y + 5, buf, theme::TEXT);
    ksnprintf(buf, sizeof buf, "%04u-%02u-%02u", now.year, now.month, now.day);
    tw = measure_text(g.font, buf);
    draw_text(back, g.font, g.W - 16 - tw, pr.y + 22, buf, theme::TEXT_MUTED);
    PmmStats pm = pmm_stats();
    u32 pct = pm.usable_frames ? (u32)(pm.used_frames * 100 / pm.usable_frames) : 0;
    ksnprintf(buf, sizeof buf, "RAM %u%%", pct);
    tw = measure_text(g.font, buf);
    draw_text(back, g.font, g.W - 120 - tw, pr.y + (pr.h - g.font.height) / 2, buf, theme::TEXT_MUTED);
    draw_vline(back, g.W - 108, pr.y + 10, pr.bottom() - 10, theme::PANEL_LINE);
}

void draw_menu(Surface& back) {
    Rect mr = menu_rect();
    fill_rect_rounded(back, mr.translated(0, 4), 12, rgba(0, 0, 0, 90));
    fill_rect_rounded(back, mr, 12, theme::MENU_BG);
    stroke_rect_rounded(back, mr, 12, theme::BORDER);
    for (int i = 0; i < MENU_COUNT; i++) {
        Rect ir{mr.x + 8, mr.y + 8 + i * theme::MENU_ITEM_H, mr.w - 16, theme::MENU_ITEM_H};
        if (i == g.menu_hover) fill_rect_rounded(back, ir, 8, theme::MENU_HOVER);
        fill_circle_aa(back, ir.x + 16, ir.y + ir.h / 2, 5, i == g.menu_hover ? theme::ACCENT : theme::TEXT_MUTED);
        draw_text(back, g.font, ir.x + 34, ir.y + (ir.h - g.font.height) / 2, MENU_ITEMS[i].label, theme::TEXT);
    }
}

// Framebuffer rows are copied with plain 32-bit stores, never `rep movsb`:
// VirtualBox's Hyper-V backend emulates string instructions that touch video
// memory one byte per exit and effectively never finishes a frame.
inline void copy_to_fb(int x, int y, int w) {
    const u32* src = g.back.row(y) + x;
    u32* dst = g.fb.row(y) + x;
    for (int i = 0; i < w; i++) dst[i] = src[i];
}

void draw_cursor_on_fb() {
    Rect cr = cursor_rect(g.mx, g.my);
    blit_alpha(g.fb, cr.x, cr.y, g_cursor);
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
    if (g.damage_all) {
        compose({0, 0, g.W, g.H});
    } else {
        for (int i = 0; i < g.damage_count; i++) compose(g.damage[i]);
    }
    g.damage_all = false;
    g.damage_count = 0;
    g.stats.frames++;
    g.stats.last_frame_us = refclock_now_us() - t0;
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
            int i = (y - mr.y - 8) / theme::MENU_ITEM_H;
            h.item = (i >= 0 && i < MENU_COUNT) ? i : -1;
            return h;
        }
    }
    if (panel_rect().contains(x, y)) {
        h.what = Hit::Panel;
        if (launcher_rect().contains(x, y)) h.what = Hit::Launcher;
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
        if (x < w.frame.x + theme::RESIZE_BAND) edges |= 1;
        if (x >= w.frame.right() - theme::RESIZE_BAND) edges |= 2;
        if (y < w.frame.y + theme::RESIZE_BAND) edges |= 4;
        if (y >= w.frame.bottom() - theme::RESIZE_BAND) edges |= 8;
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
    damage(menu_rect().inset(-8).translated(0, 4));
    damage(launcher_rect());
}

void open_menu() {
    g.menu_open = true;
    g.menu_hover = -1;
    damage(menu_rect().inset(-8).translated(0, 4));
    damage(launcher_rect());
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
        if (h.item >= 0) {
            close_menu();
            open_kind(MENU_ITEMS[h.item].kind);
        }
        break;
    case Hit::Launcher:
        if (g.menu_open) close_menu();
        else open_menu();
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
}

void on_motion() {
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
        int hover = -1;
        if (mr.contains(g.mx, g.my)) {
            int i = (g.my - mr.y - 8) / theme::MENU_ITEM_H;
            if (i >= 0 && i < MENU_COUNT) hover = i;
        }
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
    usize next = (g.term_in_head + 1) % sizeof g.term_in;
    if (next == g.term_in_tail) return;
    g.term_in[g.term_in_head] = c;
    g.term_in_head = next;
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
        if (!e.pressed) continue;
        bool alt = e.mods & mod::ALT, super_ = e.mods & mod::SUPER;
        if (alt && e.key == key::F1 + 3) {                 // Alt+F4
            if (g.focus >= 0) destroy_window(g.focus);
            continue;
        }
        if (alt && e.key == key::TAB) { cycle_focus(); continue; }
        if (e.key == key::SUPER) { if (g.menu_open) close_menu(); else open_menu(); continue; }
        if (super_ && e.ascii == 't') { close_menu(); open_kind(Kind::Terminal); continue; }
        if (super_ && e.ascii == 'm') { if (g.focus >= 0) toggle_maximise(g.windows[g.focus]); continue; }
        if (e.key == key::ESCAPE && g.menu_open) { close_menu(); continue; }
        if (g.menu_open) {
            if (e.key == key::DOWN) { g.menu_hover = (g.menu_hover + 1) % MENU_COUNT; damage(menu_rect()); continue; }
            if (e.key == key::UP) { g.menu_hover = (g.menu_hover + MENU_COUNT - 1) % MENU_COUNT; damage(menu_rect()); continue; }
            if (e.key == key::ENTER && g.menu_hover >= 0) { Kind k = MENU_ITEMS[g.menu_hover].kind; close_menu(); open_kind(k); continue; }
        }
        if (g.focus >= 0 && g.focus == g.term_win && e.ascii) term_input_push(e.ascii);
    }
}

void process_mouse() {
    MouseEvent e;
    while (ps2mouse_poll(&e)) {
        if (e.dx || e.dy) {
            move_cursor(g.mx + e.dx, g.my + e.dy);
            on_motion();
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
void build_wallpaper() {
    Surface& s = g.wall;
    fill_gradient_vertical(s, s.bounds(), theme::WALL_TOP, theme::WALL_BOTTOM);
    fill_gradient_radial(s, s.bounds(), g.W * 4 / 5, g.H / 5, g.W / 2, theme::GLOW_BLUE, rgba(0, 0, 0, 0));
    fill_gradient_radial(s, s.bounds(), g.W / 6, g.H * 5 / 6, g.W * 2 / 5, theme::GLOW_VIOLET, rgba(0, 0, 0, 0));
    // Faint wordmark.
    const char* mark = "Lumen";
    int scale = g.W >= 1600 ? 4 : 3;
    int mw = 5 * g.font_big.width * scale;
    int x = (g.W - mw) / 2, y = (g.H - theme::PANEL_H - g.font_big.height * scale) / 2;
    for (int i = 0; mark[i]; i++) {
        draw_char_scaled(s, g.font_big, x, y, mark[i], scale, rgba(255, 255, 255, 26));
        x += g.font_big.width * scale;
    }
    char line[64];
    ksnprintf(line, sizeof line, "Lumen %s  |  preview desktop", lumen_version());
    int tw = measure_text(g.font, line);
    draw_text(s, g.font, g.W - tw - 16, g.H - theme::PANEL_H - 26, line, rgba(255, 255, 255, 70));
}

void build_cursor() {
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            char c = CURSOR_ART[y][x];
            g_cursor_px[y * CURSOR_W + x] = c == '#' ? rgba(0, 0, 0, 230) : c == 'o' ? rgb(255, 255, 255) : 0;
        }
    }
    g_cursor = Surface(g_cursor_px, CURSOR_W, CURSOR_H, CURSOR_W);
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
    g.font = font_from_psf2(_binary_font_8x16_start);
    g.font_big = font_from_psf2(_binary_font_16x32_start);
    if (!g.font.valid() || !g.font_big.valid()) {
        kprintf("gui: fonts invalid\n");
        return false;
    }

    u32* back = alloc_pixels((usize)g.W * g.H);
    u32* wall = alloc_pixels((usize)g.W * g.H);
    usize scratch_px = (usize)(g.W + 2 * theme::SHADOW_PAD) * (g.H + 2 * theme::SHADOW_PAD);
    u32* scratch = alloc_pixels(scratch_px);
    constexpr int TERM_COLS = 200, TERM_ROWS = 40;
    u8* cells = (u8*)kmalloc((usize)TERM_COLS * TERM_ROWS);
    if (!back || !wall || !scratch || !cells) {
        kprintf("gui: out of memory for screen buffers\n");
        return false;
    }
    g.back = Surface(back, g.W, g.H, g.W);
    g.wall = Surface(wall, g.W, g.H, g.W);
    g.scratch = Surface(scratch, g.W + 2 * theme::SHADOW_PAD, g.H + 2 * theme::SHADOW_PAD, g.W + 2 * theme::SHADOW_PAD);
    g.term_cells = cells;
    g.term.init(g.term_cells, TERM_COLS, TERM_ROWS, g.font);

    build_wallpaper();
    build_cursor();
    g.mx = g.W / 2;
    g.my = g.H / 2;

    fbconsole_disable();
    g.active = true;
    create_window(Kind::About, "About Lumen", {g.W - 520, 90, 460, 400});
    create_window(Kind::Terminal, "Terminal", {56, 56, 740, 470});
    damage_all();
    flush_frame();
    kprintf("gui: desktop up at %dx%d, %d windows, first frame %lu us (%lu px to the framebuffer)\n", g.W, g.H,
            g.order_count, (unsigned long)g.stats.last_frame_us, (unsigned long)g.stats.last_present_pixels);
    return true;
}

bool gui_active() { return g.active; }

void gui_terminal_putc(char c) {
    if (!g.active) return;
    g.term.putc(c);
    g.term_dirty = true;
}

int gui_terminal_getc() {
    if (g.term_in_head == g.term_in_tail) return -1;
    char c = g.term_in[g.term_in_tail];
    g.term_in_tail = (g.term_in_tail + 1) % sizeof g.term_in;
    return (unsigned char)c;
}

void gui_pump() {
    if (!g.active) return;
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
    if (now_us - g.last_frame_us < FRAME_US) return;

    for (int i = 0; i < g.order_count; i++) {
        Window& w = g.windows[g.order[i]];
        if (!w.used || w.minimised) continue;
        if (w.needs_paint || (w.kind == Kind::Terminal && g.term_dirty)) paint_window_content(w);
    }
    if (g.damage_all || g.damage_count) flush_frame();
}

GuiStats gui_stats() {
    GuiStats s = g.stats;
    s.windows = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) s.windows += g.windows[i].used;
    return s;
}

void gui_emergency_text_mode() {
    if (!g.active) return;
    g.active = false;
    fbconsole_enable();
}
