// Fatal error handling. PANIC prints the message and location and halts. The
// register dump and backtrace are added with the exception machinery in
// phase 2. ASSERT is active in debug builds only; ASSERT_ALWAYS is unconditional.
#pragma once

#include <lib/types.h>

[[noreturn]] void panic_impl(const char* file, int line, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

[[noreturn]] void halt_forever();

// First step of every fatal report: interrupts off, the other CPUs stopped.
// If another CPU is already reporting, this one just stops. Afterwards
// panic_in_progress() is true and spinlocks no longer block (their holders
// may have been stopped mid-section).
void panic_begin();
bool panic_in_progress();

#define PANIC(...) panic_impl(__FILE__, __LINE__, __VA_ARGS__)

#define ASSERT_ALWAYS(cond)                                                                  \
    do {                                                                                     \
        if (!(cond)) PANIC("assertion failed: %s", #cond);                                   \
    } while (0)

#ifdef CERBERUS_DEBUG
#define ASSERT(cond) ASSERT_ALWAYS(cond)
#else
#define ASSERT(cond)                                                                         \
    do {                                                                                     \
    } while (0)
#endif
