// Text console on the Limine framebuffer using an embedded PSF2 bitmap font.
// Provides scrolling, a foreground/background colour, and a block cursor.
// fbconsole_putc is a no-op until fbconsole_init succeeds, so kprintf may be
// used before the framebuffer is known. Not interrupt-safe (no lock yet).
#pragma once

#include <lib/types.h>

struct FramebufferInfo;

// Returns false (and prints why over serial) if the framebuffer is unusable,
// e.g. not 32 bits per pixel. The spec requires failing loudly rather than
// drawing garbage.
bool fbconsole_init(const FramebufferInfo& fb);
bool fbconsole_ready();
void fbconsole_putc(char c);
// Paints everything written since the last flush. Output is batched (at most
// one repaint per ~20 ms during bursts); call this before blocking or halting.
void fbconsole_flush();
void fbconsole_set_colour(u32 fg, u32 bg);
void fbconsole_clear();
u32 fbconsole_columns();
u32 fbconsole_rows();
