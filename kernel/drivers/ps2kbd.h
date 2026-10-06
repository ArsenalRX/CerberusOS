// PS/2 keyboard (SPEC phase 10): IRQ 1 through the I/O APIC, scancode set 2
// read untranslated (falling back to the controller's set-1 translation if
// the keyboard refuses), the full US key map with the keypad and Num Lock,
// modifier tracking, key repeat marked on the events, and a Unicode value
// per key. Events go to the kernel's own queue (shell, desktop) and to
// /dev/input/kbd0 (drivers/input.h).
#pragma once

#include <lib/types.h>

namespace key {
constexpr u8 NONE = 0;
constexpr u8 ESCAPE = 1, TAB = 2, ENTER = 3, BACKSPACE = 4, DELETE = 5;
constexpr u8 UP = 6, DOWN = 7, LEFT = 8, RIGHT = 9, HOME = 10, END = 11, PAGE_UP = 12, PAGE_DOWN = 13;
constexpr u8 SUPER = 14, ALT = 15, CTRL = 16, SHIFT = 17, CAPS_LOCK = 18, INSERT = 19, PRINT = 20;
constexpr u8 NUM_LOCK = 21, SCROLL_LOCK = 22, MENU = 23, PAUSE = 24;
constexpr u8 F1 = 32;               // F1..F12 = 32..43
constexpr u8 CHAR = 64;             // printable key; see KeyEvent::ascii
} // namespace key

namespace mod {
constexpr u8 SHIFT = 1 << 0, CTRL = 1 << 1, ALT = 1 << 2, SUPER = 1 << 3, CAPS = 1 << 4, NUM = 1 << 5;
}

struct KeyEvent {
    u8 key;         // key:: code
    char ascii;     // 0 if none (modifier-adjusted: shift/caps/ctrl applied)
    u8 mods;        // mod:: bits at the time of the event
    bool pressed;   // false = release
    bool repeat;    // a press repeated by the keyboard while the key is held
    u16 keycode;    // the key's position: its set-1 make code, + 0x100 for extended (E0) keys
    u32 unicode;    // the character typed, before Ctrl is applied; 0 if none
};

// Programs the keyboard (scancode set, repeat rate), registers the IRQ 1
// handler and unmasks the line. Requires ioapic_init. Interrupts off.
void ps2kbd_init();
// Next ASCII byte typed (from press events only), or -1 if none.
int ps2kbd_getc();
// Next key event (press or release); false if the queue is empty.
bool ps2kbd_poll_event(KeyEvent* out);
// "set 2" or "set 1 (translated)".
const char* ps2kbd_mode();
// Layouts (0.0.5h): 0 US, 1 UK, 2 German, 3 French (AZERTY). ASCII only:
// a key whose character has no ASCII form types nothing. AltGr (the right
// Alt) gives the third character of a key where the layout has one.
constexpr int KBD_LAYOUTS = 4;
const char* ps2kbd_layout_name(int layout);
int ps2kbd_layout();
void ps2kbd_set_layout(int layout);
// Typematic repeat: delay 0..3 (250/500/750/1000 ms), rate 0..2
// (10/20/30 per second). Talks to the keyboard; thread context.
void ps2kbd_set_repeat(int delay, int rate);
int ps2kbd_repeat_delay();
int ps2kbd_repeat_rate();
// The lock and modifier bits at this moment (mod::*).
u8 ps2kbd_mods();
// Sends the Caps/Num/Scroll Lock lights to the keyboard if a lock key
// changed since the last call. Thread context (the compositor calls it).
void ps2kbd_update_leds();
