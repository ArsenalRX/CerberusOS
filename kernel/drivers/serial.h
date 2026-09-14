// COM1 (0x3F8) UART driver, 115200 8N1, polled. This is the kernel log.
// serial_putc is safe from any context: it spins on the transmit-empty bit
// and never sleeps. serial_init must run before the first kprintf.
#pragma once

#include <lib/types.h>

void serial_init();
void serial_putc(char c);
void serial_write(const char* s, usize n);
// Returns -1 when no byte is waiting. Polled; used by the kernel shell.
int serial_getc();
