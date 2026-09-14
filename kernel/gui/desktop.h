// The desktop: compositor, window management, panel, launcher, cursor. Runs
// inside the kernel until phase 12 turns it into the Pane server process;
// the design follows SPEC §9 "Compositor internals" so that move is a
// transport change, not a rewrite. Single-threaded: gui_pump() must be called
// regularly (the shell's idle loop does) to process input and present frames.
#pragma once

#include <lib/types.h>

// Takes over the framebuffer, opens the initial windows. Requires pmm_init,
// the framebuffer, keyboard/mouse drivers, refclock, rtc.
bool gui_init();
bool gui_active();
// Processes pending input, repaints the terminal if the shell wrote to it,
// and presents a frame if anything changed (paced at ~60 Hz).
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
    u64 last_present_pixels;
    u32 windows;
};
GuiStats gui_stats();
