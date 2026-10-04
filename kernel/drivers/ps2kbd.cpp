// See ps2kbd.h.
//
// The keyboard is asked for scancode set 2 with the controller's
// translation switched off. Each set-2 code is mapped to the key's set-1
// make code (the table the controller itself would apply), so one key map
// serves both modes; if the keyboard does not confirm set 2, translation is
// switched back on and the bytes are decoded as set 1.
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/io.h>
#include <drivers/input.h>
#include <drivers/ioapic.h>
#include <drivers/lapic.h>
#include <drivers/ps2.h>
#include <drivers/ps2kbd.h>
#include <lib/kprintf.h>

namespace {

constexpr u8 IRQ_KEYBOARD = 1;

constexpr usize RING_SIZE = 128;
KeyEvent g_ring[RING_SIZE];
volatile usize g_head = 0, g_tail = 0;

u8 g_mods = mod::NUM;           // Num Lock starts on
bool g_set2 = false;
// Decoder state.
bool g_extended = false;        // after E0
bool g_break = false;           // set 2: after F0
u8 g_skip = 0;                  // bytes of a Pause sequence still to ignore
u8 g_down[64];                  // one bit per keycode (0..511): is the key held?

// Index = set-1 make code. 0 = no character.
const char MAP_LOWER[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   '\\', 0,  0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};
const char MAP_UPPER[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   '|', 0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

// Set 2 make code -> set 1 make code, for ordinary and for extended (E0)
// keys. Built once from pairs; 0 = no such key.
u8 g_set2_plain[0x90];
u8 g_set2_ext[0x90];

struct Pair {
    u8 set2, set1;
};
const Pair PLAIN[] = {
    {0x76, 0x01}, {0x16, 0x02}, {0x1E, 0x03}, {0x26, 0x04}, {0x25, 0x05}, {0x2E, 0x06}, {0x36, 0x07}, {0x3D, 0x08},
    {0x3E, 0x09}, {0x46, 0x0A}, {0x45, 0x0B}, {0x4E, 0x0C}, {0x55, 0x0D}, {0x66, 0x0E}, {0x0D, 0x0F}, {0x15, 0x10},
    {0x1D, 0x11}, {0x24, 0x12}, {0x2D, 0x13}, {0x2C, 0x14}, {0x35, 0x15}, {0x3C, 0x16}, {0x43, 0x17}, {0x44, 0x18},
    {0x4D, 0x19}, {0x54, 0x1A}, {0x5B, 0x1B}, {0x5A, 0x1C}, {0x14, 0x1D}, {0x1C, 0x1E}, {0x1B, 0x1F}, {0x23, 0x20},
    {0x2B, 0x21}, {0x34, 0x22}, {0x33, 0x23}, {0x3B, 0x24}, {0x42, 0x25}, {0x4B, 0x26}, {0x4C, 0x27}, {0x52, 0x28},
    {0x0E, 0x29}, {0x12, 0x2A}, {0x5D, 0x2B}, {0x1A, 0x2C}, {0x22, 0x2D}, {0x21, 0x2E}, {0x2A, 0x2F}, {0x32, 0x30},
    {0x31, 0x31}, {0x3A, 0x32}, {0x41, 0x33}, {0x49, 0x34}, {0x4A, 0x35}, {0x59, 0x36}, {0x7C, 0x37}, {0x11, 0x38},
    {0x29, 0x39}, {0x58, 0x3A}, {0x05, 0x3B}, {0x06, 0x3C}, {0x04, 0x3D}, {0x0C, 0x3E}, {0x03, 0x3F}, {0x0B, 0x40},
    {0x83, 0x41}, {0x0A, 0x42}, {0x01, 0x43}, {0x09, 0x44}, {0x77, 0x45}, {0x7E, 0x46}, {0x6C, 0x47}, {0x75, 0x48},
    {0x7D, 0x49}, {0x7B, 0x4A}, {0x6B, 0x4B}, {0x73, 0x4C}, {0x74, 0x4D}, {0x79, 0x4E}, {0x69, 0x4F}, {0x72, 0x50},
    {0x7A, 0x51}, {0x70, 0x52}, {0x71, 0x53}, {0x61, 0x56}, {0x78, 0x57}, {0x07, 0x58},
};
const Pair EXTENDED[] = {
    {0x14, 0x1D}, {0x11, 0x38}, {0x1F, 0x5B}, {0x27, 0x5C}, {0x2F, 0x5D}, {0x70, 0x52}, {0x6C, 0x47}, {0x7D, 0x49},
    {0x71, 0x53}, {0x69, 0x4F}, {0x7A, 0x51}, {0x75, 0x48}, {0x6B, 0x4B}, {0x72, 0x50}, {0x74, 0x4D}, {0x4A, 0x35},
    {0x5A, 0x1C}, {0x7C, 0x37},
};

void push(const KeyEvent& e) {
    usize next = (g_head + 1) % RING_SIZE;
    if (next == g_tail) return;     // full: drop the key
    g_ring[g_head] = e;
    // Publish the slot only after it is filled: the reader may be on another CPU.
    __atomic_store_n(&g_head, next, __ATOMIC_RELEASE);
}

u8 special_key(u8 code, bool extended) {
    if (extended) {
        switch (code) {
        case 0x48: return key::UP;
        case 0x50: return key::DOWN;
        case 0x4B: return key::LEFT;
        case 0x4D: return key::RIGHT;
        case 0x47: return key::HOME;
        case 0x4F: return key::END;
        case 0x49: return key::PAGE_UP;
        case 0x51: return key::PAGE_DOWN;
        case 0x53: return key::DELETE;
        case 0x52: return key::INSERT;
        case 0x5B: case 0x5C: return key::SUPER;
        case 0x5D: return key::MENU;
        case 0x1D: return key::CTRL;
        case 0x38: return key::ALT;
        case 0x1C: return key::ENTER;
        case 0x37: return key::PRINT;
        default: return key::NONE;
        }
    }
    switch (code) {
    case 0x01: return key::ESCAPE;
    case 0x0F: return key::TAB;
    case 0x1C: return key::ENTER;
    case 0x0E: return key::BACKSPACE;
    case 0x2A: case 0x36: return key::SHIFT;
    case 0x1D: return key::CTRL;
    case 0x38: return key::ALT;
    case 0x3A: return key::CAPS_LOCK;
    case 0x45: return key::NUM_LOCK;
    case 0x46: return key::SCROLL_LOCK;
    default: break;
    }
    if (code >= 0x3B && code <= 0x44) return (u8)(key::F1 + (code - 0x3B));
    if (code == 0x57) return key::F1 + 10;
    if (code == 0x58) return key::F1 + 11;
    return key::NONE;
}

// The keypad without Num Lock is a second navigation cluster.
u8 keypad_navigation(u8 code) {
    switch (code) {
    case 0x47: return key::HOME;
    case 0x48: return key::UP;
    case 0x49: return key::PAGE_UP;
    case 0x4B: return key::LEFT;
    case 0x4D: return key::RIGHT;
    case 0x4F: return key::END;
    case 0x50: return key::DOWN;
    case 0x51: return key::PAGE_DOWN;
    case 0x52: return key::INSERT;
    case 0x53: return key::DELETE;
    default: return key::NONE;
    }
}

// One key going down or up, identified by its set-1 make code.
void handle_key(u8 code, bool extended, bool pressed) {
    u16 keycode = (u16)(code | (extended ? 0x100 : 0));
    bool was_down = g_down[keycode / 8] & (1 << (keycode % 8));
    if (pressed) g_down[keycode / 8] |= (u8)(1 << (keycode % 8));
    else g_down[keycode / 8] &= (u8)~(1 << (keycode % 8));
    bool repeat = pressed && was_down;

    u8 k = special_key(code, extended);
    switch (k) {
    case key::SHIFT: g_mods = pressed ? (g_mods | mod::SHIFT) : (g_mods & ~mod::SHIFT); break;
    case key::CTRL: g_mods = pressed ? (g_mods | mod::CTRL) : (g_mods & ~mod::CTRL); break;
    case key::ALT: g_mods = pressed ? (g_mods | mod::ALT) : (g_mods & ~mod::ALT); break;
    case key::SUPER: g_mods = pressed ? (g_mods | mod::SUPER) : (g_mods & ~mod::SUPER); break;
    case key::CAPS_LOCK: if (pressed && !repeat) g_mods ^= mod::CAPS; break;
    case key::NUM_LOCK: if (pressed && !repeat) g_mods ^= mod::NUM; break;
    default: break;
    }

    KeyEvent e{k, 0, g_mods, pressed, repeat, keycode, 0};
    bool keypad = !extended && code >= 0x47 && code <= 0x53 && code != 0x4A && code != 0x4E;
    if (keypad && !(g_mods & mod::NUM)) {
        e.key = keypad_navigation(code);
    } else if (!extended) {
        bool shift = g_mods & mod::SHIFT;
        char c = shift && !keypad ? MAP_UPPER[code] : MAP_LOWER[code];
        if (c) {
            bool caps = g_mods & mod::CAPS;
            if (caps && !shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            else if (caps && shift && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            e.unicode = (u8)c;
            if ((g_mods & mod::CTRL) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c = (char)(c & 0x1F);
            e.ascii = c;
            if (e.key == key::NONE) e.key = key::CHAR;
        }
    } else if (k == key::ENTER) {
        e.ascii = '\n';
        e.unicode = '\n';
    } else if (code == 0x35) {      // keypad /
        e.ascii = '/';
        e.unicode = '/';
        e.key = key::CHAR;
    }
    input_report(input::DEV_KBD, input::EV_KEY, keycode, pressed ? (repeat ? 2 : 1) : 0, e.unicode, g_mods);
    if (e.key == key::NONE && !e.ascii) return;
    push(e);
}

void handle_set1(u8 sc) {
    if (sc == 0xE0) {
        g_extended = true;
        return;
    }
    if (sc == 0xE1) {               // Pause: E1 1D 45 E1 9D C5, no release
        g_skip = 5;
        return;
    }
    bool extended = g_extended;
    g_extended = false;
    u8 code = sc & 0x7F;
    // E0 2A / E0 36 are "fake shifts" some keyboards wrap around navigation
    // keys and Print Screen: not keys.
    if (extended && (code == 0x2A || code == 0x36)) return;
    handle_key(code, extended, !(sc & 0x80));
}

void handle_set2(u8 sc) {
    if (sc == 0xE0) {
        g_extended = true;
        return;
    }
    if (sc == 0xF0) {
        g_break = true;
        return;
    }
    if (sc == 0xE1) {               // Pause: E1 14 77 E1 F0 14 F0 77
        g_skip = 7;
        return;
    }
    bool extended = g_extended, released = g_break;
    g_extended = g_break = false;
    if (sc >= sizeof g_set2_plain) return;
    if (extended && (sc == 0x12 || sc == 0x59)) return;     // fake shifts, as above
    u8 code = extended ? g_set2_ext[sc] : g_set2_plain[sc];
    if (!code) return;
    handle_key(code, extended, !released);
}

void irq_handler(InterruptFrame*, void*) {
    ps2_drain();
    lapic_eoi();
}

// ---- controller conversation at start-up (interrupts off, polled) ----
bool wait_input_clear() {
    for (u32 i = 0; i < 100000; i++) {
        if (!(inb(PS2_PORT_STATUS) & PS2_STATUS_INPUT_FULL)) return true;
        asm volatile("pause");
    }
    return false;
}

int read_data() {
    for (u32 i = 0; i < 200000; i++) {
        if (inb(PS2_PORT_STATUS) & PS2_STATUS_OUTPUT_FULL) return inb(PS2_PORT_DATA);
        asm volatile("pause");
    }
    return -1;
}

void flush_output() {
    for (int i = 0; i < 64 && (inb(PS2_PORT_STATUS) & PS2_STATUS_OUTPUT_FULL); i++) inb(PS2_PORT_DATA);
}

void controller_cmd(u8 cmd) {
    wait_input_clear();
    outb(PS2_PORT_CMD, cmd);
}

void controller_write_config(u8 cfg) {
    controller_cmd(0x60);
    wait_input_clear();
    outb(PS2_PORT_DATA, cfg);
}

// Sends one byte to the keyboard and waits for its ACK.
bool kbd_send(u8 b) {
    for (int tries = 0; tries < 3; tries++) {
        wait_input_clear();
        outb(PS2_PORT_DATA, b);
        int r = read_data();
        if (r == 0xFA) return true;
        if (r != 0xFE) return false;        // 0xFE = resend
    }
    return false;
}

// Tries to switch to scancode set 2, untranslated. Leaves the controller in
// translated set 1 on any doubt.
bool enter_set2() {
    controller_cmd(0x20);
    int cfg = read_data();
    if (cfg < 0) return false;
    controller_write_config((u8)(cfg & ~0x40));             // translation off
    bool ok = kbd_send(0xF0) && kbd_send(0x02);             // select set 2
    if (ok) {
        // Ask which set is active; a keyboard that ignored us says so here.
        ok = kbd_send(0xF0) && kbd_send(0x00) && read_data() == 0x02;
    }
    if (!ok) controller_write_config((u8)(cfg | 0x40));     // back to translation
    return ok;
}

} // namespace

void ps2kbd_handle_byte(u8 b) {
    if (g_skip) {
        g_skip--;
        return;
    }
    if (g_set2) handle_set2(b);
    else handle_set1(b);
}

void ps2kbd_init() {
    for (const Pair& p : PLAIN) g_set2_plain[p.set2] = p.set1;
    for (const Pair& p : EXTENDED) g_set2_ext[p.set2] = p.set1;
    // The mouse port stays shut during the handshake so none of its bytes
    // can be mistaken for the keyboard's answers; ps2mouse_init reopens it.
    controller_cmd(0xA7);
    flush_output();
    g_set2 = enter_set2();
    // Repeat: 500 ms delay, then about 30 keys per second.
    if (kbd_send(0xF3)) kbd_send(0x20);
    kbd_send(0xF4);                 // scanning on (a failed command may have left it off)
    flush_output();
    interrupt_register(vec::IRQ_BASE + IRQ_KEYBOARD, irq_handler);
    ioapic_unmask_irq(IRQ_KEYBOARD);
    kprintf("ps2kbd: scancode %s, US layout, repeat 500 ms / 30 per second\n", ps2kbd_mode());
}

const char* ps2kbd_mode() { return g_set2 ? "set 2" : "set 1 (translated by the controller)"; }

bool ps2kbd_poll_event(KeyEvent* out) {
    if (__atomic_load_n(&g_head, __ATOMIC_ACQUIRE) == g_tail) return false;
    *out = g_ring[g_tail];
    __atomic_store_n(&g_tail, (g_tail + 1) % RING_SIZE, __ATOMIC_RELEASE);
    return true;
}

int ps2kbd_getc() {
    KeyEvent e;
    while (ps2kbd_poll_event(&e)) {
        if (e.pressed && e.ascii) return (unsigned char)e.ascii;
    }
    return -1;
}
