// Console multiplexer: every byte of kernel output goes to the serial port and
// to whichever screen console is active (the framebuffer text console during
// boot, the desktop's terminal window once the desktop runs). Input is the
// mirror image: serial, keyboard, or the desktop terminal's queue.
#pragma once

#include <lib/types.h>

void console_putc(char c);
void console_write(const char* s, usize n);
void console_puts(const char* s);

// Next input byte from any source, or -1. Pumps the desktop when it is active.
int console_getc();
// Called by the shell when there is nothing to do: presents pending desktop
// frames or flushes the text console, then halts until the next interrupt.
void console_idle(bool spin);

// Switches the screen back to the text console for panic and exception
// dumps; a no-op when the desktop is not running.
void console_emergency_text_mode();
