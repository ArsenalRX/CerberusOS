// Deliberate misbehaviour, one kind per argument. Each must end this process
// only; the kernel reports it and carries on.
#include <lumen.h>

__attribute__((noinline)) static void smash(const char* src) {
    char small[8];
    // Deliberately unbounded copy: overruns `small` and hits the stack guard.
    for (size_t i = 0; src[i]; i++) ((volatile char*)small)[i] = src[i];
}

static unsigned char g_data[16] = {0xC3};       // a `ret` instruction, in a data page

int main(int argc, char** argv, char**) {
    const char* what = argc > 1 ? argv[1] : "";
    printf("crash: %s\n", what);
    if (!strcmp(what, "null")) {
        *(volatile int*)nullptr = 1;                            // unmapped: the first 64 KiB never map
    } else if (!strcmp(what, "kernel")) {
        volatile unsigned long v = *(volatile unsigned long*)0xFFFFFFFF80001000ul;   // kernel memory
        (void)v;
    } else if (!strcmp(what, "priv")) {
        asm volatile("hlt");                                     // privileged instruction
    } else if (!strcmp(what, "exec")) {
        ((void (*)())g_data)();                                  // data is not executable
    } else if (!strcmp(what, "smash")) {
        smash("this string is much longer than eight bytes");
    } else if (!strcmp(what, "divide")) {
        // A real divide instruction: the compiler turns `1 / x` into a
        // comparison, which never faults.
        asm volatile("xorl %%ecx, %%ecx; movl $1, %%eax; xorl %%edx, %%edx; divl %%ecx" ::: "eax", "ecx", "edx");
    } else {
        printf("usage: crash null|kernel|priv|exec|smash|divide\n");
        return 2;
    }
    printf("crash: FAIL: still running after %s\n", what);
    return 1;
}
