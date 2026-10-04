// Console multiplexer: every byte of kernel output goes to the serial port and
// to whichever screen console is active (the framebuffer text console during
// boot, the desktop's terminal window once the desktop runs). Input is the
// mirror image: serial, keyboard, or the desktop terminal's queue.
#pragma once

#include <lib/types.h>

// The console lock (rank CONSOLE, the last in lib/lock_order.h) covers the
// serial port, the framebuffer console and the terminal's cell grid. kprintf
// takes it for one whole call; the functions below do not take it, so
// callers that use them directly hold it around their output. Interrupts are
// off while it is held.
void console_lock();
void console_unlock();

void console_putc(char c);
void console_write(const char* s, usize n);
void console_puts(const char* s);

// Next input byte from any source, or -1. Never blocks.
int console_getc();
// Called by the shell when there is no input: flushes the text console and
// sleeps until the next timer tick (thread context once the scheduler runs).
void console_idle();

// Switches the screen back to the text console for panic and exception
// dumps; a no-op when the desktop is not running.
void console_emergency_text_mode();
