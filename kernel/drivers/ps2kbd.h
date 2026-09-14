// PS/2 keyboard, early version: IRQ 1 through the I/O APIC, scancode set 1
// (the controller's translation mode), US layout, shift/caps/ctrl tracking,
// ASCII output into a small ring buffer for the kernel shell. Phase 10
// replaces this with the full driver (set 2, key events, repeat, /dev/input).
#pragma once

#include <lib/types.h>

// Registers the IRQ 1 handler and unmasks the line. Requires ioapic_init.
void ps2kbd_init();
// Next ASCII byte typed, or -1 if none. Safe to poll with interrupts enabled.
int ps2kbd_getc();
