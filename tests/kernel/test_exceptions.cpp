// Deliberately raises CPU exceptions so the dump path can be inspected. Each
// fault goes through two extra call frames so the backtrace has something to
// show, and every fault function makes a call before faulting so its own frame
// is established (an RBP walk cannot name a function that faults inside its
// prologue). The kernel halts afterwards by design (phase 2 acceptance).
#include <kernel/ktest.h>
#include <lib/kprintf.h>
#include <lib/string.h>

namespace {

__attribute__((noinline)) void fault_de() {
    kprintf("  dividing by zero\n");
    asm volatile("xor %%ecx, %%ecx\n"
                 "mov $1, %%eax\n"
                 "xor %%edx, %%edx\n"
                 "div %%ecx\n" ::: "eax", "ecx", "edx", "memory");
}

__attribute__((noinline)) void fault_ud() {
    kprintf("  executing ud2\n");
    asm volatile("ud2");
}

__attribute__((noinline)) void fault_pf_read() {
    volatile u64* p = (volatile u64*)0x0000000DEADBEE000ull;
    kprintf("  reading %p\n", (void*)p);
    u64 v = *p;
    kprintf("unexpectedly read %lx\n", (unsigned long)v);
}

__attribute__((noinline)) void fault_pf_write() {
    volatile u64* p = (volatile u64*)0x0000000DEADBEE000ull;
    kprintf("  writing %p\n", (void*)p);
    *p = 42;
}

__attribute__((noinline)) void fault_gp() {
    // Non-canonical address: #GP rather than #PF.
    volatile u64* p = (volatile u64*)0x8000DEADBEEF0000ull;
    kprintf("  reading non-canonical %p\n", (void*)p);
    u64 v = *p;
    kprintf("unexpectedly read %lx\n", (unsigned long)v);
}

__attribute__((noinline)) void fault_bp() {
    kprintf("  executing int3\n");
    asm volatile("int3");
}

__attribute__((noinline)) void trampoline(void (*fn)()) {
    kprintf("  raising...\n");
    fn();
    kprintf("  returned from the fault function?!\n");
}

} // namespace

int ktest_exceptions(int argc, char** argv) {
    if (argc < 2) {
        kprintf("usage: test exceptions <de|ud|pf|pfw|gp|bp>\n");
        return 1;
    }
    const char* which = argv[1];
    void (*fn)() = nullptr;
    if (strcmp(which, "de") == 0) fn = fault_de;
    else if (strcmp(which, "ud") == 0) fn = fault_ud;
    else if (strcmp(which, "pf") == 0) fn = fault_pf_read;
    else if (strcmp(which, "pfw") == 0) fn = fault_pf_write;
    else if (strcmp(which, "gp") == 0) fn = fault_gp;
    else if (strcmp(which, "bp") == 0) fn = fault_bp;
    if (!fn) {
        kprintf("unknown exception '%s'\n", which);
        return 1;
    }
    kprintf("test exceptions: triggering %s (the kernel will halt)\n", which);
    trampoline(fn);
    kprintf("test exceptions: FAIL, execution continued past the fault\n");
    return 1;
}
