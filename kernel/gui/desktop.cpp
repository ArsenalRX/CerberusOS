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
#include <fs/fs.h>
#include <fs/vfs.h>
#include <drivers/lapic.h>
#include <arch/x86_64/smp.h>
#include <drivers/ps2kbd.h>
#include <lib/console.h>
#include <drivers/ps2mouse.h>
#include <drivers/refclock.h>
#include <drivers/rtc.h>
#include <gfx/gfx.h>
#include <gui/calc.h>
#include <gui/desktop.h>
#include <gui/files.h>
#include <gui/notes.h>
#include <gui/terminal.h>
#include <lib/kprintf.h>
#include <lib/csprng.h>
#include <proc/signal.h>
#include <lib/panic.h>
#include <lib/sha256.h>
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

// Wakes the compositor thread (defined with it, below).
void g_compositor_wake_hint();

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
constexpr int WALLPAPER_COUNT = 6;
const char* const WALLPAPER_NAMES[WALLPAPER_COUNT] = {"Nebula", "Aurora", "Ember", "Ocean", "Sunset", "Graphite"};
constexpr int MAX_RADIUS = 16;

struct Prefs {
    int accent = 0;                 // an index into ACCENTS, or ACCENT_COUNT: the custom hue
    int custom_hue = 1000;          // 0..1535 round the colour wheel
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
    int fps = 144;                  // most frames per second the compositor presents
    int tz_minutes = 0;             // local time = the clock's time plus this
    int layout = 0;                 // keyboard: ps2kbd layouts
    int repeat_delay = 1, repeat_rate = 2;
    // Lock screen: SHA-256(salt + password) and the salt, as hex; empty = no password.
    char lock_hash[65] = {};
    char lock_salt[33] = {};
    bool night_light = false;       // a warm tint over the whole screen
    int autolock_min = 0;           // lock the screen after this many idle minutes; 0 = never
};
Prefs g_prefs;

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
// A hue as an accent colour: lightened to sit with the fixed ones.
inline Color hue_accent(int hue) { return mix(hue_color((u32)hue), rgb(255, 255, 255), 80); }
inline Color accent() { return g_prefs.accent < ACCENT_COUNT ? ACCENTS[g_prefs.accent].c : hue_accent(g_prefs.custom_hue); }
inline Color accent_dark() {
    Color c = accent();
    return rgb((u8)(red_of(c) * 5 / 9), (u8)(green_of(c) * 5 / 9), (u8)(blue_of(c) * 5 / 9));
}
inline int radius() { return g_prefs.radius; }
inline bool fx_animated() { return g_prefs.fx >= BorderFx::Breathing; }

struct Mode {
    int w, h;
};
// Common screens, 4:3 to 32:9 ultrawide. Only those the adapter can hold are offered.
const Mode MODES[] = {{800, 600},   {1024, 768},  {1280, 720},  {1280, 800},  {1280, 1024}, {1366, 768},
                      {1440, 900},  {1600, 900},  {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1080},
                      {2560, 1440}, {2560, 1600}, {3440, 1440}, {3840, 1600}, {3840, 2160}, {5120, 1440}};
constexpr int MODE_COUNT = sizeof MODES / sizeof MODES[0];
const int FPS_CHOICES[] = {30, 60, 75, 90, 120, 144};
constexpr int FPS_COUNT = sizeof FPS_CHOICES / sizeof FPS_CHOICES[0];

// Minimum time between frames. Input wakes the compositor at once, so a drag
// is drawn as soon as the mouse reports, up to the chosen frame rate.
inline u64 frame_us() { return 1000000 / (u64)g_prefs.fps; }
constexpr int MAX_WINDOWS = 12;
constexpr int MAX_DAMAGE = 24;
// Windows and the launcher fade and slide into place over this long.
constexpr u64 ANIM_US = 160000;
constexpr int SLIDE_OPEN = 12, SLIDE_MINIMISE = 30, SLIDE_MENU = 10;
enum : u8 { ANIM_NONE, ANIM_IN, ANIM_OUT };

enum class Kind { Terminal, About, SystemMonitor, MemoryMap, Settings, Files, Notes, Calculator };

struct Window {
    bool used = false;
    u32 id = 0;
    Kind kind = Kind::About;
    char title[48] = {};
    Rect frame{0, 0, 0, 0};
    Rect restore{0, 0, 0, 0};
    bool maximised = false, minimised = false;
    u8 snapped = 0;             // 1 left half, 2 right half (restore holds the old frame)
    u32* pixels = nullptr;
    Surface content;            // full screen-sized buffer; view via content_view()
    u32* shadow = nullptr;
    int shadow_w = 0, shadow_h = 0;
    bool needs_paint = true;
    int min_w = 240, min_h = 140;
    u8 anim = ANIM_NONE;        // appearing, or (minimised) on its way out
    u64 anim_start = 0;
    u8 desk = 0;                // the virtual desktop it is on (0..DESKS-1)
};
constexpr int DESKS = 4;

// A closed window, kept only until it has faded away. It owns the buffers
// the window had.
struct Ghost {
    Window w;
    u64 start;
};
constexpr int MAX_GHOSTS = 4;
Ghost g_ghosts[MAX_GHOSTS];
int g_ghost_count = 0;

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
    {"Files", "browse folders disk explorer manager", Kind::Files, rgb(250, 204, 21)},
    {"Notes", "text editor write edit", Kind::Notes, rgb(45, 212, 191)},
    {"Calculator", "maths math calc numbers hex binary", Kind::Calculator, rgb(248, 113, 113)},
    {"Settings", "theme colours colors wallpaper background border rgb display resolution screen appearance",
     Kind::Settings, rgb(167, 139, 250)},
    {"System Monitor", "cpu memory ram tasks performance", Kind::SystemMonitor, rgb(74, 222, 128)},
    {"Memory Map", "physical ram regions", Kind::MemoryMap, rgb(251, 146, 60)},
    {"About Cerberus", "version information", Kind::About, rgb(244, 114, 182)},
};
constexpr int APP_COUNT = sizeof APPS / sizeof APPS[0];
// Launcher items that are not applications.
constexpr int ITEM_NONE = -1, ITEM_POWER = -2, ITEM_RESTART = -3;

// The built-in applications (one window each).
CalcApp g_calc;
NotesApp g_notes;
FilesApp g_files;

char g_search[24];                  // what has been typed into the launcher
int g_search_len = 0;
int g_found[APP_COUNT];             // indices into APPS that match it
int g_found_count = 0;

enum class Drag { None, Move, Resize, Select };

// The desktop clipboard: one piece of text. Selecting text in the terminal
// copies it; Ctrl+C / Ctrl+Shift+C copy; Ctrl+V / Ctrl+Shift+V paste into
// whatever has the keyboard (terminal, launcher search, calendar line).
char g_clipboard[4096];
usize g_clip_len = 0;

void clipboard_set(const char* text, usize len) {
    if (len >= sizeof g_clipboard) len = sizeof g_clipboard - 1;
    memcpy(g_clipboard, text, len);
    g_clipboard[len] = 0;
    g_clip_len = len;
}

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
    int cur_desk = 0;           // the virtual desktop shown

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
    u8 menu_anim = ANIM_NONE;
    u64 menu_anim_start = 0;
    bool hue_drag = false;      // the mouse is held on the Settings hue strip
    int snap_preview = 0;       // while dragging at an edge: 1 left, 2 right, 3 top (maximise)
    u64 last_click_us = 0;      // for double-clicks on a title bar
    int last_click_win = -1;
    u64 last_content_us = 0;    // and inside a window's content
    int last_content_win = -1;
    bool switcher_open = false; // Alt+Tab
    int switcher_sel = 0;
    int switcher_wins[MAX_WINDOWS];
    int switcher_count = 0;
    bool ctx_open = false;      // the right-click menu on the desktop
    int ctx_x = 0, ctx_y = 0;
    int ctx_win = -1;           // ... or on a taskbar button: the window it is about
    u32 shown_before = 0;       // Super+D: the windows it hid, to bring back
    bool tray_open = false;     // the quick-settings panel above the tray button
    u8 tray_anim = ANIM_NONE;
    u64 tray_anim_start = 0;
    bool cal_open = false;      // the calendar above the clock
    u8 cal_anim = ANIM_NONE;
    u64 cal_anim_start = 0;
    int cal_year = 2026, cal_month = 1;     // the month shown
    int sel_year = 2026, sel_month = 1, sel_day = 1;    // the day picked
    bool super_down = false;    // Super is held...
    bool super_chord = false;   // ...and another key was pressed with it
    int menu_hover = -1;
    int press_task = -1;

    Rect task_rects[MAX_WINDOWS];
    int task_win[MAX_WINDOWS];
    int task_count = 0;

    u32 screenshot_count = 0;

    Terminal term;
    u8* term_cells = nullptr;
    int term_win = -1;
    bool term_dirty = false;
    char term_in[256];
    volatile usize term_in_head = 0, term_in_tail = 0;

    u8 last_second = 255;
    u64 last_frame_us = 0;
    u64 last_input_us = 0;      // for the auto-lock
    u64 power_armed_us = 0;     // the launcher's Power off was pressed once
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
constexpr int THUMB_W = 120, THUMB_H = 75;
u32 g_thumb_px[WALLPAPER_COUNT][THUMB_W * THUMB_H];

// Controls of the Settings window, recorded as it is painted so a click can
// be matched to what it hit.
enum : u8 {
    ACT_TAB, ACT_ACCENT, ACT_WALL, ACT_RADIUS, ACT_GLASS, ACT_FLOAT, ACT_CLOCK12, ACT_SECONDS,
    ACT_FX, ACT_BCOLOR, ACT_BWIDTH, ACT_SPEED, ACT_BALL, ACT_GLOW, ACT_MODE, ACT_HUE, ACT_FPS, ACT_TZ, ACT_FORMAT,
    ACT_LAYOUT, ACT_RDELAY, ACT_RRATE, ACT_PW_FIELD, ACT_PW_SET, ACT_PW_CLEAR, ACT_LOCK, ACT_AUTOLOCK,
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
int g_settings_scroll = 0;          // pixels the tab's cards are scrolled up (B-012)
int g_settings_height = 0;          // the tab's content height, from the last paint
bool g_measuring = false;           // painting only to find out how tall a card is
int g_format_confirm = -1;          // Storage: the disk whose "erase" button was pressed once
// Security: the password being typed in Settings, and on the lock screen.
char g_pw_text[40];
int g_pw_len = 0;
bool g_pw_editing = false;          // the Settings field has the keyboard
bool g_locked = false;
bool g_pw_wrong = false;
u64 g_unlock_retry_us = 0;          // a wrong password costs a second
bool g_prefs_dirty = false;         // settings changed and not yet written to /data
u64 g_prefs_dirty_us = 0;
int g_wrap_right = 0;               // where a row of chips wraps

// ------------------------------------------------------------- calendar --
// Reminders live in memory until there is somewhere per-user to keep them
// (phase 15), like the preferences.
struct Reminder {
    bool used;
    u16 year;
    u8 month, day, hour, minute;
    u8 repeat;                      // 0 once, 1 daily, 2 weekly
    char text[40];
};
constexpr int MAX_REMINDERS = 32;
Reminder g_reminders[MAX_REMINDERS];
char g_rem_text[40];                // what is being typed into the calendar
int g_rem_len = 0;
// The calendar's controls, recorded as it is painted like the Settings ones.
enum : u8 { CAL_PREV, CAL_NEXT, CAL_TODAY, CAL_DAY, CAL_DELETE };
Ctl g_cal_ctls[64];
int g_cal_ctl_count = 0;
constexpr int CAL_W = 324;

// Notifications: a card in the top-right corner for a few seconds, or until
// it is clicked.
struct Toast {
    bool used;
    char title[32];
    char text[64];
    u64 start, until;
    char snooze_text[40];       // non-empty: a reminder that "Snooze" repeats in five minutes
};
constexpr int MAX_TOASTS = 4;
Toast g_toasts[MAX_TOASTS];
constexpr int TOAST_W = 320, TOAST_H = 66, TOAST_GAP = 10;
constexpr u64 TOAST_US = 8000000;
// Posted from other threads (the shell's `notify`), taken by the compositor.
Toast g_pending[MAX_TOASTS];
volatile u32 g_pending_head = 0, g_pending_tail = 0;
// What the tray shows: the last notifications, and CPU load per CPU.
struct Past {
    char title[32];
    char text[64];
    u8 hour, minute;
};
constexpr int HISTORY_MAX = 8;
Past g_history[HISTORY_MAX];
int g_history_count = 0;
constexpr int MAX_CPUS_SHOWN = 8;
u64 g_cpu_prev_ticks[MAX_CPUS_SHOWN], g_cpu_prev_idle[MAX_CPUS_SHOWN];
u8 g_cpu_busy[MAX_CPUS_SHOWN];      // percent, refreshed once a second
// The tray's controls, recorded as it is painted.
enum : u8 { TRAY_NIGHT, TRAY_DESKTOP, TRAY_ACCENT, TRAY_FPS, TRAY_CLEAR, TRAY_LOCK, TRAY_SETTINGS };
Ctl g_tray_ctls[32];
int g_tray_ctl_count = 0;
constexpr int TRAY_W = 340;

const char* const MONTH_NAMES[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};
const char* const DAY_NAMES[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
const char* const DAY_LONG[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

int days_in_month(int y, int m) {
    static const u8 D[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
    return m == 2 && leap ? 29 : D[m - 1];
}

// 0 = Monday.
int weekday(int y, int m, int d) {
    static const int T[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    y -= m < 3;
    int sunday0 = (y + y / 4 - y / 100 + y / 400 + T[m - 1] + d) % 7;
    return (sunday0 + 6) % 7;
}

// The clock's time shifted into the chosen time zone.
DateTime local_now() {
    i64 t = (i64)datetime_to_unix(rtc_now()) + (i64)g_prefs.tz_minutes * 60;
    return unix_to_datetime(t < 0 ? 0 : (u64)t);
}

bool has_reminder(int y, int m, int d) {
    for (const Reminder& r : g_reminders)
        if (r.used && r.year == y && r.month == m && r.day == d) return true;
    return false;
}
Rect g_hue_rect{0, 0, 0, 0};        // the hue strip, in the Settings window's content
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

// Everything a window can cover while it slides in or out.
Rect anim_bounds(const Window& w) {
    Rect b = visual_bounds(w);
    b.h += SLIDE_MINIMISE;
    return b;
}

// How far an animation started at `start` has come, 0..255, slowing down
// towards the end.
u32 anim_eased(u64 start) {
    u64 elapsed = refclock_now_us() - start;
    if (elapsed >= ANIM_US) return 255;
    u32 left = 255 - (u32)(elapsed * 255 / ANIM_US);
    return 255 - left * left * left / (255 * 255);
}

void start_anim(Window& w, u8 anim) {
    w.anim = anim;
    w.anim_start = refclock_now_us();
    damage(anim_bounds(w));
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
// What the launcher covers on the screen, shadow and slide included.
Rect menu_area() {
    Rect a = menu_rect().inset(-8);
    a.h += 4 + SLIDE_MENU;
    return a;
}
Rect cal_rect() {
    int h = 48 + 22 + 6 * 32 + 10 + 150;
    Rect b = bar_rect();
    return {b.right() - 8 - CAL_W, g.H - theme::PANEL_H - h - 8, CAL_W, h};
}
Rect cal_area() {
    Rect a = cal_rect().inset(-8);
    a.h += 4 + SLIDE_MENU;
    return a;
}
// The right-click menu's items and its place (kept on the screen).
const char* const CTX_ITEMS[] = {"Terminal", "Files", "Notes", "Settings", "Change wallpaper", "Show desktop", "About Cerberus"};
const char* const TASK_ITEMS[] = {"Restore", "Minimise", "Maximise", "Snap left", "Snap right", "Close"};
constexpr int CTX_COUNT = sizeof CTX_ITEMS / sizeof CTX_ITEMS[0];
constexpr int TASK_COUNT = sizeof TASK_ITEMS / sizeof TASK_ITEMS[0];
inline int ctx_item_count() { return g.ctx_win >= 0 ? TASK_COUNT : CTX_COUNT; }
inline const char* ctx_item(int i) { return g.ctx_win >= 0 ? TASK_ITEMS[i] : CTX_ITEMS[i]; }
constexpr int CTX_W = 200, CTX_ITEM_H = 32;
Rect ctx_rect() {
    int h = ctx_item_count() * CTX_ITEM_H + 12;
    int x = g.ctx_x, y = g.ctx_y;
    if (x + CTX_W > g.W - 4) x = g.W - 4 - CTX_W;
    if (y + h > g.H - theme::PANEL_H) y = g.H - theme::PANEL_H - h;
    return {x, y, CTX_W, h};
}
Rect ctx_area() { return ctx_rect().inset(-8).translated(0, 2); }
// The Alt+Tab switcher: a row of tiles in the middle of the screen.
constexpr int SW_TILE = 128, SW_TILE_H = 104;
Rect switcher_rect() {
    int w = g.switcher_count * (SW_TILE + 8) + 16;
    return {(g.W - w) / 2, (g.H - theme::PANEL_H - SW_TILE_H - 24) / 2, w, SW_TILE_H + 24};
}
Rect snap_target(int side) {
    Rect wa = work_area();
    if (side == 1) return {wa.x + 6, wa.y + 6, wa.w / 2 - 9, wa.h - 12};
    if (side == 2) return {wa.x + wa.w / 2 + 3, wa.y + 6, wa.w - wa.w / 2 - 9, wa.h - 12};
    return {wa.x + 6, wa.y + 6, wa.w - 12, wa.h - 12};
}
Rect tray_button_rect() {
    Rect b = bar_rect();
    int sep = b.right() - 16 - 104;
    return {sep - 16 - 64 - 12 - 28, bar_mid() - 12, 24, 24};
}
// The virtual-desktop squares, to the left of the tray button.
Rect desk_rect(int d) {
    Rect t = tray_button_rect();
    return {t.x - 10 - DESKS * 18 + d * 18, bar_mid() - 7, 14, 14};
}
Rect tray_rect() {
    int cpus = (int)smp_cpu_count();
    if (cpus > MAX_CPUS_SHOWN) cpus = MAX_CPUS_SHOWN;
    // Two rows of buttons, swatches, frame rates, the CPU bars, RAM, the
    // history: the same steps draw_tray takes.
    int h = 16 + 46 + 46 + 54 + 54 + 20 + cpus * 16 + 4 + 30 + 24 + (g_history_count ? g_history_count * 22 : 22) + 16;
    Rect b = bar_rect();
    int x = tray_button_rect().x + 12 - TRAY_W / 2;
    if (x + TRAY_W > b.right() - 8) x = b.right() - 8 - TRAY_W;
    return {x, g.H - theme::PANEL_H - h - 8, TRAY_W, h};
}
Rect tray_area() {
    Rect a = tray_rect().inset(-8);
    a.h += 4 + SLIDE_MENU;
    return a;
}
Rect toast_rect(int i) { return {g.W - TOAST_W - 16, 16 + i * (TOAST_H + TOAST_GAP), TOAST_W, TOAST_H}; }
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
    // Names that match come first, then keyword matches, so "c" offers
    // Calculator before the Terminal's "console".
    g_found_count = 0;
    for (int i = 0; i < APP_COUNT; i++)
        if (matches(APPS[i].label, g_search, g_search_len)) g_found[g_found_count++] = i;
    for (int i = 0; i < APP_COUNT; i++)
        if (!matches(APPS[i].label, g_search, g_search_len) && matches(APPS[i].keywords, g_search, g_search_len))
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
// Not on screen: minimised, or on another virtual desktop.
inline bool hidden(const Window& w) { return w.minimised || w.desk != g.cur_desk; }

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
        if (w.used && !hidden(w)) return g.order[i];
    }
    return -1;
}

void terminal_fit(Window& w) {
    Rect cr = content_rect(w);
    // The cell grid is written by whoever prints (any thread, any CPU) under
    // the console lock; resizing and painting it take the same lock.
    console_lock();
    g.term.select_clear();
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
    w.desk = (u8)g.cur_desk;
    if (kind == Kind::About) {
        w.min_w = 430;
        w.min_h = 340;
    }
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

void switcher_finish(bool pick);
void switch_desk(int d);

void destroy_window(int idx) {
    Window& w = g.windows[idx];
    if (!w.used) return;
    damage(anim_bounds(w));
    damage(panel_rect());
    if (!hidden(w) && g_ghost_count < MAX_GHOSTS) {
        // Leave its picture behind to fade out.
        Ghost& gh = g_ghosts[g_ghost_count++];
        gh.w = w;
        gh.start = refclock_now_us();
    } else {
        free_pixels(w.pixels);
        free_pixels(w.shadow);
    }
    w.used = false;
    for (int i = 0; i < g.order_count; i++) {
        if (g.order[i] == idx) {
            for (int j = i; j < g.order_count - 1; j++) g.order[j] = g.order[j + 1];
            g.order_count--;
            break;
        }
    }
    if (g.term_win == idx) g.term_win = -1;
    if (g.switcher_open) switcher_finish(false);
    g.shown_before &= ~(1u << idx);
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
        if (!w.snapped) w.restore = w.frame;
        w.snapped = 0;
        w.maximised = true;
        set_window_frame(w, snap_target(3));
    }
}

// Puts a window on the left or right half of the screen (Windows-style snap).
void snap_window(Window& w, int side) {
    if (!w.maximised && !w.snapped) w.restore = w.frame;
    w.maximised = false;
    w.snapped = (u8)side;
    set_window_frame(w, snap_target(side));
}

// Back to the size it had before it was maximised or snapped.
void unsnap(Window& w) {
    if (!w.maximised && !w.snapped) return;
    w.maximised = false;
    w.snapped = 0;
    set_window_frame(w, w.restore);
}

void minimise(Window& w) {
    w.minimised = true;
    start_anim(w, ANIM_OUT);
    damage(panel_rect());
    if (g.focus == window_index(&w)) {
        g.focus = -1;
        set_focus(top_visible_window());
    }
}

void restore(Window& w) {
    if (w.minimised) start_anim(w, ANIM_IN);
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
            if (w.desk != g.cur_desk) switch_desk(w.desk);
            restore(w);
            return;
        }
    }
    int n = g.order_count;
    int x = 80 + (n % 5) * 40, y = 60 + (n % 5) * 36;
    int idx = -1;
    switch (kind) {
    case Kind::Terminal: idx = create_window(kind, "Terminal", {x, y, 720, 460}); break;
    case Kind::About: idx = create_window(kind, "About Cerberus", {x + 120, y + 40, 460, 400}); break;
    case Kind::SystemMonitor: idx = create_window(kind, "System Monitor", {x + 60, y + 20, 520, 360}); break;
    case Kind::MemoryMap: idx = create_window(kind, "Memory Map", {x + 30, y + 10, 700, 480}); break;
    case Kind::Files:
        idx = create_window(kind, "Files", {x + 40, y + 20, 640, 440});
        if (idx >= 0) {
            g.windows[idx].min_w = FilesApp::MIN_W;
            g.windows[idx].min_h = FilesApp::MIN_H;
            if (!g_files.path()[1]) g_files.go(fs_data_mounted() ? "/data" : "/");
            g_files.refresh();
        }
        break;
    case Kind::Notes:
        idx = create_window(kind, "Notes", {x + 70, y + 30, 640, 460});
        if (idx >= 0) {
            g.windows[idx].min_w = NotesApp::MIN_W;
            g.windows[idx].min_h = NotesApp::MIN_H;
        }
        break;
    case Kind::Calculator:
        idx = create_window(kind, "Calculator", {x + 200, y + 40, 340, 480});
        if (idx >= 0) {
            g.windows[idx].min_w = CalcApp::MIN_W;
            g.windows[idx].min_h = CalcApp::MIN_H;
        }
        break;
    case Kind::Settings: {
        int ww = g.W - 60 < 700 ? g.W - 60 : 700, wh = g.H - theme::PANEL_H - 60 < 560 ? g.H - theme::PANEL_H - 60 : 560;
        idx = create_window(kind, "Settings", {(g.W - ww) / 2, (g.H - theme::PANEL_H - wh) / 2, ww, wh});
        if (idx >= 0) {
            g.windows[idx].min_w = 620;
            g.windows[idx].min_h = 440;
        }
        break;
    }
    }
    if (idx >= 0) start_anim(g.windows[idx], ANIM_IN);
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
void cpu_sample();
void notify(const char* title, const char* text);

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
    draw_text_ellipsis(s, g.font, 26, 96, "A hybrid-kernel operating system, built from scratch.", s.width - 42,
                       theme::TEXT_MUTED);
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
    draw_text_ellipsis(s, g.font, 16, s.height - 30, "Super+1..4 desktops, Super+arrows snap, Super+D, Alt+Tab, Print Screen",
                       s.width - 32, theme::TEXT_MUTED);
}

// System Monitor (Gauge's first form): the last minute of CPU and memory
// as graphs, and the process table with an End task button.
constexpr int HIST = 60;
u8 g_cpu_hist[HIST];                // total CPU busy %, one per second
u8 g_ram_hist[HIST];                // RAM used %
int g_hist_count = 0;
ProcessInfo g_procs[32];
u32 g_proc_count = 0;
u64 g_proc_prev_ticks[32];
u32 g_proc_prev_pid[32];
u8 g_proc_cpu[32];                  // percent over the last second
int g_proc_selected = -1;
int g_proc_scroll = 0;
Rect g_proc_rows[32];
Rect g_proc_end_rect{0, 0, 0, 0};
int g_proc_rows_shown = 0;

// Once a second (from the pump): a new sample of everything the window shows.
void sysmon_sample() {
    cpu_sample();
    int cpus = (int)smp_cpu_count();
    if (cpus > MAX_CPUS_SHOWN) cpus = MAX_CPUS_SHOWN;
    u32 sum = 0;
    for (int i = 0; i < cpus; i++) sum += g_cpu_busy[i];
    PmmStats pm = pmm_stats();
    u8 ram = pm.usable_frames ? (u8)(pm.used_frames * 100 / pm.usable_frames) : 0;
    if (g_hist_count == HIST) {
        memmove(g_cpu_hist, g_cpu_hist + 1, HIST - 1);
        memmove(g_ram_hist, g_ram_hist + 1, HIST - 1);
        g_hist_count = HIST - 1;
    }
    g_cpu_hist[g_hist_count] = cpus ? (u8)(sum / cpus) : 0;
    g_ram_hist[g_hist_count] = ram;
    g_hist_count++;
    // Processes, and each one's share of the last second (100 ticks).
    ProcessInfo fresh[32];
    u32 n = sched_process_snapshot(fresh, 32);
    for (u32 i = 0; i < n; i++) {
        u64 prev = 0;
        for (u32 j = 0; j < g_proc_count; j++)
            if (g_proc_prev_pid[j] == fresh[i].pid) prev = g_proc_prev_ticks[j];
        u64 d = fresh[i].run_ticks > prev ? fresh[i].run_ticks - prev : 0;
        g_proc_cpu[i] = (u8)(d > 100 ? 100 : d);
    }
    for (u32 i = 0; i < n; i++) {
        g_proc_prev_pid[i] = fresh[i].pid;
        g_proc_prev_ticks[i] = fresh[i].run_ticks;
    }
    memcpy(g_procs, fresh, sizeof fresh);
    g_proc_count = n;
}

void draw_graph(Surface& s, const Rect& r, const u8* hist, int count, const char* label, u8 now_value, Color c) {
    fill_rect_rounded(s, r, 8, rgba(255, 255, 255, 7));
    stroke_rect_rounded(s, r, 8, rgba(255, 255, 255, 14));
    for (int q = 1; q < 4; q++) draw_hline(s, r.x + 4, r.right() - 5, r.y + r.h * q / 4, rgba(255, 255, 255, 10));
    int inner_w = r.w - 8, inner_h = r.h - 8;
    int step = inner_w / (HIST - 1);
    if (step < 1) step = 1;
    int x0 = r.right() - 4 - (count - 1) * step;
    for (int i = 1; i < count; i++) {
        int xa = x0 + (i - 1) * step, xb = x0 + i * step;
        int ya = r.bottom() - 4 - inner_h * hist[i - 1] / 100, yb = r.bottom() - 4 - inner_h * hist[i] / 100;
        // A filled column under the line, then the line itself.
        fill_rect(s, {xa, yb < ya ? yb : ya, xb - xa, r.bottom() - 4 - (yb < ya ? yb : ya)}, with_alpha(c, 40));
        draw_line_aa(s, xa, ya, xb, yb, 24, c);
    }
    char line[32];
    ksnprintf(line, sizeof line, "%s  %u%%", label, now_value);
    draw_text(s, g.bold, r.x + 10, r.y + 8, line, theme::TEXT);
}

void paint_sysmon(Surface& s) {
    fill_rect(s, s.bounds(), theme::CONTENT_BG);
    if (!g_hist_count) sysmon_sample();
    int cpus = (int)smp_cpu_count();
    if (cpus > MAX_CPUS_SHOWN) cpus = MAX_CPUS_SHOWN;
    PmmStats pm = pmm_stats();
    u64 used_mb = pm.used_frames * PAGE_SIZE / MIB, total_mb = pm.usable_frames * PAGE_SIZE / MIB;
    // Graphs side by side, a minute long.
    int gw = (s.width - 16 * 3) / 2, gh = 110;
    draw_graph(s, {16, 12, gw, gh}, g_cpu_hist, g_hist_count, "CPU", g_cpu_hist[g_hist_count - 1], accent());
    draw_graph(s, {32 + gw, 12, gw, gh}, g_ram_hist, g_hist_count, "Memory", g_ram_hist[g_hist_count - 1], rgb(74, 222, 128));
    char line[96];
    int y = 12 + gh + 8;
    // One line per CPU, then the memory figure.
    int cx = 16;
    for (int i = 0; i < cpus; i++) {
        ksnprintf(line, sizeof line, "cpu%d %u%%", i, g_cpu_busy[i]);
        draw_text(s, g.font, cx, y, line, theme::TEXT_MUTED);
        cx += measure_text(g.font, line) + 14;
    }
    ksnprintf(line, sizeof line, "%lu / %lu MiB", (unsigned long)used_mb, (unsigned long)total_mb);
    int tw = measure_text(g.font, line);
    draw_text(s, g.font, s.width - 16 - tw, y, line, theme::TEXT_MUTED);
    y += 26;
    // The process table.
    draw_text(s, g.bold, 16, y, "Processes", theme::TEXT);
    {
        bool can = g_proc_selected >= 0 && g_proc_selected < (int)g_proc_count && g_procs[g_proc_selected].pid > 1;
        Rect b{s.width - 16 - 90, y - 4, 90, 26};
        g_proc_end_rect = can ? b : Rect{0, 0, 0, 0};
        fill_rect_rounded(s, b, 8, can ? rgb(196, 43, 28) : rgba(255, 255, 255, 10));
        draw_text(s, g.font, b.x + (b.w - measure_text(g.font, "End task")) / 2, b.y + (b.h - g.font.height) / 2, "End task",
                  can ? rgb(255, 255, 255) : theme::TEXT_MUTED);
    }
    y += 28;
    draw_text(s, g.mono, 16, y, "  pid  name                  thr   cpu    ticks", theme::TEXT_MUTED);
    y += 20;
    int row_h = 20;
    int rows = (s.height - y - 8) / row_h;
    if (rows < 1) rows = 1;
    if (g_proc_scroll > (int)g_proc_count - rows) g_proc_scroll = (int)g_proc_count - rows;
    if (g_proc_scroll < 0) g_proc_scroll = 0;
    g_proc_rows_shown = 0;
    for (int i = g_proc_scroll; i < (int)g_proc_count && g_proc_rows_shown < rows; i++) {
        const ProcessInfo& p = g_procs[i];
        Rect r{10, y - 2, s.width - 20, row_h};
        if (i == g_proc_selected) fill_rect_rounded(s, r, 6, with_alpha(accent(), 70));
        ksnprintf(line, sizeof line, "%5u  %-20s %3u  %3u%%  %7lu%s", p.pid, p.name, p.threads, g_proc_cpu[i],
                  (unsigned long)p.run_ticks, p.zombie ? "  (ended)" : "");
        draw_text(s, g.mono, 16, y, line, p.zombie ? theme::TEXT_MUTED : theme::TEXT);
        g_proc_rows[g_proc_rows_shown++] = r;
        y += row_h;
    }
}

// A click in the System Monitor's content: picks a process or ends it.
void sysmon_click(int x, int y) {
    if (g_proc_end_rect.contains(x, y) && g_proc_selected >= 0 && g_proc_selected < (int)g_proc_count) {
        const Credentials root = {0, 0};
        Result<void> r = signal_send(g_procs[g_proc_selected].pid, sig::KILL, root);
        if (!r.ok()) notify("System Monitor", "That process could not be ended.");
        g_proc_selected = -1;
        return;
    }
    for (int i = 0; i < g_proc_rows_shown; i++)
        if (g_proc_rows[i].contains(x, y)) {
            g_proc_selected = g_proc_scroll + i;
            return;
        }
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
    if (!g_measuring && g_ctl_count < MAX_CTLS) g_ctls[g_ctl_count++] = Ctl{r, act, (i16)val};
}

// A pill-shaped choice; lit when it is the current one. Wraps to the next
// row at the right edge. Advances x.
void chip(Surface& s, int& x, int& y, int left, const char* label, bool on, u8 act, int val) {
    int w = measure_text(g.font, label) + 28;
    if (x + w > g_wrap_right && x > left) {
        x = left;
        y += 38;
    }
    Rect r{x, y, w, 30};
    fill_rect_rounded(s, r, 15, on ? accent() : rgba(255, 255, 255, 20));
    draw_text(s, g.font, x + 14, y + (30 - g.font.height) / 2, label, on ? rgb(15, 23, 42) : theme::TEXT);
    ctl_add(r, act, val);
    x += w + 8;
}

constexpr int SIDE = 172;           // the Settings sidebar

// A titled card. `body(x, y)` paints the controls from (x, y) and returns
// the y below them; it runs twice, first with everything clipped away to
// learn the card's height, then for real. Returns the y for the next card.
template <typename F> int card(Surface& s, int y, const char* title, F body) {
    const int CX = SIDE + 20, CW = s.width - CX - 20, PAD = 16;
    Rect clip = s.clip;
    s.clip = {0, 0, 0, 0};
    g_measuring = true;
    g_wrap_right = CX + CW - PAD;
    int bottom = body(CX + PAD, y + 40);
    g_measuring = false;
    s.clip = clip;
    Rect r{CX, y, CW, bottom - y + 12};
    fill_rect_rounded(s, r, 12, rgba(255, 255, 255, 7));
    stroke_rect_rounded(s, r, 12, rgba(255, 255, 255, 14));
    draw_text(s, g.bold, CX + PAD, y + 12, title, theme::TEXT);
    body(CX + PAD, y + 40);
    return r.bottom() + 10;
}

// The cards are painted after the sidebar, clipped to the content area to
// its right, so a scrolled card never shows over it.
struct CardClip {
    Surface& s;
    Rect saved;
    CardClip(Surface& surface) : s(surface), saved(surface.clip) { s.clip = s.clip.intersect({SIDE + 1, 0, s.width - SIDE - 1, s.height}); }
    ~CardClip() { s.clip = saved; }
};

// The little pictures beside the sidebar entries.
void tab_icon(Surface& s, int x, int y, int tab, bool on) {
    Color c = on ? accent() : theme::TEXT_MUTED;
    switch (tab) {
    case 0:     // a paint drop
        fill_circle_aa(s, x + 7, y + 8, 6, c);
        fill_circle_aa(s, x + 7, y + 8, 2, theme::CONTENT_BG);
        break;
    case 1:     // a picture
        fill_rect_rounded(s, {x, y + 1, 15, 12}, 3, c);
        fill_circle_aa(s, x + 5, y + 5, 2, theme::CONTENT_BG);
        break;
    case 2:     // a window outline
        stroke_rect_rounded(s, {x, y + 1, 15, 13}, 4, c);
        stroke_rect_rounded(s, {x + 1, y + 2, 13, 11}, 3, c);
        break;
    case 3:     // a screen on a stand
        fill_rect_rounded(s, {x, y, 15, 10}, 2, c);
        fill_rect(s, {x + 5, y + 11, 5, 2}, c);
        fill_rect(s, {x + 3, y + 13, 9, 1}, c);
        break;
    case 4:     // a keyboard: a row of keys
        stroke_rect_rounded(s, {x, y + 2, 15, 11}, 3, c);
        for (int i = 0; i < 4; i++) fill_rect(s, {x + 3 + i * 3, y + 5, 2, 2}, c);
        fill_rect(s, {x + 4, y + 9, 7, 2}, c);
        break;
    case 5:     // a disk: two platters
        fill_rect_rounded(s, {x, y + 1, 15, 6}, 3, c);
        fill_rect_rounded(s, {x, y + 8, 15, 6}, 3, c);
        break;
    default:    // a padlock
        stroke_rect_rounded(s, {x + 3, y, 9, 8}, 4, c);
        fill_rect_rounded(s, {x, y + 6, 15, 9}, 3, c);
        break;
    }
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

u8 rounded_coverage(const Rect& r, int R, int x, int y);
bool password_set();
void cpu_sample();
void draw_lock_screen(Surface& back);

// The strip every hue can be picked from, with a knob on the chosen one.
void hue_strip(Surface& s, const Rect& r) {
    Rect c = r.intersect(s.clip);
    for (int y = c.y; y < c.bottom(); y++) {
        u32* row = s.row(y);
        for (int x = c.x; x < c.right(); x++)
            row[x] = mix(row[x], hue_accent((x - r.x) * 1535 / (r.w - 1)), rounded_coverage(r, r.h / 2, x, y));
    }
    if (g_prefs.accent == ACCENT_COUNT) {
        int kx = r.x + g_prefs.custom_hue * (r.w - 1) / 1535, ky = r.y + r.h / 2;
        fill_circle_aa(s, kx, ky, 10, rgb(255, 255, 255));
        fill_circle_aa(s, kx, ky, 7, accent());
    }
    g_hue_rect = r;
    ctl_add({r.x, r.y - 9, r.w, r.h + 18}, ACT_HUE, 0);
}

void paint_settings(Surface& s) {
    g_ctl_count = 0;
    g_hue_rect = {0, 0, 0, 0};
    fill_rect(s, s.bounds(), theme::CONTENT_BG);
    // Sidebar.
    fill_rect(s, {0, 0, SIDE, s.height}, rgb(17, 19, 27));
    draw_vline(s, SIDE, 0, s.height - 1, rgba(255, 255, 255, 18));
    static const char* const TABS[] = {"Appearance", "Wallpaper", "Window borders", "Display", "Keyboard", "Storage", "Security"};
    for (int i = 0; i < 7; i++) {
        Rect r{8, 12 + i * 40, SIDE - 16, 34};
        bool on = i == g_settings_tab;
        if (on) {
            fill_rect_rounded(s, r, 8, rgba(255, 255, 255, 22));
            fill_rect_rounded(s, {r.x, r.y + 8, 3, r.h - 16}, 1, accent());
        }
        tab_icon(s, r.x + 14, r.y + 10, i, on);
        draw_text(s, g.font, r.x + 38, r.y + (r.h - g.font.height) / 2, TABS[i], on ? theme::TEXT : theme::TEXT_MUTED);
        ctl_add(r, ACT_TAB, i);
    }

    CardClip card_clip(s);
    int y = 18 - g_settings_scroll;
    if (g_settings_tab == 0) {
        y = card(s, y, "Accent colour", [&](int x, int y) {
            for (int i = 0; i < ACCENT_COUNT; i++) swatch(s, x + 14 + i * 40, y + 16, ACCENTS[i].c, g_prefs.accent == i, ACT_ACCENT, i);
            // Or any colour at all: click or drag along the strip.
            hue_strip(s, {x + 2, y + 44, 340, 12});
            return y + 60;
        });
        y = card(s, y, "Window corners", [&](int x, int y) {
            int left = x;
            chip(s, x, y, left, "Square", g_prefs.radius == 0, ACT_RADIUS, 0);
            chip(s, x, y, left, "Soft", g_prefs.radius == 6, ACT_RADIUS, 6);
            chip(s, x, y, left, "Round", g_prefs.radius == 12, ACT_RADIUS, 12);
            return y + 30;
        });
        y = card(s, y, "Taskbar", [&](int x, int y) {
            int left = x;
            chip(s, x, y, left, "Floating", g_prefs.panel_floating, ACT_FLOAT, 1);
            chip(s, x, y, left, "Docked", !g_prefs.panel_floating, ACT_FLOAT, 0);
            x += 16;
            chip(s, x, y, left, "Glass", g_prefs.panel_glass, ACT_GLASS, 1);
            chip(s, x, y, left, "Solid", !g_prefs.panel_glass, ACT_GLASS, 0);
            return y + 30;
        });
        y = card(s, y, "Clock", [&](int x, int y) {
            int left = x;
            chip(s, x, y, left, "24-hour", !g_prefs.clock_12h, ACT_CLOCK12, 0);
            chip(s, x, y, left, "12-hour", g_prefs.clock_12h, ACT_CLOCK12, 1);
            x += 16;
            chip(s, x, y, left, "Seconds", g_prefs.clock_seconds, ACT_SECONDS, !g_prefs.clock_seconds);
            // Time zone: a stepper in half hours.
            y += 42;
            x = left;
            draw_text(s, g.font, x, y + (30 - g.font.height) / 2, "Time zone", theme::TEXT_MUTED);
            x += measure_text(g.font, "Time zone") + 16;
            chip(s, x, y, left, "-", false, ACT_TZ, -30);
            int tz = g_prefs.tz_minutes, a = tz < 0 ? -tz : tz;
            char line[24];
            ksnprintf(line, sizeof line, "UTC%c%02d:%02d", tz < 0 ? '-' : '+', a / 60, a % 60);
            int w = measure_text(g.font, line) + 20;
            fill_rect_rounded(s, {x, y, w, 30}, 8, rgba(255, 255, 255, 10));
            draw_text(s, g.font, x + 10, y + (30 - g.font.height) / 2, line, theme::TEXT);
            x += w + 8;
            chip(s, x, y, left, "+", false, ACT_TZ, 30);
            return y + 30;
        });
    } else if (g_settings_tab == 1) {
        y = card(s, y, "Wallpaper", [&](int x, int y) {
            for (int i = 0; i < WALLPAPER_COUNT; i++) {
                Rect r{x + i % 3 * (THUMB_W + 12), y + i / 3 * (THUMB_H + 40), THUMB_W, THUMB_H};
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
            return y + 2 * (THUMB_H + 40) - 10;
        });
    } else if (g_settings_tab == 2) {
        y = card(s, y, "Effect", [&](int x, int y) {
            int left = x;
            for (int i = 0; i < (int)BorderFx::COUNT; i++) chip(s, x, y, left, FX_NAMES[i], (int)g_prefs.fx == i, ACT_FX, i);
            return y + 30;
        });
        y = card(s, y, "Colour", [&](int x, int y) {
            int left = x;
            chip(s, x, y, left, "Accent", g_prefs.border_color < 0, ACT_BCOLOR, -1);
            for (int i = 0; i < ACCENT_COUNT; i++)
                swatch(s, x + 18 + i * 38, y + 15, ACCENTS[i].c, g_prefs.border_color == i, ACT_BCOLOR, i);
            return y + 30;
        });
        y = card(s, y, "Thickness", [&](int x, int y) {
            int left = x;
            static const char* const WIDTHS[] = {"1 px", "2 px", "3 px", "4 px"};
            for (int i = 1; i <= 4; i++) chip(s, x, y, left, WIDTHS[i - 1], g_prefs.border_w == i, ACT_BWIDTH, i);
            return y + 30;
        });
        y = card(s, y, "Speed", [&](int x, int y) {
            int left = x;
            static const char* const SPEEDS[] = {"Slow", "Normal", "Fast"};
            for (int i = 0; i < 3; i++) chip(s, x, y, left, SPEEDS[i], g_prefs.speed == i, ACT_SPEED, i);
            return y + 30;
        });
        y = card(s, y, "Show on", [&](int x, int y) {
            int left = x;
            chip(s, x, y, left, "Focused window", !g_prefs.border_all, ACT_BALL, 0);
            chip(s, x, y, left, "All windows", g_prefs.border_all, ACT_BALL, 1);
            x += 16;
            chip(s, x, y, left, "Glow", g_prefs.glow, ACT_GLOW, !g_prefs.glow);
            return y + 30;
        });
    } else if (g_settings_tab == 4) {
        y = card(s, y, "Layout", [&](int x, int y) {
            int left = x;
            for (int i = 0; i < KBD_LAYOUTS; i++) chip(s, x, y, left, ps2kbd_layout_name(i), g_prefs.layout == i, ACT_LAYOUT, i);
            return y + 30;
        });
        y = card(s, y, "Repeat delay", [&](int x, int y) {
            int left = x;
            static const char* const DELAYS[] = {"250 ms", "500 ms", "750 ms", "1 s"};
            for (int i = 0; i < 4; i++) chip(s, x, y, left, DELAYS[i], g_prefs.repeat_delay == i, ACT_RDELAY, i);
            return y + 30;
        });
        y = card(s, y, "Repeat rate", [&](int x, int y) {
            int left = x;
            static const char* const RATES[] = {"10 per second", "20 per second", "30 per second"};
            for (int i = 0; i < 3; i++) chip(s, x, y, left, RATES[i], g_prefs.repeat_rate == i, ACT_RRATE, i);
            return y + 30;
        });
        y = card(s, y, "Lock keys", [&](int x, int y) {
            u8 m = ps2kbd_mods();
            int left = x;
            chip(s, x, y, left, "Caps Lock", m & mod::CAPS, ACT_TAB, g_settings_tab);
            chip(s, x, y, left, "Num Lock", m & mod::NUM, ACT_TAB, g_settings_tab);
            return y + 30;
        });
    } else if (g_settings_tab == 6) {
        y = card(s, y, password_set() ? "Lock screen password is set" : "Lock screen password", [&](int x, int y) {
            Rect f{x, y, 260, 30};
            fill_rect_rounded(s, f, 8, rgba(255, 255, 255, 14));
            stroke_rect_rounded(s, f, 8, g_pw_editing ? with_alpha(accent(), 150) : rgba(255, 255, 255, 22));
            if (g_pw_len) {
                for (int i = 0; i < g_pw_len && i < 22; i++) fill_circle_aa(s, f.x + 14 + i * 11, f.y + 15, 3, theme::TEXT);
            } else {
                draw_text(s, g.font, f.x + 10, f.y + (f.h - g.font.height) / 2,
                          g_pw_editing ? "Type it, then Enter" : (password_set() ? "New password" : "Choose a password"), theme::TEXT_MUTED);
            }
            ctl_add(f, ACT_PW_FIELD, 0);
            int cx = f.right() + 10;
            chip(s, cx, y, x, "Set", false, ACT_PW_SET, 0);
            if (password_set()) {
                chip(s, cx, y, x, "Remove", false, ACT_PW_CLEAR, 0);
                chip(s, cx, y, x, "Lock now  (Super+L)", true, ACT_LOCK, 0);
            }
            return y + 30;
        });
        if (password_set()) {
            y = card(s, y, "Lock after being idle for", [&](int x, int y) {
                int left = x;
                static const int MINS[] = {0, 1, 5, 15, 30};
                static const char* const LABELS[] = {"Never", "1 min", "5 min", "15 min", "30 min"};
                for (int i = 0; i < 5; i++) chip(s, x, y, left, LABELS[i], g_prefs.autolock_min == MINS[i], ACT_AUTOLOCK, MINS[i]);
                return y + 30;
            });
        }
    } else if (g_settings_tab == 5) {
        y = card(s, y, fs_data_mounted() ? "Settings are saved on /data" : "Settings are not saved yet", [&](int x, int y) {
            DiskInfo disks[8];
            u32 count = fs_disks(disks, 8);
            if (!count) {
                draw_text(s, g.font, x, y + 6, "No disk found. Add a virtual hard disk to the machine.", theme::TEXT_MUTED);
                return y + 30;
            }
            char line[96];
            for (u32 i = 0; i < count; i++) {
                const DiskInfo& d = disks[i];
                const char* what = d.state == DiskInfo::Data ? "holds the settings (/data)"
                                   : d.state == DiskInfo::Busy ? "in use"
                                   : d.state == DiskInfo::Cerfs ? "a Cerberus disk (mount it from the terminal)"
                                                                 : "blank";
                ksnprintf(line, sizeof line, "%s  %lu MiB  %s", d.name, (unsigned long)d.mib, what);
                draw_text(s, g.font, x, y + 6, line, d.state == DiskInfo::Data ? theme::TEXT : theme::TEXT_MUTED);
                y += 30;
                if (!fs_data_mounted() && (d.state == DiskInfo::Blank || d.state == DiskInfo::Cerfs)) {
                    int cx = x;
                    if (g_format_confirm == (int)i) {
                        ksnprintf(line, sizeof line, "Erase %s and use it", d.name);
                        chip(s, cx, y, x, line, true, ACT_FORMAT, (int)i);
                    } else {
                        chip(s, cx, y, x, "Use for settings", false, ACT_FORMAT, (int)i);
                    }
                    y += 38;
                }
            }
            return y;
        });
    } else {
        y = card(s, y, "Screen resolution", [&](int x, int y) {
            int left = x;
            char line[32];
            if (!bga_available()) {
                ksnprintf(line, sizeof line, "%d x %d", g.W, g.H);
                chip(s, x, y, left, line, true, ACT_MODE, -1);
                return y + 30;
            }
            for (int i = 0; i < MODE_COUNT; i++) {
                if (!bga_mode_fits((u32)MODES[i].w, (u32)MODES[i].h)) continue;
                ksnprintf(line, sizeof line, "%d x %d", MODES[i].w, MODES[i].h);
                chip(s, x, y, left, line, MODES[i].w == g.W && MODES[i].h == g.H, ACT_MODE, i);
            }
            return y + 30;
        });
        y = card(s, y, "Frame rate", [&](int x, int y) {
            int left = x;
            char line[16];
            for (int i = 0; i < FPS_COUNT; i++) {
                ksnprintf(line, sizeof line, "%d Hz", FPS_CHOICES[i]);
                chip(s, x, y, left, line, g_prefs.fps == FPS_CHOICES[i], ACT_FPS, FPS_CHOICES[i]);
            }
            return y + 30;
        });
    }
    // What does not fit can be scrolled to; a thin bar shows where we are.
    g_settings_height = y + g_settings_scroll;
    int max_scroll = g_settings_height - s.height;
    if (max_scroll > 0) {
        Rect track{s.width - 6, 8, 3, s.height - 16};
        fill_rect_rounded(s, track, 1, rgba(255, 255, 255, 14));
        int th = track.h * s.height / g_settings_height;
        if (th < 20) th = 20;
        int ty = track.y + (track.h - th) * g_settings_scroll / max_scroll;
        fill_rect_rounded(s, {track.x, ty, 3, th}, 1, rgba(255, 255, 255, 70));
    }
}

void notify(const char* title, const char* text);
void open_kind(Kind kind);

AppContext app_ctx() {
    AppContext c;
    c.font = &g.font;
    c.bold = &g.bold;
    c.mono = &g.mono;
    c.accent = accent();
    c.text = theme::TEXT;
    c.muted = theme::TEXT_MUTED;
    c.bg = theme::CONTENT_BG;
    c.clipboard = g_clipboard;
    c.clipboard_len = g_clip_len;
    c.clipboard_set = clipboard_set;
    c.notify = notify;
    return c;
}

// Files asked for a file to be opened: hand it to Notes.
void serve_open_request() {
    const char* path = g_files.take_open_request();
    if (!path) return;
    g_notes.load(path);
    g_files.clear_open_request();
    open_kind(Kind::Notes);
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g.windows[i].used && g.windows[i].kind == Kind::Notes) g.windows[i].needs_paint = true;
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
    case Kind::Files: {
        g_files.paint(view, app_ctx());
        char title[64];
        ksnprintf(title, sizeof title, "Files - %s", g_files.path());
        if (strcmp(w.title, title) != 0) {
            strlcpy(w.title, title, sizeof w.title);
            damage(w.frame);
            damage(panel_rect());
        }
        break;
    }
    case Kind::Notes: {
        g_notes.paint(view, app_ctx());
        char title[64];
        ksnprintf(title, sizeof title, "Notes - %s%s", g_notes.path()[0] ? g_notes.path() : "new", g_notes.modified() ? " *" : "");
        if (strcmp(w.title, title) != 0) {
            strlcpy(w.title, title, sizeof w.title);
            damage(w.frame);
            damage(panel_rect());
        }
        break;
    }
    case Kind::Calculator: g_calc.paint(view, app_ctx()); break;
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

// The shadow is cached; while a window is being resized the old one is
// scaled instead, to stay responsive.
void ensure_shadow(Window& w) {
    int sw = w.frame.w + 2 * theme::SHADOW_PAD, sh = w.frame.h + 2 * theme::SHADOW_PAD;
    if (!w.shadow || ((w.shadow_w != sw || w.shadow_h != sh) && g.drag != Drag::Resize)) build_shadow(w);
}

void draw_window(Surface& back, Window& w, bool focused) {
    Rect sr = shadow_rect(w);
    int sw = w.frame.w + 2 * theme::SHADOW_PAD, sh = w.frame.h + 2 * theme::SHADOW_PAD;
    ensure_shadow(w);
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

// Runs `draw`, then leaves only `shown`/255 of what it drew inside `bounds`:
// the rest stays as it was. This is how windows and the launcher fade.
template <typename F> void draw_faded(Surface& back, const Rect& bounds, u32 shown, F draw) {
    Rect a = bounds.intersect(back.clip);
    if (a.empty()) return;
    u32* saved = g.scratch.pixels;      // at least a screen in size
    for (int y = 0; y < a.h; y++) memcpy(saved + (isize)y * a.w, back.row(a.y + y) + a.x, (usize)a.w * 4);
    draw();
    for (int y = 0; y < a.h; y++) {
        u32* row = back.row(a.y + y) + a.x;
        const u32* was = saved + (isize)y * a.w;
        for (int x = 0; x < a.w; x++) row[x] = mix(was[x], row[x], (u8)shown);
    }
}

// A window `shown`/255 visible and `dy` pixels below its place.
void draw_window_faded(Surface& back, Window& w, bool focused, u32 shown, int dy) {
    ensure_shadow(w);       // now: building one uses the scratch buffer too
    draw_faded(back, visual_bounds(w).translated(0, dy), shown, [&] {
        w.frame.y += dy;
        draw_window(back, w, focused);
        w.frame.y -= dy;
    });
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
    for (int i = 0; i < g.order_count; i++) shown += g.windows[g.order[i]].used && g.windows[g.order[i]].desk == g.cur_desk;
    int room = clock_rect().x - 10 - x;
    int task_w = shown ? (room - 6 * (shown - 1)) / shown : 176;
    if (task_w > 176) task_w = 176;
    if (task_w < 40) task_w = 40;
    // In the order the windows were opened, not the stacking order, so a
    // button does not move when its window is clicked.
    for (int idx = 0; idx < MAX_WINDOWS; idx++) {
        Window& w = g.windows[idx];
        if (!w.used || w.desk != g.cur_desk) continue;
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

    // Right-hand side: memory meter, then the clock over the date. The clock
    // is a button: it opens the calendar.
    DateTime now = local_now();
    char buf[64];
    {
        Rect ck = clock_rect();
        Rect hl{ck.right() - 118, mid - 20, 112, 40};
        bool hover = ck.contains(g.mx, g.my) && g.mx >= hl.x;
        if (hover || g.cal_open) fill_rect_rounded(back, hl, 10, hover && !g.cal_open ? rgba(255, 255, 255, 22) : with_alpha(accent(), 54));
    }
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
    {
        // The tray button: a chevron that opens the quick settings.
        Rect tb = tray_button_rect();
        bool hover = tb.contains(g.mx, g.my);
        if (hover || g.tray_open) fill_rect_rounded(back, tb, 8, hover && !g.tray_open ? rgba(255, 255, 255, 22) : with_alpha(accent(), 54));
        int cx = tb.x + 12, cy = tb.y + 12 + (g.tray_open ? 2 : -1);
        Color gc = theme::TEXT_MUTED;
        if (g.tray_open) {
            draw_line_aa(back, cx - 5, cy - 3, cx, cy + 2, 24, gc);
            draw_line_aa(back, cx + 5, cy - 3, cx, cy + 2, 24, gc);
        } else {
            draw_line_aa(back, cx - 5, cy + 3, cx, cy - 2, 24, gc);
            draw_line_aa(back, cx + 5, cy + 3, cx, cy - 2, 24, gc);
        }
        // The desktops: a square each, the current one in the accent colour,
        // a dot in those that hold windows.
        for (int d = 0; d < DESKS; d++) {
            Rect dr = desk_rect(d);
            bool any = false;
            for (int i = 0; i < MAX_WINDOWS; i++) any |= g.windows[i].used && g.windows[i].desk == d;
            bool here = d == g.cur_desk, dh = dr.inset(-3).contains(g.mx, g.my);
            fill_rect_rounded(back, dr, 4, here ? accent() : rgba(255, 255, 255, dh ? 50 : 24));
            if (any && !here) fill_circle_aa(back, dr.x + 7, dr.y + 7, 2, theme::TEXT);
        }
        // Caps Lock on, or Num Lock off, is worth a glance: a small pill.
        u8 m = ps2kbd_mods();
        int px = desk_rect(0).x - 8;
        auto pill = [&](const char* label, Color c) {
            int w = measure_text(g.font, label) + 12;
            Rect r{px - w, mid - 10, w, 20};
            fill_rect_rounded(back, r, 6, with_alpha(c, 50));
            draw_text(back, g.font, r.x + 6, r.y + (20 - g.font.height) / 2, label, c);
            px = r.x - 6;
        };
        if (m & mod::CAPS) pill("CAPS", accent());
        if (!(m & mod::NUM)) pill("NUM off", theme::TEXT_MUTED);
    }
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

void draw_menu(Surface& back, int dy = 0) {
    Rect mr = menu_rect().translated(0, dy);
    fill_rect_rounded(back, mr.translated(0, 4), 14, rgba(0, 0, 0, 90));
    fill_rect_rounded(back, mr, 14, theme::MENU_BG);
    stroke_rect_rounded(back, mr, 14, theme::BORDER);

    // Search field.
    Rect sr = menu_search_rect().translated(0, dy);
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
        Rect r = menu_power_rect(restart).translated(0, dy);
        bool hover = g.menu_hover == (restart ? ITEM_RESTART : ITEM_POWER);
        fill_rect_rounded(back, r, 8, hover ? (restart ? rgba(255, 255, 255, 40) : rgb(196, 43, 28)) : rgba(255, 255, 255, 18));
        bool armed = refclock_now_us() - g.power_armed_us <= 5000000;
        const char* label = restart ? (armed ? "Restart?" : "Restart") : (armed ? "Sure?" : "Power off");
        if (armed) fill_rect_rounded(back, r, 8, with_alpha(accent(), 160));
        draw_text(back, g.font, r.x + (r.w - measure_text(g.font, label)) / 2, r.y + (r.h - g.font.height) / 2, label,
                  theme::TEXT);
    }
}

void cal_ctl(const Rect& r, u8 act, int val) {
    if (g_cal_ctl_count < (int)(sizeof g_cal_ctls / sizeof g_cal_ctls[0])) g_cal_ctls[g_cal_ctl_count++] = Ctl{r, act, (i16)val};
}

// The calendar above the clock: a month to page through, today marked, and
// the reminders of the day picked, with a line to add one.
void draw_calendar(Surface& back, int dy = 0) {
    g_cal_ctl_count = 0;
    Rect cr = cal_rect().translated(0, dy);
    fill_rect_rounded(back, cr.translated(0, 4), 14, rgba(0, 0, 0, 90));
    fill_rect_rounded(back, cr, 14, theme::MENU_BG);
    stroke_rect_rounded(back, cr, 14, theme::BORDER);
    DateTime now = local_now();

    // Month and year, with the paging buttons.
    char line[64];
    ksnprintf(line, sizeof line, "%s %d", MONTH_NAMES[g.cal_month - 1], g.cal_year);
    draw_text(back, g.bold, cr.x + 20, cr.y + 16, line, theme::TEXT);
    for (int i = 0; i < 2; i++) {
        Rect b{cr.right() - 20 - (2 - i) * 32 + 4, cr.y + 12, 28, 28};
        bool hover = b.contains(g.mx, g.my);
        fill_rect_rounded(back, b, 8, rgba(255, 255, 255, hover ? 40 : 16));
        int cx = b.x + 14, cy = b.y + 14, d = i ? 1 : -1;      // the chevron points the way it pages
        draw_line_aa(back, cx + d * 2, cy, cx - d * 3, cy - 5, 24, theme::TEXT);
        draw_line_aa(back, cx + d * 2, cy, cx - d * 3, cy + 5, 24, theme::TEXT);
        cal_ctl(b, i ? CAL_NEXT : CAL_PREV, 0);
    }

    // Weekday headings and the grid of days.
    const int cell = 40, gx = cr.x + (CAL_W - 7 * cell) / 2;
    int y = cr.y + 50;
    for (int i = 0; i < 7; i++) {
        int tw = measure_text(g.font, DAY_NAMES[i]);
        draw_text(back, g.font, gx + i * cell + (cell - tw) / 2, y, DAY_NAMES[i], theme::TEXT_MUTED);
    }
    y += 24;
    int first = weekday(g.cal_year, g.cal_month, 1), days = days_in_month(g.cal_year, g.cal_month);
    for (int d = 1; d <= days; d++) {
        int slot = first + d - 1;
        Rect c{gx + slot % 7 * cell, y + slot / 7 * 32, cell, 32};
        int cx = c.x + cell / 2, cy = c.y + 16;
        bool today = now.year == g.cal_year && now.month == g.cal_month && now.day == d;
        bool picked = g.sel_year == g.cal_year && g.sel_month == g.cal_month && g.sel_day == d;
        bool hover = c.contains(g.mx, g.my);
        if (today) fill_circle_aa(back, cx, cy, 14, accent());
        else if (picked) fill_circle_aa(back, cx, cy, 14, with_alpha(accent(), 70));
        else if (hover) fill_circle_aa(back, cx, cy, 14, rgba(255, 255, 255, 24));
        ksnprintf(line, sizeof line, "%d", d);
        int tw = measure_text(g.font, line);
        draw_text(back, g.font, cx - tw / 2, cy - g.font.height / 2, line, today ? rgb(15, 23, 42) : theme::TEXT);
        if (has_reminder(g.cal_year, g.cal_month, d)) fill_circle_aa(back, cx, cy + 11, 2, today ? rgb(15, 23, 42) : accent());
        cal_ctl(c, CAL_DAY, d);
    }
    y += 6 * 32 + 6;
    draw_hline(back, cr.x + 16, cr.right() - 17, y, rgba(255, 255, 255, 22));
    y += 12;

    // Reminders for the picked day.
    ksnprintf(line, sizeof line, "%s %d %s", DAY_LONG[weekday(g.sel_year, g.sel_month, g.sel_day)], g.sel_day,
              MONTH_NAMES[g.sel_month - 1]);
    draw_text(back, g.bold, cr.x + 20, y, line, theme::TEXT);
    {
        Rect t{cr.right() - 20 - 64, y - 4, 64, 24};
        bool hover = t.contains(g.mx, g.my);
        fill_rect_rounded(back, t, 8, rgba(255, 255, 255, hover ? 40 : 16));
        draw_text(back, g.font, t.x + (t.w - measure_text(g.font, "Today")) / 2, t.y + (t.h - g.font.height) / 2, "Today",
                  theme::TEXT);
        cal_ctl(t, CAL_TODAY, 0);
    }
    y += 28;
    Rect in{cr.x + 16, y, CAL_W - 32, 30};
    fill_rect_rounded(back, in, 8, rgba(255, 255, 255, 14));
    stroke_rect_rounded(back, in, 8, with_alpha(accent(), 150));
    int tx = in.x + 10, ty = in.y + (in.h - g.font.height) / 2;
    if (g_rem_len) tx = draw_text_ellipsis(back, g.font, tx, ty, g_rem_text, in.w - 20, theme::TEXT);
    else draw_text(back, g.font, tx, ty, "New reminder, e.g. 14:30 Call home", theme::TEXT_MUTED);
    fill_rect(back, {tx + 1, in.y + 7, 2, in.h - 14}, accent());
    y += 38;
    int shown = 0;
    for (int i = 0; i < MAX_REMINDERS && shown < 3; i++) {
        const Reminder& r = g_reminders[i];
        if (!r.used || r.year != g.sel_year || r.month != g.sel_month || r.day != g.sel_day) continue;
        ksnprintf(line, sizeof line, "%02u:%02u", r.hour, r.minute);
        draw_text(back, g.mono, cr.x + 22, y, line, accent());
        char label[64];
        ksnprintf(label, sizeof label, "%s%s", r.text, r.repeat == 1 ? " (daily)" : r.repeat == 2 ? " (weekly)" : "");
        draw_text_ellipsis(back, g.font, cr.x + 80, y, label, CAL_W - 80 - 50, theme::TEXT);
        Rect xr{cr.right() - 40, y - 3, 22, 22};
        bool hover = xr.contains(g.mx, g.my);
        if (hover) fill_rect_rounded(back, xr, 6, rgba(255, 255, 255, 30));
        int cx = xr.x + 11, cy = xr.y + 11;
        draw_line_aa(back, cx - 4, cy - 4, cx + 4, cy + 4, 20, hover ? theme::TEXT : theme::TEXT_MUTED);
        draw_line_aa(back, cx + 4, cy - 4, cx - 4, cy + 4, 20, hover ? theme::TEXT : theme::TEXT_MUTED);
        cal_ctl(xr, CAL_DELETE, i);
        y += 24;
        shown++;
    }
}

void draw_context_menu(Surface& back) {
    Rect mr = ctx_rect();
    fill_rect_rounded(back, mr.translated(0, 3), 10, rgba(0, 0, 0, 90));
    fill_rect_rounded(back, mr, 10, theme::MENU_BG);
    stroke_rect_rounded(back, mr, 10, theme::BORDER);
    for (int i = 0; i < ctx_item_count(); i++) {
        Rect ir{mr.x + 6, mr.y + 6 + i * CTX_ITEM_H, mr.w - 12, CTX_ITEM_H};
        if (ir.contains(g.mx, g.my)) fill_rect_rounded(back, ir, 6, with_alpha(accent(), 70));
        draw_text(back, g.font, ir.x + 12, ir.y + (ir.h - g.font.height) / 2, ctx_item(i), theme::TEXT);
    }
}

// Alt+Tab: every window as a tile, the one that Alt's release will pick lit.
void draw_switcher(Surface& back) {
    Rect sr = switcher_rect();
    fill_rect_rounded(back, sr.translated(0, 4), 16, rgba(0, 0, 0, 110));
    fill_rect_rounded(back, sr, 16, theme::MENU_BG);
    stroke_rect_rounded(back, sr, 16, theme::BORDER);
    for (int i = 0; i < g.switcher_count; i++) {
        const Window& w = g.windows[g.switcher_wins[i]];
        Rect t{sr.x + 12 + i * (SW_TILE + 8), sr.y + 12, SW_TILE, SW_TILE_H};
        if (i == g.switcher_sel) {
            fill_rect_rounded(back, t, 12, with_alpha(accent(), 60));
            stroke_rect_rounded(back, t, 12, with_alpha(accent(), 200));
        }
        draw_app_icon(back, t.x + (t.w - 44) / 2, t.y + 14, 44, w.kind);
        int tw = measure_text(g.font, w.title);
        if (tw > t.w - 16) draw_text_ellipsis(back, g.font, t.x + 8, t.bottom() - 30, w.title, t.w - 16, theme::TEXT);
        else draw_text(back, g.font, t.x + (t.w - tw) / 2, t.bottom() - 30, w.title, w.minimised ? theme::TEXT_MUTED : theme::TEXT);
    }
}

void tray_ctl(const Rect& r, u8 act, int val) {
    if (g_tray_ctl_count < (int)(sizeof g_tray_ctls / sizeof g_tray_ctls[0])) g_tray_ctls[g_tray_ctl_count++] = Ctl{r, act, (i16)val};
}

// A small toggle or button in the tray.
void tray_toggle(Surface& back, int x, int y, int w, const char* label, bool on, u8 act, int val) {
    Rect r{x, y, w, 36};
    fill_rect_rounded(back, r, 10, on ? with_alpha(accent(), 180) : rgba(255, 255, 255, 16));
    int tw = measure_text(g.font, label);
    draw_text(back, g.font, r.x + (r.w - tw) / 2, r.y + (r.h - g.font.height) / 2, label, on ? rgb(15, 23, 42) : theme::TEXT);
    tray_ctl(r, act, val);
}

void draw_tray(Surface& back, int dy = 0) {
    g_tray_ctl_count = 0;
    Rect tr = tray_rect().translated(0, dy);
    fill_rect_rounded(back, tr.translated(0, 4), 14, rgba(0, 0, 0, 90));
    fill_rect_rounded(back, tr, 14, theme::MENU_BG);
    stroke_rect_rounded(back, tr, 14, theme::BORDER);
    int x = tr.x + 16, y = tr.y + 16, w = tr.w - 32;
    // Toggles.
    int half = (w - 8) / 2;
    tray_toggle(back, x, y, half, "Night light", g_prefs.night_light, TRAY_NIGHT, 0);
    tray_toggle(back, x + half + 8, y, half, "Show desktop", false, TRAY_DESKTOP, 0);
    y += 36 + 10;
    tray_toggle(back, x, y, half, "Lock", false, TRAY_LOCK, 0);
    tray_toggle(back, x + half + 8, y, half, "Settings", false, TRAY_SETTINGS, 0);
    y += 36 + 10;
    // Accent swatches.
    for (int i = 0; i < ACCENT_COUNT; i++) {
        int cx = x + 12 + i * 34, cy = y + 16;
        if (g_prefs.accent == i) {
            fill_circle_aa(back, cx, cy, 14, rgb(255, 255, 255));
            fill_circle_aa(back, cx, cy, 12, theme::MENU_BG);
        }
        fill_circle_aa(back, cx, cy, 10, ACCENTS[i].c);
        tray_ctl({cx - 14, cy - 14, 28, 28}, TRAY_ACCENT, i);
    }
    y += 44 + 10;
    // Frame rate.
    {
        int cx = x;
        char line[16];
        for (int i = 0; i < FPS_COUNT; i++) {
            ksnprintf(line, sizeof line, "%d", FPS_CHOICES[i]);
            int cw = measure_text(g.font, line) + 18;
            Rect r{cx, y, cw, 28};
            fill_rect_rounded(back, r, 8, g_prefs.fps == FPS_CHOICES[i] ? accent() : rgba(255, 255, 255, 16));
            draw_text(back, g.font, r.x + 9, r.y + (r.h - g.font.height) / 2, line, g_prefs.fps == FPS_CHOICES[i] ? rgb(15, 23, 42) : theme::TEXT);
            tray_ctl(r, TRAY_FPS, FPS_CHOICES[i]);
            cx += cw + 6;
        }
        draw_text(back, g.font, cx + 4, y + (28 - g.font.height) / 2, "Hz", theme::TEXT_MUTED);
    }
    y += 44 + 10;
    // CPU and memory.
    draw_text(back, g.bold, x, y, "CPU", theme::TEXT);
    y += 20;
    int cpus = (int)smp_cpu_count();
    if (cpus > MAX_CPUS_SHOWN) cpus = MAX_CPUS_SHOWN;
    char line[48];
    for (int i = 0; i < cpus; i++) {
        ksnprintf(line, sizeof line, "%d", i);
        draw_text(back, g.font, x, y, line, theme::TEXT_MUTED);
        Rect bar{x + 24, y + 5, w - 24 - 44, 7};
        fill_rect_rounded(back, bar, 3, rgba(255, 255, 255, 25));
        int fw = bar.w * g_cpu_busy[i] / 100;
        if (fw > 0) fill_rect_rounded(back, {bar.x, bar.y, fw < 7 ? 7 : fw, bar.h}, 3, g_cpu_busy[i] > 85 ? rgb(248, 113, 113) : accent());
        ksnprintf(line, sizeof line, "%u%%", g_cpu_busy[i]);
        int tw = measure_text(g.font, line);
        draw_text(back, g.font, x + w - tw, y, line, theme::TEXT);
        y += 16;
    }
    y += 4;
    PmmStats pm = pmm_stats();
    u64 used_mb = pm.used_frames * PAGE_SIZE / MIB, total_mb = pm.usable_frames * PAGE_SIZE / MIB;
    ksnprintf(line, sizeof line, "RAM  %lu / %lu MiB", (unsigned long)used_mb, (unsigned long)total_mb);
    draw_text(back, g.font, x, y, line, theme::TEXT_MUTED);
    y += 20 + 10;
    // Notification history.
    draw_text(back, g.bold, x, y, "Notifications", theme::TEXT);
    if (g_history_count) {
        Rect cl{x + w - 56, y - 2, 56, 22};
        fill_rect_rounded(back, cl, 6, rgba(255, 255, 255, 16));
        draw_text(back, g.font, cl.x + 10, cl.y + (cl.h - g.font.height) / 2, "Clear", theme::TEXT);
        tray_ctl(cl, TRAY_CLEAR, 0);
    }
    y += 24;
    if (!g_history_count) {
        draw_text(back, g.font, x, y, "None yet", theme::TEXT_MUTED);
    } else {
        for (int i = g_history_count - 1; i >= 0; i--) {
            const Past& h = g_history[i];
            ksnprintf(line, sizeof line, "%02u:%02u", h.hour, h.minute);
            draw_text(back, g.mono, x, y + 1, line, theme::TEXT_MUTED);
            char both[100];
            ksnprintf(both, sizeof both, "%s: %s", h.title, h.text);
            draw_text_ellipsis(back, g.font, x + 50, y, both, w - 50, theme::TEXT);
            y += 22;
        }
    }
}

void draw_toasts(Surface& back) {
    for (int i = 0; i < MAX_TOASTS; i++) {
        const Toast& t = g_toasts[i];
        if (!t.used) continue;
        Rect r = toast_rect(i);
        if (!r.inset(-8).overlaps(back.clip)) continue;
        u32 shown = anim_eased(t.start);
        draw_faded(back, r.inset(-8).translated(0, -SLIDE_MENU), shown, [&] {
            Rect tr = r.translated(0, -SLIDE_MENU * (int)(255 - shown) / 255);
            fill_rect_rounded(back, tr.translated(0, 3), 12, rgba(0, 0, 0, 80));
            fill_rect_rounded(back, tr, 12, theme::MENU_BG);
            stroke_rect_rounded(back, tr, 12, theme::BORDER);
            fill_rect_rounded(back, {tr.x + 10, tr.y + 14, 4, tr.h - 28}, 2, accent());
            draw_text_ellipsis(back, g.bold, tr.x + 24, tr.y + 12, t.title, tr.w - 60, theme::TEXT);
            draw_text_ellipsis(back, g.font, tr.x + 24, tr.y + 36, t.text, tr.w - 40, theme::TEXT_MUTED);
            int cx = tr.right() - 20, cy = tr.y + 20;
            draw_line_aa(back, cx - 4, cy - 4, cx + 4, cy + 4, 20, theme::TEXT_MUTED);
            draw_line_aa(back, cx + 4, cy - 4, cx - 4, cy + 4, 20, theme::TEXT_MUTED);
            if (t.snooze_text[0]) {
                Rect sb{tr.right() - 86, tr.bottom() - 30, 70, 22};
                fill_rect_rounded(back, sb, 6, with_alpha(accent(), 140));
                draw_text(back, g.font, sb.x + (sb.w - measure_text(g.font, "Snooze")) / 2, sb.y + (sb.h - g.font.height) / 2,
                          "Snooze", rgb(15, 23, 42));
            }
        });
    }
}

// Framebuffer pixels are written with plain 32-bit stores, never `rep movsb`:
// VirtualBox's Hyper-V backend emulates string instructions that touch video
// memory one byte per exit and effectively never finishes a frame. Only
// pixels that differ from what the screen already shows are written.
// Night light: less blue and a little less green, a warm evening screen.
inline u32 warm(u32 v) {
    if (!g_prefs.night_light) return v;
    u32 r = (v >> 16) & 255, gr = (v >> 8) & 255, b = v & 255;
    return (v & 0xFF000000u) | (r << 16) | ((gr * 230 / 255) << 8) | (b * 170 / 255);
}

inline void present_px(int x, int y, u32 v) {
    v = warm(v);
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
        u32 v = warm(src[i]);
        if (f[i] == v) continue;
        f[i] = v;
        dst[i] = v;
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
        if (!w.used || w.desk != g.cur_desk || (w.minimised && w.anim != ANIM_OUT)) continue;
        if (!anim_bounds(w).overlaps(c)) continue;
        bool focused = g.focus == g.order[i];
        u32 e = w.anim ? anim_eased(w.anim_start) : 255;
        if (w.anim == ANIM_IN) draw_window_faded(g.back, w, focused, e, SLIDE_OPEN * (int)(255 - e) / 255);
        else if (w.anim == ANIM_OUT) draw_window_faded(g.back, w, focused, 255 - e, SLIDE_MINIMISE * (int)e / 255);
        else draw_window(g.back, w, focused);
    }
    if (g.snap_preview) {
        Rect t = snap_target(g.snap_preview);
        fill_rect_rounded(g.back, t, radius(), with_alpha(accent(), 40));
        stroke_rect_rounded(g.back, t, radius(), with_alpha(accent(), 160));
    }
    for (int i = 0; i < g_ghost_count; i++) {
        Window& w = g_ghosts[i].w;
        if (!anim_bounds(w).overlaps(c)) continue;
        u32 e = anim_eased(g_ghosts[i].start);
        draw_window_faded(g.back, w, false, 255 - e, SLIDE_OPEN * (int)e / 255);
    }
    if ((g.menu_open || g.menu_anim == ANIM_OUT) && menu_area().overlaps(c)) {
        u32 e = g.menu_anim ? anim_eased(g.menu_anim_start) : 255;
        u32 shown = g.menu_anim == ANIM_OUT ? 255 - e : e;
        if (shown == 255) {
            draw_menu(g.back);
        } else {
            // It rises out of the taskbar and sinks back into it.
            int dy = SLIDE_MENU * (int)(255 - shown) / 255;
            draw_faded(g.back, menu_area(), shown, [&] { draw_menu(g.back, dy); });
        }
    }
    if ((g.cal_open || g.cal_anim == ANIM_OUT) && cal_area().overlaps(c)) {
        u32 e = g.cal_anim ? anim_eased(g.cal_anim_start) : 255;
        u32 shown = g.cal_anim == ANIM_OUT ? 255 - e : e;
        if (shown == 255) {
            draw_calendar(g.back);
        } else {
            int dy = SLIDE_MENU * (int)(255 - shown) / 255;
            draw_faded(g.back, cal_area(), shown, [&] { draw_calendar(g.back, dy); });
        }
    }
    if (g.ctx_open && ctx_area().overlaps(c)) draw_context_menu(g.back);
    if (g.switcher_open && switcher_rect().inset(-8).overlaps(c)) draw_switcher(g.back);
    if ((g.tray_open || g.tray_anim == ANIM_OUT) && tray_area().overlaps(c)) {
        u32 e = g.tray_anim ? anim_eased(g.tray_anim_start) : 255;
        u32 shown = g.tray_anim == ANIM_OUT ? 255 - e : e;
        if (shown == 255) {
            draw_tray(g.back);
        } else {
            int dy = SLIDE_MENU * (int)(255 - shown) / 255;
            draw_faded(g.back, tray_area(), shown, [&] { draw_tray(g.back, dy); });
        }
    }
    draw_toasts(g.back);
    if (panel_rect().overlaps(c)) draw_panel(g.back);
    if (g_locked) draw_lock_screen(g.back);
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
    enum What { Nothing, Title, Button, Content, Edge, Panel, Launcher, Task, Menu, Clock, Calendar, Toast, Context, Tray, TrayPopup, Desk } what = Nothing;
    int win = -1;
    int button = -1;
    u8 edges = 0;
    int item = -1;
};

Hit hit_test(int x, int y) {
    Hit h;
    for (int i = 0; i < MAX_TOASTS; i++) {
        if (g_toasts[i].used && toast_rect(i).contains(x, y)) {
            h.what = Hit::Toast;
            h.item = i;
            return h;
        }
    }
    if (g.cal_open && cal_rect().contains(x, y)) {
        h.what = Hit::Calendar;
        return h;
    }
    if (g.tray_open && tray_rect().contains(x, y)) {
        h.what = Hit::TrayPopup;
        return h;
    }
    if (g.ctx_open && ctx_rect().contains(x, y)) {
        Rect mr = ctx_rect();
        h.what = Hit::Context;
        int i = (y - mr.y - 6) / CTX_ITEM_H;
        h.item = y >= mr.y + 6 && i < ctx_item_count() ? i : -1;
        return h;
    }
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
        Rect ck = clock_rect();
        if (ck.contains(x, y) && x >= ck.right() - 118) h.what = Hit::Clock;
        if (tray_button_rect().inset(-3).contains(x, y)) h.what = Hit::Tray;
        for (int d = 0; d < DESKS; d++)
            if (desk_rect(d).inset(-3).contains(x, y)) {
                h.what = Hit::Desk;
                h.item = d;
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
        if (!w.used || hidden(w)) continue;
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
    g.menu_anim = ANIM_OUT;
    g.menu_anim_start = refclock_now_us();
    damage(search_rect());
    damage(menu_area());
    damage(launcher_rect());
}

void open_menu() {
    g.menu_open = true;
    g.menu_anim = ANIM_IN;
    g.menu_anim_start = refclock_now_us();
    g.menu_hover = -1;
    g_search_len = 0;
    g_search[0] = 0;
    search_update();
    damage(search_rect());
    damage(menu_area());
    damage(launcher_rect());
}

void reminders_write();

void close_tray() {
    if (!g.tray_open) return;
    g.tray_open = false;
    g.tray_anim = ANIM_OUT;
    g.tray_anim_start = refclock_now_us();
    damage(tray_area());
    damage(panel_rect());
}

void cpu_sample() {
    int cpus = (int)smp_cpu_count();
    if (cpus > MAX_CPUS_SHOWN) cpus = MAX_CPUS_SHOWN;
    for (int i = 0; i < cpus; i++) {
        u64 ticks = 0, idle = 0;
        sched_cpu_ticks((u32)i, &ticks, &idle);
        u64 dt = ticks - g_cpu_prev_ticks[i], di = idle - g_cpu_prev_idle[i];
        g_cpu_busy[i] = dt ? (u8)((dt - (di < dt ? di : dt)) * 100 / dt) : 0;
        g_cpu_prev_ticks[i] = ticks;
        g_cpu_prev_idle[i] = idle;
    }
}

void close_calendar() {
    if (!g.cal_open) return;
    g.cal_open = false;
    g.cal_anim = ANIM_OUT;
    g.cal_anim_start = refclock_now_us();
    damage(cal_area());
    damage(clock_rect());
}

void open_calendar() {
    close_menu();
    DateTime now = local_now();
    g.cal_open = true;
    g.cal_anim = ANIM_IN;
    g.cal_anim_start = refclock_now_us();
    g.cal_year = g.sel_year = now.year;
    g.cal_month = g.sel_month = now.month;
    g.sel_day = now.day;
    g_rem_len = 0;
    g_rem_text[0] = 0;
    damage(cal_area());
    damage(clock_rect());
}

void open_tray() {
    close_menu();
    close_calendar();
    cpu_sample();
    g.tray_open = true;
    g.tray_anim = ANIM_IN;
    g.tray_anim_start = refclock_now_us();
    damage(tray_area());
    damage(panel_rect());
}

void calendar_page(int months) {
    g.cal_month += months;
    while (g.cal_month < 1) { g.cal_month += 12; g.cal_year--; }
    while (g.cal_month > 12) { g.cal_month -= 12; g.cal_year++; }
    damage(cal_rect());
}

void notify(const char* title, const char* text);
void open_kind(Kind kind);
void dismiss_toast(int i);
void notify_reminder(const char* shown, const char* text);
void reminder_advance(Reminder& r, int days, int minutes);

void notify_impl(const char* title, const char* text) {
    int slot = -1;
    for (int i = 0; i < MAX_TOASTS && slot < 0; i++)
        if (!g_toasts[i].used) slot = i;
    if (slot < 0) {
        // Full: the oldest makes room.
        for (int i = 1; i < MAX_TOASTS; i++) g_toasts[i - 1] = g_toasts[i];
        slot = MAX_TOASTS - 1;
        damage({toast_rect(0).x - 8, 0, TOAST_W + 24, toast_rect(MAX_TOASTS).y});
    }
    Toast& t = g_toasts[slot];
    t.used = true;
    t.snooze_text[0] = 0;
    strlcpy(t.title, title, sizeof t.title);
    strlcpy(t.text, text, sizeof t.text);
    // Remembered for the tray's history.
    if (g_history_count == HISTORY_MAX) {
        for (int i = 1; i < HISTORY_MAX; i++) g_history[i - 1] = g_history[i];
        g_history_count--;
    }
    Past& h = g_history[g_history_count++];
    strlcpy(h.title, title, sizeof h.title);
    strlcpy(h.text, text, sizeof h.text);
    DateTime now = local_now();
    h.hour = now.hour;
    h.minute = now.minute;
    if (g.tray_open) damage(tray_area());
    t.start = refclock_now_us();
    t.until = t.start + TOAST_US;
    damage(toast_rect(slot).inset(-8).translated(0, -SLIDE_MENU));
}

void notify(const char* title, const char* text) { notify_impl(title, text); }

// A reminder's toast: the same card with a snooze button.
void notify_reminder(const char* shown, const char* text) {
    notify("Reminder", shown);
    for (int i = MAX_TOASTS - 1; i >= 0; i--) {
        if (!g_toasts[i].used) continue;
        strlcpy(g_toasts[i].snooze_text, text, sizeof g_toasts[i].snooze_text);
        break;
    }
}

void snooze_toast(int i) {
    if (i < 0 || i >= MAX_TOASTS || !g_toasts[i].used || !g_toasts[i].snooze_text[0]) return;
    DateTime now = local_now();
    for (Reminder& r : g_reminders) {
        if (r.used) continue;
        r.used = true;
        r.year = now.year;
        r.month = now.month;
        r.day = now.day;
        r.hour = now.hour;
        r.minute = now.minute;
        r.repeat = 0;
        strlcpy(r.text, g_toasts[i].snooze_text, sizeof r.text);
        reminder_advance(r, 0, 5);
        break;
    }
    reminders_write();
    dismiss_toast(i);
}

void dismiss_toast(int i) {
    if (i < 0 || i >= MAX_TOASTS || !g_toasts[i].used) return;
    damage({toast_rect(0).x - 8, 0, TOAST_W + 24, toast_rect(MAX_TOASTS).y});
    for (int k = i + 1; k < MAX_TOASTS; k++) g_toasts[k - 1] = g_toasts[k];
    g_toasts[MAX_TOASTS - 1].used = false;
}

// Enter in the calendar's line: "14:30 Dentist" or just "Dentist" (at 09:00).
void add_reminder() {
    if (!g_rem_len) return;
    const char* t = g_rem_text;
    int hour = 9, minute = 0;
    if (g_rem_len >= 5 && t[0] >= '0' && t[0] <= '9' && t[1] >= '0' && t[1] <= '9' && t[2] == ':' && t[3] >= '0' &&
        t[3] <= '9' && t[4] >= '0' && t[4] <= '9') {
        hour = (t[0] - '0') * 10 + t[1] - '0';
        minute = (t[3] - '0') * 10 + t[4] - '0';
        t += 5;
        while (*t == ' ') t++;
    }
    if (hour > 23 || minute > 59) return;
    // "daily" or "weekly" first (before or after the time) repeats it.
    u8 repeat = 0;
    auto word = [&](const char* w, u8 kind) {
        usize n = strlen(w);
        if (strncmp(t, w, n) == 0 && (t[n] == ' ' || t[n] == 0)) {
            repeat = kind;
            t += n;
            while (*t == ' ') t++;
        }
    };
    word("daily", 1);
    word("weekly", 2);
    for (Reminder& r : g_reminders) {
        if (r.used) continue;
        r.used = true;
        r.year = (u16)g.sel_year;
        r.month = (u8)g.sel_month;
        r.day = (u8)g.sel_day;
        r.hour = (u8)hour;
        r.minute = (u8)minute;
        r.repeat = repeat;
        strlcpy(r.text, *t ? t : "Reminder", sizeof r.text);
        break;
    }
    g_rem_len = 0;
    g_rem_text[0] = 0;
    reminders_write();
    damage(cal_rect());
}

void calendar_click(int x, int y) {
    for (int i = 0; i < g_cal_ctl_count; i++) {
        const Ctl& c = g_cal_ctls[i];
        if (!c.r.contains(x, y)) continue;
        switch (c.act) {
        case CAL_PREV: calendar_page(-1); break;
        case CAL_NEXT: calendar_page(1); break;
        case CAL_TODAY: {
            DateTime now = local_now();
            g.cal_year = g.sel_year = now.year;
            g.cal_month = g.sel_month = now.month;
            g.sel_day = now.day;
            break;
        }
        case CAL_DAY:
            g.sel_year = g.cal_year;
            g.sel_month = g.cal_month;
            g.sel_day = c.val;
            break;
        case CAL_DELETE:
            g_reminders[c.val].used = false;
            reminders_write();
            break;
        }
        damage(cal_rect());
        return;
    }
}

// Once a second: reminders whose minute has come become notifications.
// Moves a reminder forward by `days` (a repeat, or a snooze of `minutes`).
void reminder_advance(Reminder& r, int days, int minutes) {
    DateTime t{r.year, r.month, r.day, r.hour, r.minute, 0};
    u64 secs = datetime_to_unix(t) + (u64)days * 86400 + (u64)minutes * 60;
    DateTime u = unix_to_datetime(secs);
    r.year = u.year;
    r.month = u.month;
    r.day = u.day;
    r.hour = u.hour;
    r.minute = u.minute;
}

void fire_reminders(const DateTime& now) {
    for (Reminder& r : g_reminders) {
        if (!r.used || r.year != now.year || r.month != now.month || r.day != now.day || r.hour != now.hour ||
            r.minute != now.minute)
            continue;
        if (r.repeat) reminder_advance(r, r.repeat == 1 ? 1 : 7, 0);
        else r.used = false;
        char text[64];
        ksnprintf(text, sizeof text, "%s%s", r.text, r.repeat ? (r.repeat == 1 ? "  (daily)" : "  (weekly)") : "");
        notify_reminder(text, r.text);
        reminders_write();
        if (g.cal_open) damage(cal_rect());
    }
}

void close_context_menu() {
    if (!g.ctx_open) return;
    g.ctx_open = false;
    damage(ctx_area());
}

void switch_desk(int d);

void open_context_menu(int x, int y) {
    close_menu();
    close_calendar();
    g.ctx_open = true;
    g.ctx_win = -1;
    g.ctx_x = x;
    g.ctx_y = y;
    damage(ctx_area());
}

void open_task_menu(int win, int x, int y) {
    open_context_menu(x, y);
    g.ctx_win = win;
    damage(ctx_area());
}

// Super+1..4: another virtual desktop; Super+Shift+1..4 moves the focused
// window there.
void switch_desk(int d) {
    if (d < 0 || d >= DESKS || d == g.cur_desk) return;
    close_menu();
    close_calendar();
    close_context_menu();
    g.cur_desk = d;
    g.focus = -1;
    set_focus(top_visible_window());
    damage_all();
}

void move_window_to_desk(Window& w, int d) {
    if (d < 0 || d >= DESKS || w.desk == d) return;
    damage(anim_bounds(w));
    w.desk = (u8)d;
    w.anim = ANIM_NONE;
    if (g.focus == window_index(&w)) {
        g.focus = -1;
        set_focus(top_visible_window());
    }
    damage(panel_rect());
}

// Super+D: hide every window, or bring back the ones it hid.
void toggle_show_desktop() {
    bool any = false;
    for (int i = 0; i < MAX_WINDOWS; i++) any |= g.windows[i].used && !hidden(g.windows[i]);
    if (any) {
        g.shown_before = 0;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            Window& w = g.windows[i];
            if (!w.used || hidden(w)) continue;
            g.shown_before |= 1u << i;
            minimise(w);
        }
    } else {
        // Bottom to top, so they stack as they did.
        int order[MAX_WINDOWS];
        int count = g.order_count;
        for (int i = 0; i < count; i++) order[i] = g.order[i];
        for (int i = 0; i < count; i++) {
            int idx = order[i];
            if ((g.shown_before & (1u << idx)) && g.windows[idx].used && g.windows[idx].minimised) restore(g.windows[idx]);
        }
        g.shown_before = 0;
    }
}

// Alt+Tab: opens the switcher (next window chosen), or moves on through it.
void switcher_step(int dir) {
    if (!g.switcher_open) {
        g.switcher_count = 0;
        for (int i = g.order_count - 1; i >= 0; i--)
            if (g.windows[g.order[i]].used && g.windows[g.order[i]].desk == g.cur_desk) g.switcher_wins[g.switcher_count++] = g.order[i];
        if (g.switcher_count < 2) {
            g.switcher_count = 0;
            return;
        }
        g.switcher_open = true;
        g.switcher_sel = 0;
        close_menu();
        close_calendar();
        close_context_menu();
    }
    g.switcher_sel = (g.switcher_sel + dir + g.switcher_count) % g.switcher_count;
    damage(switcher_rect().inset(-8).translated(0, 2));
}

void switcher_finish(bool pick) {
    if (!g.switcher_open) return;
    g.switcher_open = false;
    damage(switcher_rect().inset(-8).translated(0, 2));
    if (pick) restore(g.windows[g.switcher_wins[g.switcher_sel]]);
}

// Print Screen: the screen as a BMP file in /tmp.
void save_screenshot() {
    char path[48];
    ksnprintf(path, sizeof path, "/tmp/screenshot-%u.bmp", ++g.screenshot_count);
    const Credentials root = {0, 0};
    Result<Vnode*> v = vfs_create(nullptr, path, root, VType::File, 0644, true);
    if (!v.ok()) {
        notify("Screenshot", "Could not create the file in /tmp.");
        return;
    }
    // BITMAPFILEHEADER + BITMAPINFOHEADER, 32 bits per pixel, rows bottom-up.
    u32 row_bytes = (u32)g.W * 4, image = row_bytes * (u32)g.H;
    u8 hdr[54] = {};
    auto put32 = [&](int at, u32 x) { hdr[at] = (u8)x; hdr[at + 1] = (u8)(x >> 8); hdr[at + 2] = (u8)(x >> 16); hdr[at + 3] = (u8)(x >> 24); };
    hdr[0] = 'B';
    hdr[1] = 'M';
    put32(2, 54 + image);
    put32(10, 54);
    put32(14, 40);
    put32(18, (u32)g.W);
    put32(22, (u32)g.H);
    hdr[26] = 1;
    hdr[28] = 32;
    put32(34, image);
    bool ok = vfs_write(v.value(), 0, hdr, sizeof hdr).ok();
    for (int y = g.H - 1; y >= 0 && ok; y--)
        ok = vfs_write(v.value(), 54 + (u64)(g.H - 1 - y) * row_bytes, g.back.row(y), row_bytes).ok();
    vnode_unref(v.value());
    char msg[64];
    ksnprintf(msg, sizeof msg, ok ? "Saved as %s" : "Writing %s failed", path);
    notify("Screenshot", msg);
}

// ----------------------------------------------------------- persistence --
// Settings and reminders live on /data when a disk labelled "data" is
// mounted there (B-011). Plain text, one "key=value" per line; anything
// unexpected in the file is ignored.
const Credentials ROOT_CRED = {0, 0};

bool write_whole_file(const char* path, const char* text, usize len) {
    Result<Vnode*> v = vfs_create(nullptr, path, ROOT_CRED, VType::File, 0644, false);
    if (!v.ok()) return false;
    bool ok = vfs_truncate(v.value(), 0).ok() && vfs_write(v.value(), 0, text, len).ok();
    vnode_unref(v.value());
    return ok;
}

// Reads a small text file into a NUL-terminated heap buffer, or nullptr.
char* read_whole_file(const char* path, usize max) {
    Result<Vnode*> v = vfs_resolve(nullptr, path, ROOT_CRED, LookupFlags{});
    if (!v.ok()) return nullptr;
    usize size = 0;
    Result<u8*> data = vfs_read_all(v.value(), max, &size);
    vnode_unref(v.value());
    if (!data.ok()) return nullptr;
    char* text = (char*)kmalloc(size + 1);
    if (text) {
        memcpy(text, data.value(), size);
        text[size] = 0;
    }
    kfree(data.value());
    return text;
}

int parse_int(const char* p) {
    bool neg = *p == '-';
    if (neg) p++;
    int v = 0;
    for (; *p >= '0' && *p <= '9' && v < 100000; p++) v = v * 10 + (*p - '0');
    return neg ? -v : v;
}

void prefs_write_now() {
    g_prefs_dirty = false;
    if (!fs_data_mounted()) return;
    char text[512];
    int len = ksnprintf(text, sizeof text,
                        "accent=%d\nhue=%d\nwallpaper=%d\nradius=%d\nglass=%d\nfloating=%d\nclock12=%d\nseconds=%d\n"
                        "fx=%d\nbcolor=%d\nbwidth=%d\nspeed=%d\nball=%d\nglow=%d\nfps=%d\ntz=%d\n"
                        "layout=%d\nrdelay=%d\nrrate=%d\nlockhash=%s\nlocksalt=%s\nnight=%d\nautolock=%d\n",
                        g_prefs.accent, g_prefs.custom_hue, g_prefs.wallpaper, g_prefs.radius, g_prefs.panel_glass,
                        g_prefs.panel_floating, g_prefs.clock_12h, g_prefs.clock_seconds, (int)g_prefs.fx,
                        g_prefs.border_color, g_prefs.border_w, g_prefs.speed, g_prefs.border_all, g_prefs.glow,
                        g_prefs.fps, g_prefs.tz_minutes, g_prefs.layout, g_prefs.repeat_delay, g_prefs.repeat_rate,
                        g_prefs.lock_hash, g_prefs.lock_salt, g_prefs.night_light, g_prefs.autolock_min);
    if (len < 0 || len >= (int)sizeof text) return;
    if (!write_whole_file("/data/desktop.conf", text, (usize)len)) kprintf("gui: could not write /data/desktop.conf\n");
}

// Marks the settings as changed; they are written a moment later, once.
void prefs_changed() {
    g_prefs_dirty = true;
    g_prefs_dirty_us = refclock_now_us();
}

void prefs_load() {
    char* text = read_whole_file("/data/desktop.conf", 4096);
    if (!text) return;
    auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; };
    for (char* line = text; *line;) {
        char* end = line;
        while (*end && *end != '\n') end++;
        bool more = *end == '\n';
        *end = 0;
        char* eq = line;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = 0;
            int v = parse_int(eq + 1);
            const char* k = line;
            if (!strcmp(k, "accent")) g_prefs.accent = clamp(v, 0, ACCENT_COUNT);
            else if (!strcmp(k, "hue")) g_prefs.custom_hue = clamp(v, 0, 1535);
            else if (!strcmp(k, "wallpaper")) g_prefs.wallpaper = clamp(v, 0, WALLPAPER_COUNT - 1);
            else if (!strcmp(k, "radius")) g_prefs.radius = v == 0 || v == 6 ? v : 12;
            else if (!strcmp(k, "glass")) g_prefs.panel_glass = v != 0;
            else if (!strcmp(k, "floating")) g_prefs.panel_floating = v != 0;
            else if (!strcmp(k, "clock12")) g_prefs.clock_12h = v != 0;
            else if (!strcmp(k, "seconds")) g_prefs.clock_seconds = v != 0;
            else if (!strcmp(k, "fx")) g_prefs.fx = (BorderFx)clamp(v, 0, (int)BorderFx::COUNT - 1);
            else if (!strcmp(k, "bcolor")) g_prefs.border_color = clamp(v, -1, ACCENT_COUNT - 1);
            else if (!strcmp(k, "bwidth")) g_prefs.border_w = clamp(v, 1, 4);
            else if (!strcmp(k, "speed")) g_prefs.speed = clamp(v, 0, 2);
            else if (!strcmp(k, "ball")) g_prefs.border_all = v != 0;
            else if (!strcmp(k, "glow")) g_prefs.glow = v != 0;
            else if (!strcmp(k, "fps")) {
                g_prefs.fps = 60;
                for (int f : FPS_CHOICES)
                    if (f == v) g_prefs.fps = v;
            } else if (!strcmp(k, "tz")) g_prefs.tz_minutes = clamp(v, -12 * 60, 14 * 60);
            else if (!strcmp(k, "layout")) g_prefs.layout = clamp(v, 0, KBD_LAYOUTS - 1);
            else if (!strcmp(k, "rdelay")) g_prefs.repeat_delay = clamp(v, 0, 3);
            else if (!strcmp(k, "rrate")) g_prefs.repeat_rate = clamp(v, 0, 2);
            else if (!strcmp(k, "lockhash") && strlen(eq + 1) == 64) strlcpy(g_prefs.lock_hash, eq + 1, sizeof g_prefs.lock_hash);
            else if (!strcmp(k, "locksalt") && strlen(eq + 1) == 32) strlcpy(g_prefs.lock_salt, eq + 1, sizeof g_prefs.lock_salt);
            else if (!strcmp(k, "night")) g_prefs.night_light = v != 0;
            else if (!strcmp(k, "autolock")) g_prefs.autolock_min = v == 1 || v == 5 || v == 15 || v == 30 ? v : 0;
        }
        line = more ? end + 1 : end;
    }
    kfree(text);
}

void reminders_write() {
    if (!fs_data_mounted()) return;
    char text[MAX_REMINDERS * 64];
    usize len = 0;
    for (const Reminder& r : g_reminders) {
        if (!r.used) continue;
        int n = ksnprintf(text + len, sizeof text - len, "%04u-%02u-%02u %02u:%02u %s%s\n", r.year, r.month, r.day, r.hour,
                          r.minute, r.repeat == 1 ? "daily " : r.repeat == 2 ? "weekly " : "", r.text);
        if (n < 0 || len + (usize)n >= sizeof text) break;
        len += (usize)n;
    }
    if (!write_whole_file("/data/reminders.txt", text, len)) kprintf("gui: could not write /data/reminders.txt\n");
}

void reminders_load() {
    char* text = read_whole_file("/data/reminders.txt", 8192);
    if (!text) return;
    int count = 0;
    for (char* line = text; *line && count < MAX_REMINDERS;) {
        char* end = line;
        while (*end && *end != '\n') end++;
        bool more = *end == '\n';
        *end = 0;
        // "YYYY-MM-DD HH:MM text"
        auto digits = [&](const char* p, int n) {
            for (int i = 0; i < n; i++)
                if (p[i] < '0' || p[i] > '9') return false;
            return true;
        };
        if (end - line >= 17 && digits(line, 4) && line[4] == '-' && digits(line + 5, 2) && line[7] == '-' &&
            digits(line + 8, 2) && line[10] == ' ' && digits(line + 11, 2) && line[13] == ':' && digits(line + 14, 2)) {
            int y = parse_int(line), mo = parse_int(line + 5), d = parse_int(line + 8), h = parse_int(line + 11),
                mi = parse_int(line + 14);
            const char* t = line + 16;
            while (*t == ' ') t++;
            u8 repeat = 0;
            if (!strncmp(t, "daily ", 6)) { repeat = 1; t += 6; }
            else if (!strncmp(t, "weekly ", 7)) { repeat = 2; t += 7; }
            if (y >= 2000 && y <= 2200 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h <= 23 && mi <= 59 && *t) {
                Reminder& r = g_reminders[count++];
                r.used = true;
                r.repeat = repeat;
                r.year = (u16)y;
                r.month = (u8)mo;
                r.day = (u8)d;
                r.hour = (u8)h;
                r.minute = (u8)mi;
                strlcpy(r.text, t, sizeof r.text);
            }
        }
        line = more ? end + 1 : end;
    }
    kfree(text);
}

// ------------------------------------------------------------ lock screen --
void hex_of(const u8* bytes, usize n, char* out) {
    static const char* D = "0123456789abcdef";
    for (usize i = 0; i < n; i++) {
        out[i * 2] = D[bytes[i] >> 4];
        out[i * 2 + 1] = D[bytes[i] & 15];
    }
    out[n * 2] = 0;
}

bool unhex(const char* hex, u8* out, usize n) {
    auto val = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (usize i = 0; i < n; i++) {
        int hi = val(hex[i * 2]), lo = hi < 0 ? -1 : val(hex[i * 2 + 1]);
        if (lo < 0) return false;
        out[i] = (u8)(hi << 4 | lo);
    }
    return hex[n * 2] == 0;
}

bool password_set() { return g_prefs.lock_hash[0] != 0; }

void password_hash(const u8* salt, const char* text, usize len, u8 out[32]) {
    Sha256 h;
    h.init();
    h.update(salt, 16);
    h.update(text, len);
    h.final(out);
}

void set_password(const char* text, usize len) {
    u8 salt[16], digest[32];
    csprng_bytes(salt, sizeof salt);
    password_hash(salt, text, len, digest);
    hex_of(salt, 16, g_prefs.lock_salt);
    hex_of(digest, 32, g_prefs.lock_hash);
    memset(digest, 0, sizeof digest);
    prefs_changed();
}

// Constant time: the comparison takes as long whatever the difference.
bool password_matches(const char* text, usize len) {
    u8 salt[16], want[32], got[32];
    if (!password_set() || !unhex(g_prefs.lock_salt, salt, 16) || !unhex(g_prefs.lock_hash, want, 32)) return false;
    password_hash(salt, text, len, got);
    u8 diff = 0;
    for (int i = 0; i < 32; i++) diff |= (u8)(want[i] ^ got[i]);
    memset(got, 0, sizeof got);
    return diff == 0;
}

void lock_screen() {
    if (!password_set()) {
        notify("Lock screen", "Set a password first: Settings, Security.");
        return;
    }
    close_menu();
    close_calendar();
    close_context_menu();
    switcher_finish(false);
    close_tray();
    g_locked = true;
    g_pw_len = 0;
    g_pw_text[0] = 0;
    g_pw_wrong = false;
    damage_all();
}

void draw_lock_screen(Surface& back) {
    Rect all{0, 0, g.W, g.H};
    Surface wv = g.wall.sub(all.intersect(back.clip));
    blit(back, back.clip.x, back.clip.y, wv);
    fill_rect(back, all, rgba(0, 0, 0, 120));
    DateTime now = local_now();
    char line[64];
    u32 hour = now.hour;
    const char* suffix = "";
    if (g_prefs.clock_12h) {
        suffix = hour >= 12 ? " PM" : " AM";
        hour = hour % 12 ? hour % 12 : 12;
    }
    ksnprintf(line, sizeof line, "%02u:%02u%s", hour, now.minute, suffix);
    const Font& big = g.W >= 1100 ? g.display_big : g.display;
    int tw = measure_text(big, line);
    int cy = g.H / 2 - 120;
    draw_text(back, big, (g.W - tw) / 2, cy - big.height, line, theme::TEXT);
    ksnprintf(line, sizeof line, "%s %d %s %d", DAY_LONG[weekday(now.year, now.month, now.day)], now.day,
              MONTH_NAMES[now.month - 1], now.year);
    tw = measure_text(g.font, line);
    draw_text(back, g.font, (g.W - tw) / 2, cy + 8, line, theme::TEXT_MUTED);
    // The password field: a dot per character.
    Rect f{(g.W - 300) / 2, cy + 60, 300, 40};
    fill_rect_rounded(back, f, 12, rgba(255, 255, 255, 18));
    stroke_rect_rounded(back, f, 12, g_pw_wrong ? rgb(248, 113, 113) : with_alpha(accent(), 150));
    if (g_pw_len) {
        for (int i = 0; i < g_pw_len && i < 24; i++) fill_circle_aa(back, f.x + 18 + i * 11, f.y + 20, 4, theme::TEXT);
    } else {
        draw_text(back, g.font, f.x + 14, f.y + (f.h - g.font.height) / 2, "Password", theme::TEXT_MUTED);
    }
    const char* msg = g_pw_wrong ? "That is not the password." : "Enter to unlock";
    tw = measure_text(g.font, msg);
    draw_text(back, g.font, (g.W - tw) / 2, f.bottom() + 12, msg, g_pw_wrong ? rgb(248, 113, 113) : theme::TEXT_MUTED);
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
    case ACT_TAB:
        g_settings_tab = val;
        g_settings_scroll = 0;
        break;
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
        if (val < 0) return;
        if (!set_resolution(MODES[val].w, MODES[val].h)) kprintf("gui: could not switch to %dx%d\n", MODES[val].w, MODES[val].h);
        break;
    case ACT_FPS: g_prefs.fps = val; break;
    case ACT_PW_FIELD:
        g_pw_editing = true;
        g_pw_len = 0;
        g_pw_text[0] = 0;
        break;
    case ACT_PW_SET:
        if (g_pw_len) set_password(g_pw_text, (usize)g_pw_len);
        else g_pw_editing = true;
        memset(g_pw_text, 0, sizeof g_pw_text);
        g_pw_len = 0;
        if (password_set()) g_pw_editing = false;
        break;
    case ACT_PW_CLEAR:
        g_prefs.lock_hash[0] = 0;
        g_prefs.lock_salt[0] = 0;
        g_pw_editing = false;
        break;
    case ACT_LOCK: lock_screen(); break;
    case ACT_AUTOLOCK: g_prefs.autolock_min = val; break;
    case ACT_LAYOUT:
        g_prefs.layout = val;
        ps2kbd_set_layout(val);
        break;
    case ACT_RDELAY:
        g_prefs.repeat_delay = val;
        ps2kbd_set_repeat(val, g_prefs.repeat_rate);
        break;
    case ACT_RRATE:
        g_prefs.repeat_rate = val;
        ps2kbd_set_repeat(g_prefs.repeat_delay, val);
        break;
    case ACT_TZ: {
        int tz = g_prefs.tz_minutes + val;
        g_prefs.tz_minutes = tz < -12 * 60 ? -12 * 60 : tz > 14 * 60 ? 14 * 60 : tz;
        break;
    }
    case ACT_FORMAT: {
        if (g_format_confirm != val) {
            g_format_confirm = val;         // first press: ask again
            break;
        }
        g_format_confirm = -1;
        DiskInfo disks[8];
        u32 count = fs_disks(disks, 8);
        if (val < 0 || (u32)val >= count) break;
        Result<void> r = fs_data_format(disks[val].name);
        if (r.ok()) {
            notify("Storage", "Settings and reminders are now saved on /data.");
            prefs_write_now();
            reminders_write();
        } else {
            notify("Storage", "The disk could not be set up.");
        }
        break;
    }
    default: return;
    }
    if (act != ACT_TAB && act != ACT_FORMAT && act != ACT_PW_FIELD && act != ACT_LOCK) prefs_changed();
    if (act != ACT_PW_FIELD && act != ACT_PW_SET) g_pw_editing = false;
    if (act != ACT_FORMAT) g_format_confirm = -1;
    refresh_everything();
}

void prefs_changed();
void reminders_write();

// The custom accent colour under x on the hue strip (content coordinates).
void hue_pick(int x) {
    if (g_hue_rect.w < 2) return;
    int hue = (x - g_hue_rect.x) * 1535 / (g_hue_rect.w - 1);
    hue = hue < 0 ? 0 : hue > 1535 ? 1535 : hue;
    if (g_prefs.accent == ACCENT_COUNT && g_prefs.custom_hue == hue) return;
    g_prefs.accent = ACCENT_COUNT;
    g_prefs.custom_hue = hue;
    prefs_changed();
    refresh_everything();
}

// A click inside the Settings window, in its content's coordinates.
void settings_click(int x, int y) {
    for (int i = 0; i < g_ctl_count; i++) {
        if (!g_ctls[i].r.contains(x, y)) continue;
        if (g_ctls[i].act == ACT_HUE) {
            g.hue_drag = true;
            hue_pick(x);
            return;
        }
        settings_apply(g_ctls[i].act, g_ctls[i].val);
        return;
    }
}

void menu_activate(int item) {
    if (item == ITEM_POWER || item == ITEM_RESTART) {
        if (item == ITEM_POWER && !power_can_power_off()) return;
        // A second press within five seconds does it; the first only arms.
        u64 now = refclock_now_us();
        if (now - g.power_armed_us > 5000000) {
            g.power_armed_us = now;
            damage(menu_rect());
            return;
        }
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
    if (g_locked) return;
    Hit h = hit_test(g.mx, g.my);
    if (button != 0) {
        if (h.what == Hit::Nothing && g.menu_open) close_menu();
        if (h.what == Hit::Nothing && g.cal_open) close_calendar();
        if (h.what != Hit::Context) close_context_menu();
        // Middle-click on a taskbar button closes its window; right-click
        // on the desktop opens the menu.
        if (button == 2 && h.what == Hit::Task) destroy_window(h.win);
        else if (button == 1 && h.what == Hit::Task) open_task_menu(h.win, g.mx, g.my);
        else if (button == 1 && h.what == Hit::Nothing && !g.menu_open && !g.cal_open) open_context_menu(g.mx, g.my);
        return;
    }
    if (g.menu_open && h.what != Hit::Menu && h.what != Hit::Launcher) close_menu();
    if (g.cal_open && h.what != Hit::Calendar && h.what != Hit::Clock) close_calendar();
    if (g.tray_open && h.what != Hit::TrayPopup && h.what != Hit::Tray) close_tray();
    if (g.ctx_open && h.what != Hit::Context) close_context_menu();
    u64 now = refclock_now_us();
    bool double_click = h.what == Hit::Title && h.win == g.last_click_win && now - g.last_click_us < 400000;
    g.last_click_us = now;
    g.last_click_win = h.what == Hit::Title ? h.win : -1;
    switch (h.what) {
    case Hit::Context:
        if (g.ctx_win >= 0) {
            int win = g.ctx_win;
            close_context_menu();
            if (!g.windows[win].used) break;
            Window& tw = g.windows[win];
            switch (h.item) {
            case 0: restore(tw); unsnap(tw); break;
            case 1: minimise(tw); break;
            case 2: restore(tw); if (!tw.maximised) toggle_maximise(tw); break;
            case 3: restore(tw); snap_window(tw, 1); break;
            case 4: restore(tw); snap_window(tw, 2); break;
            case 5: destroy_window(win); break;
            default: break;
            }
            break;
        }
        close_context_menu();
        switch (h.item) {
        case 0: open_kind(Kind::Terminal); break;
        case 1: open_kind(Kind::Files); break;
        case 2: open_kind(Kind::Notes); break;
        case 3: open_kind(Kind::Settings); break;
        case 4:
            g_settings_tab = 1;
            open_kind(Kind::Settings);
            for (int i = 0; i < MAX_WINDOWS; i++)
                if (g.windows[i].used && g.windows[i].kind == Kind::Settings) g.windows[i].needs_paint = true;
            break;
        case 5: toggle_show_desktop(); break;
        case 6: open_kind(Kind::About); break;
        default: break;
        }
        break;
    case Hit::Toast: {
        Rect r = toast_rect(h.item);
        Rect sb{r.right() - 86, r.bottom() - 30, 70, 22};
        if (g_toasts[h.item].snooze_text[0] && sb.contains(g.mx, g.my)) snooze_toast(h.item);
        else dismiss_toast(h.item);
        break;
    }
    case Hit::Tray:
        if (g.tray_open) close_tray();
        else open_tray();
        break;
    case Hit::Desk: switch_desk(h.item); break;
    case Hit::TrayPopup:
        for (int i = 0; i < g_tray_ctl_count; i++) {
            const Ctl& c = g_tray_ctls[i];
            if (!c.r.contains(g.mx, g.my)) continue;
            switch (c.act) {
            case TRAY_NIGHT:
                g_prefs.night_light = !g_prefs.night_light;
                prefs_changed();
                damage_all();
                break;
            case TRAY_DESKTOP: close_tray(); toggle_show_desktop(); break;
            case TRAY_LOCK: close_tray(); lock_screen(); break;
            case TRAY_SETTINGS: close_tray(); open_kind(Kind::Settings); break;
            case TRAY_ACCENT: g_prefs.accent = c.val; prefs_changed(); refresh_everything(); break;
            case TRAY_FPS: g_prefs.fps = c.val; prefs_changed(); break;
            case TRAY_CLEAR: g_history_count = 0; damage(tray_area()); damage_all(); break;
            }
            damage(tray_area());
            break;
        }
        break;
    case Hit::Clock:
        if (g.cal_open) close_calendar();
        else open_calendar();
        break;
    case Hit::Calendar: calendar_click(g.mx, g.my); break;
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
        if (double_click) {
            toggle_maximise(g.windows[h.win]);
            g.last_click_win = -1;
            break;
        }
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
        } else if (h.win == g.term_win) {
            Rect cr = content_rect(g.windows[h.win]);
            console_lock();
            g.term.select_begin(g.mx - cr.x, g.my - cr.y);
            console_unlock();
            g.term_dirty = true;
            g.drag = Drag::Select;
            g.drag_win = h.win;
        } else {
            Window& w = g.windows[h.win];
            Rect cr = content_rect(w);
            int cx = g.mx - cr.x, cy = g.my - cr.y;
            bool again = h.win == g.last_content_win && now - g.last_content_us < 400000;
            g.last_content_us = now;
            g.last_content_win = h.win;
            bool repaint = false;
            if (w.kind == Kind::Calculator) repaint = g_calc.click(cx, cy, 0, app_ctx());
            else if (w.kind == Kind::SystemMonitor) { sysmon_click(cx, cy); repaint = true; }
            else if (w.kind == Kind::Files) repaint = g_files.click(cx, cy, 0, again ? 2 : 1, app_ctx());
            else if (w.kind == Kind::Notes) {
                repaint = g_notes.click(cx, cy, 0, false, app_ctx());
                g.drag = Drag::Select;
                g.drag_win = h.win;
            }
            if (repaint) w.needs_paint = true;
            serve_open_request();
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
    g.hue_drag = false;
    if (g.drag == Drag::Select) {
        // What was swept out is the clipboard's now.
        char text[sizeof g_clipboard];
        console_lock();
        usize len = g.term.selection_text(text, sizeof text);
        console_unlock();
        if (len) clipboard_set(text, len);
    }
    if (g.drag == Drag::Move && g.drag_win >= 0 && g.windows[g.drag_win].used && g.snap_preview) {
        Window& w = g.windows[g.drag_win];
        if (g.snap_preview == 3) { if (!w.maximised) toggle_maximise(w); }
        else snap_window(w, g.snap_preview);
    }
    if (g.snap_preview) {
        damage(snap_target(g.snap_preview).inset(-2));
        g.snap_preview = 0;
    }
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
    if (g.hue_drag) {
        // Dragging along the hue strip of the Settings window.
        if (g.focus >= 0 && g.windows[g.focus].kind == Kind::Settings) hue_pick(g.mx - content_rect(g.windows[g.focus]).x);
        return;
    }
    if (g.drag == Drag::None) {
        Hit over = hit_test(g.mx, g.my);
        set_cursor_shape(over.what == Hit::Edge ? resize_shape(over.edges) : CUR_ARROW);
    }
    if (g.drag != Drag::None && g.drag_win >= 0 && g.windows[g.drag_win].used) {
        Window& w = g.windows[g.drag_win];
        int dx = g.mx - g.drag_sx, dy = g.my - g.drag_sy;
        if (g.drag == Drag::Select) {
            Rect cr = content_rect(w);
            if (g.drag_win == g.term_win) {
                console_lock();
                g.term.select_extend(g.mx - cr.x, g.my - cr.y);
                console_unlock();
                g.term_dirty = true;
            } else if (w.kind == Kind::Notes) {
                if (g_notes.click(g.mx - cr.x, g.my - cr.y, 0, true, app_ctx())) w.needs_paint = true;
            }
            return;
        }
        if (g.drag == Drag::Move) {
            if (w.maximised || w.snapped) {
                // Dragging a maximised or snapped window lets go of that:
                // it takes its old size under the pointer.
                if (dx * dx + dy * dy < 16) return;
                Rect r = w.restore;
                r.x = g.mx - r.w * (g.drag_sx - w.frame.x) / (w.frame.w > 0 ? w.frame.w : 1);
                r.y = g.my - (g.drag_sy - w.frame.y);
                w.maximised = false;
                w.snapped = 0;
                set_window_frame(w, r);
                g.drag_start = w.frame.translated(-dx, -dy);
            }
            set_window_frame(w, g.drag_start.translated(dx, dy));
            // At an edge of the screen: show where a drop would put it.
            int preview = g.mx <= 0 ? 1 : g.mx >= g.W - 1 ? 2 : g.my <= 0 ? 3 : 0;
            if (preview != g.snap_preview) {
                if (g.snap_preview) damage(snap_target(g.snap_preview).inset(-2));
                g.snap_preview = preview;
                if (preview) damage(snap_target(preview).inset(-2));
            }
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
    if (h.what == Hit::Calendar || last_what == Hit::Calendar) damage(cal_rect());
    if (h.what == Hit::Context || last_what == Hit::Context) damage(ctx_rect());
    if (h.what == Hit::TrayPopup || last_what == Hit::TrayPopup) damage(tray_rect());
    bool panel_now = h.what == Hit::Panel || h.what == Hit::Launcher || h.what == Hit::Task || h.what == Hit::Clock || h.what == Hit::Tray || h.what == Hit::Desk;
    bool panel_before = last_what == Hit::Panel || last_what == Hit::Launcher || last_what == Hit::Task || last_what == Hit::Clock || last_what == Hit::Tray || last_what == Hit::Desk;
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

void process_keyboard() {
    KeyEvent e;
    while (ps2kbd_poll_event(&e)) {
        g.last_input_us = refclock_now_us();
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
        if (e.key == key::ALT && !e.pressed) switcher_finish(true);
        if ((e.key == key::CAPS_LOCK || e.key == key::NUM_LOCK) && e.pressed) {
            damage(panel_rect());
            for (int i = 0; i < MAX_WINDOWS; i++)
                if (g.windows[i].used && g.windows[i].kind == Kind::Settings) g.windows[i].needs_paint = true;
        }
        if (!e.pressed) continue;
        if (g.super_down) g.super_chord = true;
        bool alt = e.mods & mod::ALT, super_ = e.mods & mod::SUPER, shift = e.mods & mod::SHIFT;
        if (g_locked) {
            if (e.key == key::ENTER) {
                if (refclock_now_us() < g_unlock_retry_us) continue;
                if (password_matches(g_pw_text, (usize)g_pw_len)) {
                    g_locked = false;
                    memset(g_pw_text, 0, sizeof g_pw_text);
                    g_pw_len = 0;
                    damage_all();
                } else {
                    g_pw_wrong = true;
                    g_pw_len = 0;
                    g_pw_text[0] = 0;
                    g_unlock_retry_us = refclock_now_us() + 1000000;
                }
            } else if (e.key == key::BACKSPACE) {
                if (g_pw_len) g_pw_text[--g_pw_len] = 0;
            } else if (e.ascii >= 32 && e.ascii < 127 && !alt && !super_ && g_pw_len < (int)sizeof g_pw_text - 1) {
                g_pw_text[g_pw_len++] = e.ascii;
                g_pw_text[g_pw_len] = 0;
                g_pw_wrong = false;
            }
            damage({0, g.H / 2 - 200, g.W, 300});
            continue;
        }
        if (super_ && e.ascii == 'l') { lock_screen(); continue; }
        if (g.switcher_open) {
            if (e.key == key::TAB) switcher_step(shift ? -1 : 1);
            else if (e.key == key::ESCAPE) switcher_finish(false);
            continue;
        }
        if (alt && e.key == key::F1 + 3) {                 // Alt+F4
            if (g.focus >= 0) destroy_window(g.focus);
            continue;
        }
        if (alt && e.key == key::TAB) { switcher_step(shift ? -1 : 1); continue; }
        if (e.key == key::PRINT) { save_screenshot(); continue; }
        if (super_ && e.ascii == 't') { close_menu(); close_calendar(); open_kind(Kind::Terminal); continue; }
        if (super_ && e.ascii == 'm') { if (g.focus >= 0) toggle_maximise(g.windows[g.focus]); continue; }
        if (super_ && e.ascii == 'd') { close_menu(); close_calendar(); toggle_show_desktop(); continue; }
        if (super_ && e.keycode >= 0x02 && e.keycode < 0x02 + DESKS) {     // the 1..4 keys, whatever they type
            int d = (int)e.keycode - 0x02;
            if (shift) { if (g.focus >= 0) move_window_to_desk(g.windows[g.focus], d); }
            else switch_desk(d);
            continue;
        }
        if (super_ && (e.key == key::LEFT || e.key == key::RIGHT || e.key == key::UP || e.key == key::DOWN)) {
            if (g.focus >= 0) {
                Window& w = g.windows[g.focus];
                if (e.key == key::LEFT) snap_window(w, 1);
                else if (e.key == key::RIGHT) snap_window(w, 2);
                else if (e.key == key::UP) { if (!w.maximised) toggle_maximise(w); }
                else if (w.maximised || w.snapped) unsnap(w);
                else minimise(w);
            }
            continue;
        }
        if (e.key == key::ESCAPE && g.menu_open) { close_menu(); continue; }
        if (g.cal_open) {
            if (e.key == key::ESCAPE) close_calendar();
            else if (e.key == key::ENTER) add_reminder();
            else if (e.key == key::LEFT || e.key == key::PAGE_UP) calendar_page(-1);
            else if (e.key == key::RIGHT || e.key == key::PAGE_DOWN) calendar_page(1);
            else if (e.key == key::BACKSPACE) { if (g_rem_len) g_rem_text[--g_rem_len] = 0; }
            else if (e.ascii >= 32 && e.ascii < 127 && !alt && !super_ && g_rem_len < (int)sizeof g_rem_text - 1) {
                g_rem_text[g_rem_len++] = e.ascii;
                g_rem_text[g_rem_len] = 0;
            }
            damage(cal_rect());
            continue;
        }
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
        if (g_pw_editing && g.focus >= 0 && g.windows[g.focus].kind == Kind::Settings) {
            if (e.key == key::ENTER) {
                if (g_pw_len) set_password(g_pw_text, (usize)g_pw_len);
                g_pw_editing = false;
                memset(g_pw_text, 0, sizeof g_pw_text);
                g_pw_len = 0;
            } else if (e.key == key::ESCAPE) {
                g_pw_editing = false;
                g_pw_len = 0;
                g_pw_text[0] = 0;
            } else if (e.key == key::BACKSPACE) {
                if (g_pw_len) g_pw_text[--g_pw_len] = 0;
            } else if (e.ascii >= 32 && e.ascii < 127 && g_pw_len < (int)sizeof g_pw_text - 1) {
                g_pw_text[g_pw_len++] = e.ascii;
                g_pw_text[g_pw_len] = 0;
            }
            g.windows[g.focus].needs_paint = true;
            continue;
        }
        if (g.focus >= 0 && g.windows[g.focus].used) {
            Window& fw = g.windows[g.focus];
            bool handled = false, repaint = false;
            if (fw.kind == Kind::Calculator) { handled = true; repaint = g_calc.key(e, app_ctx()); }
            else if (fw.kind == Kind::Notes) { handled = true; repaint = g_notes.key(e, app_ctx()); }
            else if (fw.kind == Kind::Files) { handled = true; repaint = g_files.key(e, app_ctx()); }
            if (handled) {
                if (repaint) fw.needs_paint = true;
                serve_open_request();
                continue;
            }
        }
        bool ctrl = e.mods & mod::CTRL;
        if (ctrl && (e.ascii == 0x16 || e.ascii == 'v' || e.ascii == 'V')) {
            // Paste into whatever has the keyboard.
            if (g.cal_open) {
                for (usize i = 0; i < g_clip_len && g_rem_len < (int)sizeof g_rem_text - 1; i++)
                    if (g_clipboard[i] >= 32 && g_clipboard[i] < 127) g_rem_text[g_rem_len++] = g_clipboard[i];
                g_rem_text[g_rem_len] = 0;
                damage(cal_rect());
            } else if (g.menu_open) {
                for (usize i = 0; i < g_clip_len && g_search_len < (int)sizeof g_search - 1; i++)
                    if (g_clipboard[i] >= 32 && g_clipboard[i] < 127) g_search[g_search_len++] = g_clipboard[i];
                g_search[g_search_len] = 0;
                search_update();
                damage(menu_rect());
                damage(search_rect());
            } else if (g.focus >= 0 && g.focus == g.term_win) {
                if (g.term.scrolled_back()) terminal_scroll(0, true);
                for (usize i = 0; i < g_clip_len; i++) term_input_push(g_clipboard[i] == '\n' ? '\r' : g_clipboard[i]);
            }
            continue;
        }
        if (ctrl && (e.ascii == 0x03 || e.ascii == 'c' || e.ascii == 'C') && g.focus >= 0 && g.focus == g.term_win) {
            char text[sizeof g_clipboard];
            console_lock();
            usize len = g.term.selection_text(text, sizeof text);
            console_unlock();
            if (len) clipboard_set(text, len);
            continue;
        }
        if (g.focus < 0 || g.focus != g.term_win) continue;
        // Shift+Page Up/Down scroll the terminal's history by a screen.
        if (shift && (e.key == key::PAGE_UP || e.key == key::PAGE_DOWN)) {
            int page = g.term.rows() > 2 ? g.term.rows() - 2 : 1;
            terminal_scroll(e.key == key::PAGE_UP ? page : -page);
            continue;
        }
        // Up/Down walk the shell's history (Ctrl-P/Ctrl-N on the serial
        // line); Tab completes a command.
        char c = e.ascii;
        if (e.key == key::UP) c = 0x10;
        else if (e.key == key::DOWN) c = 0x0E;
        else if (e.key == key::TAB) c = '\t';
        if (c) {
            if (g.term.scrolled_back()) terminal_scroll(0, true);      // typing returns to the bottom
            if (g.term.has_selection()) {
                console_lock();
                g.term.select_clear();
                console_unlock();
                g.term_dirty = true;
            }
            term_input_push(c);
        }
    }
}

void process_mouse() {
    MouseEvent e;
    while (ps2mouse_poll(&e)) {
        g.last_input_us = refclock_now_us();
        if (e.absolute) {
            int nx = (int)((u32)e.ax * (u32)(g.W - 1) / 65535), ny = (int)((u32)e.ay * (u32)(g.H - 1) / 65535);
            if (nx != g.mx || ny != g.my) {
                move_cursor(nx, ny);
                on_motion();
            }
        } else if (e.dx || e.dy) {
            move_cursor(g.mx + e.dx, g.my + e.dy);
            on_motion();
        }
        if (e.dz) {
            Hit h = hit_test(g.mx, g.my);
            if (h.what == Hit::Content && h.win >= 0 && h.win == g.term_win) terminal_scroll(e.dz * 3);
            else if (h.what == Hit::Calendar) calendar_page(e.dz > 0 ? -1 : 1);
            else if (h.what == Hit::Content && h.win >= 0) {
                Window& w = g.windows[h.win];
                if (w.kind == Kind::Notes && g_notes.wheel(e.dz)) w.needs_paint = true;
                if (w.kind == Kind::Files && g_files.wheel(e.dz)) w.needs_paint = true;
                if (w.kind == Kind::SystemMonitor) {
                    g_proc_scroll -= e.dz * 3;
                    w.needs_paint = true;
                }
                if (w.kind == Kind::Settings) {
                    int max_scroll = g_settings_height - content_rect(w).h;
                    int want = g_settings_scroll - e.dz * 40;
                    if (want > max_scroll) want = max_scroll;
                    if (want < 0) want = 0;
                    if (want != g_settings_scroll) {
                        g_settings_scroll = want;
                        w.needs_paint = true;
                    }
                }
            }
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
        // Ocean: deep water, lit from above.
        {rgb(4, 16, 36), rgb(6, 36, 58),
         {{44, 10, 32, rgba(14, 165, 233, 110)}, {12, 44, 26, rgba(45, 212, 191, 80)}, {34, 60, 22, rgba(59, 130, 246, 70)}}},
        // Sunset: the last orange under a violet sky.
        {rgb(30, 14, 48), rgb(58, 20, 30),
         {{30, 58, 34, rgba(251, 146, 60, 125)}, {8, 20, 24, rgba(168, 85, 247, 90)}, {52, 26, 22, rgba(244, 63, 94, 80)}}},
        // Graphite: almost no colour, for those who want the windows to carry it.
        {rgb(14, 15, 18), rgb(24, 26, 31),
         {{46, 10, 30, rgba(148, 163, 184, 50)}, {10, 52, 26, rgba(100, 116, 139, 45)}, {0, 0, 0, 0}}},
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
    for (int i = 0; i < g_ghost_count; i++) {
        free_pixels(g_ghosts[i].w.pixels);
        free_pixels(g_ghosts[i].w.shadow);
    }
    g_ghost_count = 0;
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
        w.anim = ANIM_NONE;
        if (i == g.term_win) terminal_fit(w);
    }
    render_wallpaper(g.wall, g_prefs.wallpaper, true);
    if (g.mx >= nw) g.mx = nw - 1;
    if (g.my >= nh) g.my = nh - 1;
    g.cursor_drawn = false;
    g.cal_open = false;
    g.cal_anim = ANIM_NONE;
    g.tray_open = false;
    g.tray_anim = ANIM_NONE;
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

    prefs_load();
    reminders_load();
    ps2kbd_set_layout(g_prefs.layout);
    if (g_prefs.repeat_delay != 1 || g_prefs.repeat_rate != 2) ps2kbd_set_repeat(g_prefs.repeat_delay, g_prefs.repeat_rate);
    if (!g_notes.init()) kprintf("gui: no memory for the Notes buffer\n");
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

void gui_terminal_clear() {
    if (!g.active) return;
    console_lock();
    g.term.clear();
    console_unlock();
    g.term_dirty = true;
    g_compositor_wake_hint();
}

int gui_terminal_getc() {
    if (__atomic_load_n(&g.term_in_head, __ATOMIC_ACQUIRE) == g.term_in_tail) return -1;
    char c = g.term_in[g.term_in_tail];
    __atomic_store_n(&g.term_in_tail, (g.term_in_tail + 1) % sizeof g.term_in, __ATOMIC_RELEASE);
    return (unsigned char)c;
}

bool gui_notify(const char* title, const char* text) {
    if (!g.active) return false;
    u32 head = g_pending_head;
    if (head - g_pending_tail >= MAX_TOASTS) return false;
    Toast& t = g_pending[head % MAX_TOASTS];
    strlcpy(t.title, title, sizeof t.title);
    strlcpy(t.text, text, sizeof t.text);
    __atomic_store_n(&g_pending_head, head + 1, __ATOMIC_RELEASE);
    g_compositor_wake_hint();
    return true;
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
    ps2kbd_update_leds();

    while (__atomic_load_n(&g_pending_head, __ATOMIC_ACQUIRE) != g_pending_tail) {
        const Toast& t = g_pending[g_pending_tail % MAX_TOASTS];
        notify(t.title, t.text);
        __atomic_store_n(&g_pending_tail, g_pending_tail + 1, __ATOMIC_RELEASE);
    }
    DateTime now = local_now();
    if (now.second != g.last_second) {
        g.last_second = now.second;
        damage(clock_rect());
        if (now.second == 0 || g.last_frame_us == 0) fire_reminders(now);
        if (g_locked && now.second == 0) damage({0, g.H / 2 - 200, g.W, 300});
        if (!g_locked && g_prefs.autolock_min && password_set() &&
            refclock_now_us() - g.last_input_us > (u64)g_prefs.autolock_min * 60000000ull)
            lock_screen();
        if (g.cal_open) damage(cal_rect());         // "today" can change
        if (g.tray_open) {
            cpu_sample();
            damage(tray_rect());
        }
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (g.windows[i].used && g.windows[i].kind == Kind::SystemMonitor) {
                sysmon_sample();
                g.windows[i].needs_paint = true;
            }
    }

    u64 now_us = refclock_now_us();
    if (g_prefs_dirty && now_us - g_prefs_dirty_us > 800000) prefs_write_now();
    // An animated border redraws about 30 times a second; nothing else on
    // an idle desktop moves.
    if (fx_animated() && now_us - g_last_anim_us >= 33000) {
        g_last_anim_us = now_us;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            const Window& w = g.windows[i];
            if (w.used && !hidden(w) && (g_prefs.border_all || i == g.focus)) damage(w.frame.inset(-6));
        }
    }
    if (now_us - g.last_frame_us < frame_us()) return;

    // Windows and the launcher part-way through fading: another frame each,
    // and one last frame when they are done.
    for (int i = 0; i < MAX_WINDOWS; i++) {
        Window& w = g.windows[i];
        if (!w.used || !w.anim) continue;
        damage(anim_bounds(w));
        if (now_us - w.anim_start >= ANIM_US) w.anim = ANIM_NONE;
    }
    for (int i = 0; i < g_ghost_count;) {
        Ghost& gh = g_ghosts[i];
        damage(anim_bounds(gh.w));
        if (now_us - gh.start < ANIM_US) {
            i++;
            continue;
        }
        free_pixels(gh.w.pixels);
        free_pixels(gh.w.shadow);
        gh = g_ghosts[--g_ghost_count];
    }
    if (g.menu_anim) {
        damage(menu_area());
        if (now_us - g.menu_anim_start >= ANIM_US) g.menu_anim = ANIM_NONE;
    }
    if (g.cal_anim) {
        damage(cal_area());
        if (now_us - g.cal_anim_start >= ANIM_US) g.cal_anim = ANIM_NONE;
    }
    if (g.tray_anim) {
        damage(tray_area());
        if (now_us - g.tray_anim_start >= ANIM_US) g.tray_anim = ANIM_NONE;
    }
    for (int i = 0; i < MAX_TOASTS; i++) {
        Toast& t = g_toasts[i];
        if (!t.used) continue;
        if (now_us - t.start < ANIM_US) damage(toast_rect(i).inset(-8).translated(0, -SLIDE_MENU));
        if (now_us >= t.until) dismiss_toast(i--);
    }

    for (int i = 0; i < g.order_count; i++) {
        Window& w = g.windows[g.order[i]];
        if (!w.used || hidden(w)) continue;
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

void g_compositor_wake_hint() { g_compositor_wake.wake_one(); }

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
