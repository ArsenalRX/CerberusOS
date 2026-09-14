// Fan-out to the serial driver and the framebuffer console. Both are polled and
// lock-free at this stage; a lock arrives with the scheduler in phase 6.
#include <drivers/fbconsole.h>
#include <drivers/serial.h>
#include <lib/console.h>

void console_putc(char c) {
    serial_putc(c);
    fbconsole_putc(c);
}

void console_write(const char* s, usize n) {
    for (usize i = 0; i < n; i++) console_putc(s[i]);
}

void console_puts(const char* s) {
    while (*s) console_putc(*s++);
}
