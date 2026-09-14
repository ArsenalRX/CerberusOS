// Fatal error handling. PANIC prints the message and location and halts. The
// register dump and backtrace are added with the exception machinery in
// phase 2. ASSERT is active in debug builds only; ASSERT_ALWAYS is unconditional.
#pragma once

#include <lib/types.h>

[[noreturn]] void panic_impl(const char* file, int line, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

[[noreturn]] void halt_forever();

#define PANIC(...) panic_impl(__FILE__, __LINE__, __VA_ARGS__)

#define ASSERT_ALWAYS(cond)                                                                  \
    do {                                                                                     \
        if (!(cond)) PANIC("assertion failed: %s", #cond);                                   \
    } while (0)

#ifdef LUMEN_DEBUG
#define ASSERT(cond) ASSERT_ALWAYS(cond)
#else
#define ASSERT(cond)                                                                         \
    do {                                                                                     \
    } while (0)
#endif
