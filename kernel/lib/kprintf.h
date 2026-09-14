// Kernel formatted output. kprintf writes to every registered console (serial
// and framebuffer); ksnprintf formats into a buffer. Neither allocates.
//
// Supported: %d %i %u %x %X %o %b %p %s %c %%  with flags '-' '0' '#' '+' ' ',
// a width (number or '*'), a precision for %s, and the length modifiers
// l, ll, z, h, hh.
// Safe to call from any context including early boot and interrupt handlers
// (serial is polling; there is no lock yet, that arrives with the scheduler).
#pragma once

#include <lib/types.h>

using kprintf_sink = void (*)(char c, void* ctx);

// Core engine: formats into `sink`, returns the number of characters produced.
int kvformat(kprintf_sink sink, void* ctx, const char* fmt, va_list ap);

int kprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int kvprintf(const char* fmt, va_list ap);
int ksnprintf(char* buf, usize n, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
int kvsnprintf(char* buf, usize n, const char* fmt, va_list ap);
