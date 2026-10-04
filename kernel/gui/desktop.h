// The desktop: compositor, window management, panel, launcher, cursor. Runs
// inside the kernel until phase 12 turns it into the Pane server process;
// the design follows SPEC §9 "Compositor internals" so that move is a
// transport change, not a rewrite. All desktop state belongs to one thread,
// the compositor; other threads only reach it through the terminal's putc
// and getc below, which are safe to call from any thread.
#pragma once

#include <lib/types.h>

// Takes over the framebuffer, opens the initial windows. Requires the heap,
// the framebuffer, keyboard/mouse drivers, refclock, rtc.
bool gui_init();
bool gui_active();
// Starts the compositor thread (INTERACTIVE priority), which calls gui_pump
// whenever input arrives and at least once per timer tick. Requires
// gui_init and a running scheduler. Returns false if out of memory.
bool gui_start_compositor();
// One compositor pass: processes pending input, repaints the terminal if the
// shell wrote to it, and presents a frame if anything changed (paced at
// ~60 Hz). Only the compositor thread calls it.
void gui_pump();
// Console sink: characters written by kprintf while the desktop is active.
void gui_terminal_putc(char c);
// Keyboard input that reached the terminal window; -1 if none.
int gui_terminal_getc();

// Abandons the desktop and hands the framebuffer back to the text console,
// so a panic or exception dump is visible. Not reversible.
void gui_emergency_text_mode();

struct GuiStats {
    u64 frames;
    u64 last_frame_us;
    u64 last_present_pixels;    // pixels of the frame's damaged area
    u64 last_written_pixels;    // of those, pixels that changed and were written to the screen
    u64 worst_frame_us;         // slowest frame since the last gui_reset_worst()
    u64 worst_written_pixels;   // pixels written in that frame
    u32 windows;
};
GuiStats gui_stats();
void gui_reset_worst();
