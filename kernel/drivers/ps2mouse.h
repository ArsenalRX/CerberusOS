// PS/2 mouse, early version: IRQ 12 through the I/O APIC, streaming mode,
// 3 buttons plus scroll wheel (IntelliMouse extension when the device
// supports it). Movement deltas are queued as events for the compositor.
// Phase 10 replaces this with the full driver behind /dev/input.
#pragma once

#include <lib/types.h>

struct MouseEvent {
    i16 dx, dy;         // relative motion, y positive = down
    i8 dz;              // wheel notches, positive = up
    u8 buttons;         // bit 0 left, 1 right, 2 middle
};

// Enables the auxiliary device and IRQ 12. Requires ioapic_init. Returns
// false if no mouse answered (the desktop still works, keyboard only).
bool ps2mouse_init();
// Pops the next event; false if the queue is empty. Safe to poll.
bool ps2mouse_poll(MouseEvent* out);
bool ps2mouse_has_wheel();
