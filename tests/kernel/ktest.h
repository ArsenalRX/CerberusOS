// Registry of in-kernel self-tests, run from the kernel shell with
// `test <name>`. Each test returns 0 on success. Tests that deliberately
// crash the kernel set `halts` so `test all` skips them.
#pragma once

#include <lib/types.h>

struct KernelTest {
    const char* name;
    const char* help;
    int (*fn)(int argc, char** argv);
    bool halts;
    bool slow = false;          // long-running (fuzzers): run by name only, like halts
};

const KernelTest* ktests();
usize ktest_count();

// Helpers for test bodies.
#define KTEST_CHECK(cond)                                                                    \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            kprintf("  check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                        \
        }                                                                                    \
    } while (0)
