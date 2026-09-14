// Freestanding memory and C-string primitives. GCC may emit calls to memcpy,
// memset, memmove, and memcmp on its own even with -fno-builtin, so these must
// exist with C linkage.
#pragma once

#include <lib/types.h>

extern "C" {
void* memcpy(void* dst, const void* src, usize n);
void* memmove(void* dst, const void* src, usize n);
void* memset(void* dst, int c, usize n);
int memcmp(const void* a, const void* b, usize n);
usize strlen(const char* s);
usize strnlen(const char* s, usize max);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, usize n);
char* strncpy(char* dst, const char* src, usize n);
}

// Copies at most n-1 characters and always NUL-terminates. Returns strlen(src).
usize strlcpy(char* dst, const char* src, usize n);
