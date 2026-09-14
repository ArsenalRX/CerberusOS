// Console multiplexer: every byte of kernel output goes to the serial port and,
// once it exists, the framebuffer text console. kprintf sits on top of this.
#pragma once

#include <lib/types.h>

void console_putc(char c);
void console_write(const char* s, usize n);
void console_puts(const char* s);
