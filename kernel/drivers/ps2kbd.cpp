// Scancode set 1 decode with modifier state. Extended (E0) codes cover the
// navigation cluster and the Super keys.
#include <arch/x86_64/interrupts.h>
#include <arch/x86_64/io.h>
#include <drivers/ioapic.h>
#include <drivers/lapic.h>
#include <drivers/ps2.h>
#include <drivers/ps2kbd.h>

namespace {

constexpr u16 PORT_DATA = 0x60;
constexpr u16 PORT_STATUS = 0x64;
constexpr u8 IRQ_KEYBOARD = 1;

constexpr usize RING_SIZE = 128;
KeyEvent g_ring[RING_SIZE];
volatile usize g_head = 0, g_tail = 0;

u8 g_mods = 0;
bool g_extended = false;

// Index = scancode (set 1, make codes). 0 = no character.
const char MAP_LOWER[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};
const char MAP_UPPER[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

void push(const KeyEvent& e) {
    usize next = (g_head + 1) % RING_SIZE;
    if (next == g_tail) return;     // full: drop the key
    g_ring[g_head] = e;
    g_head = next;
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
        case 0x1D: return key::CTRL;
        case 0x38: return key::ALT;
        case 0x1C: return key::ENTER;
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
    case 0x37: return key::PRINT;
    default: break;
    }
    if (code >= 0x3B && code <= 0x44) return (u8)(key::F1 + (code - 0x3B));
    if (code == 0x57) return key::F1 + 10;
    if (code == 0x58) return key::F1 + 11;
    return key::NONE;
}

void handle_scancode(u8 sc) {
    if (sc == 0xE0) {
        g_extended = true;
        return;
    }
    bool pressed = !(sc & 0x80);
    u8 code = sc & 0x7F;
    bool extended = g_extended;
    g_extended = false;

    u8 k = special_key(code, extended);
    switch (k) {
    case key::SHIFT: g_mods = pressed ? (g_mods | mod::SHIFT) : (g_mods & ~mod::SHIFT); break;
    case key::CTRL: g_mods = pressed ? (g_mods | mod::CTRL) : (g_mods & ~mod::CTRL); break;
    case key::ALT: g_mods = pressed ? (g_mods | mod::ALT) : (g_mods & ~mod::ALT); break;
    case key::SUPER: g_mods = pressed ? (g_mods | mod::SUPER) : (g_mods & ~mod::SUPER); break;
    case key::CAPS_LOCK: if (pressed) g_mods ^= mod::CAPS; break;
    default: break;
    }

    KeyEvent e{k, 0, g_mods, pressed};
    if (!extended) {
        bool shift = g_mods & mod::SHIFT;
        char c = shift ? MAP_UPPER[code] : MAP_LOWER[code];
        if (c) {
            bool caps = g_mods & mod::CAPS;
            if (caps && !shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            else if (caps && shift && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if ((g_mods & mod::CTRL) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c = (char)(c & 0x1F);
            e.ascii = c;
            if (e.key == key::NONE) e.key = key::CHAR;
        }
    } else if (k == key::ENTER) {
        e.ascii = '\n';
    }
    if (e.key == key::NONE && !e.ascii) return;
    push(e);
}

void irq_handler(InterruptFrame*, void*) {
    ps2_drain();
    lapic_eoi();
}

} // namespace

void ps2kbd_handle_byte(u8 b) { handle_scancode(b); }

void ps2kbd_init() {
    // Drain anything the firmware left in the output buffer.
    while (inb(PORT_STATUS) & 1) inb(PORT_DATA);
    interrupt_register(vec::IRQ_BASE + IRQ_KEYBOARD, irq_handler);
    ioapic_unmask_irq(IRQ_KEYBOARD);
}

bool ps2kbd_poll_event(KeyEvent* out) {
    if (g_head == g_tail) return false;
    *out = g_ring[g_tail];
    g_tail = (g_tail + 1) % RING_SIZE;
    return true;
}

int ps2kbd_getc() {
    KeyEvent e;
    while (ps2kbd_poll_event(&e)) {
        if (e.pressed && e.ascii) return (unsigned char)e.ascii;
    }
    return -1;
}
