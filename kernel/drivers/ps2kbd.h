// PS/2 keyboard, early version: IRQ 1 through the I/O APIC, scancode set 1
// (the controller's translation mode), US layout, modifier tracking, key
// events with an ASCII translation queued for the shell and the desktop.
// Phase 10 replaces this with the full driver (set 2, repeat, /dev/input).
#pragma once

#include <lib/types.h>

namespace key {
constexpr u8 NONE = 0;
constexpr u8 ESCAPE = 1, TAB = 2, ENTER = 3, BACKSPACE = 4, DELETE = 5;
constexpr u8 UP = 6, DOWN = 7, LEFT = 8, RIGHT = 9, HOME = 10, END = 11, PAGE_UP = 12, PAGE_DOWN = 13;
constexpr u8 SUPER = 14, ALT = 15, CTRL = 16, SHIFT = 17, CAPS_LOCK = 18, INSERT = 19, PRINT = 20;
constexpr u8 F1 = 32;               // F1..F12 = 32..43
constexpr u8 CHAR = 64;             // printable key; see KeyEvent::ascii
} // namespace key

namespace mod {
constexpr u8 SHIFT = 1 << 0, CTRL = 1 << 1, ALT = 1 << 2, SUPER = 1 << 3, CAPS = 1 << 4;
}

struct KeyEvent {
    u8 key;         // key:: code
    char ascii;     // 0 if none (modifier-adjusted: shift/caps/ctrl applied)
    u8 mods;        // mod:: bits at the time of the event
    bool pressed;   // false = release
};

// Registers the IRQ 1 handler and unmasks the line. Requires ioapic_init.
void ps2kbd_init();
// Next ASCII byte typed (from press events only), or -1 if none.
int ps2kbd_getc();
// Next key event (press or release); false if the queue is empty.
bool ps2kbd_poll_event(KeyEvent* out);
