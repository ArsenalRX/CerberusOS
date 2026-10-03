// Shared 8042 controller access for the keyboard and mouse drivers. Both
// IRQ 1 and IRQ 12 handlers drain the controller's output buffer through
// ps2_drain(), which routes every byte to the right decoder by the status
// register's "from auxiliary device" bit, so neither driver can swallow the
// other's data regardless of which interrupt fires first.
#pragma once

#include <lib/types.h>

constexpr u16 PS2_PORT_DATA = 0x60;
constexpr u16 PS2_PORT_STATUS = 0x64;
constexpr u16 PS2_PORT_CMD = 0x64;
constexpr u8 PS2_STATUS_OUTPUT_FULL = 1 << 0;
constexpr u8 PS2_STATUS_INPUT_FULL = 1 << 1;
constexpr u8 PS2_STATUS_FROM_AUX = 1 << 5;

// Reads every pending byte and dispatches it. Safe from interrupt context.
void ps2_drain();

// Decoders implemented by the two drivers.
void ps2kbd_handle_byte(u8 b);
void ps2mouse_handle_byte(u8 b);
