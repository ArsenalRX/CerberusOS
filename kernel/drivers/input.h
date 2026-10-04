// The input event layer (SPEC phase 10): the keyboard and mouse drivers
// report events here, and programs read them from /dev/input/kbd0 and
// /dev/input/mouse0 as a stream of fixed-size records. The device nodes are
// mode 0600: only the window server's credential may open them, so no other
// program can read keystrokes (SPEC §5A).
//
// The kernel-hosted desktop still takes its input straight from the drivers'
// own queues; the device files are the interface the userland window server
// (phase 12) will use, and a second reader of every event until then.
#pragma once

#include <lib/types.h>

namespace input {

// Event types. The record layout is ABI (userland/libc/include/cerberus.h).
constexpr u16 EV_KEY = 1;       // code: key code; value: 1 press, 0 release, 2 repeat
constexpr u16 EV_REL = 2;       // code: REL_X, REL_Y, REL_WHEEL; value: movement
constexpr u16 EV_BUTTON = 3;    // code: 0 left, 1 right, 2 middle; value: 1 press, 0 release

constexpr u16 REL_X = 0, REL_Y = 1, REL_WHEEL = 2;

struct Event {
    u64 time_us;                // since boot
    u16 type;
    u16 code;
    i32 value;
    u32 unicode;                // EV_KEY: the character typed, 0 if none
    u16 mods;                   // EV_KEY: modifier bits (mod:: in ps2kbd.h)
    u16 reserved;
};
static_assert(sizeof(Event) == 24, "input event layout is ABI");

constexpr u32 DEV_KBD = 0, DEV_MOUSE = 1;

} // namespace input

// Registers the character devices behind /dev/input/*.
void input_init();
// Queues an event for readers of device `dev`. Interrupt context.
void input_report(u32 dev, u16 type, u16 code, i32 value, u32 unicode = 0, u16 mods = 0);
