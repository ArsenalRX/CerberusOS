// Program start-up: everything between the kernel's hand-off and main().
//
// The program is position-independent and there is no dynamic linker, so the
// first job is to apply the image's own relative relocations (pointers stored
// in data, such as tables of strings). Until that is done this code must not
// touch any such pointer; it uses only its arguments and addresses the
// compiler computes relative to the instruction pointer.
#include <cerberus.h>

extern "C" {

int main(int argc, char** argv, char** envp);

char** environ;
int errno;

// Stack-protector guard (the compiler's -mstack-protector-guard=global).
// Set from the 16 random bytes the kernel supplies before any protected
// function is entered.
uintptr_t __stack_chk_guard = 0x1F2E3D4C5B6A7988ull;

__attribute__((noreturn)) void __stack_chk_fail(void) {
    static const char msg[] = "*** stack smashing detected: the program overran a local buffer ***\n";
    write(2, msg, sizeof msg - 1);
    abort();
}

struct Rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};
extern const Rela __rela_start[] __attribute__((visibility("hidden")));
extern const Rela __rela_end[] __attribute__((visibility("hidden")));

constexpr uint64_t AT_NULL = 0, AT_BASE = 7, AT_RANDOM = 25;
constexpr uint64_t R_X86_64_RELATIVE = 8;

// `sp` points at: argc, argv[0..argc), NULL, envp..., NULL, auxv pairs.
__attribute__((noreturn, no_stack_protector)) void __libc_start(uint64_t* sp) {
    int argc = (int)sp[0];
    char** argv = (char**)(sp + 1);
    char** envp = argv + argc + 1;
    uint64_t* aux = (uint64_t*)envp;
    while (*aux) aux++;
    aux++;

    uint64_t base = 0;
    const uint8_t* random = nullptr;
    for (; aux[0] != AT_NULL; aux += 2) {
        if (aux[0] == AT_BASE) base = aux[1];
        else if (aux[0] == AT_RANDOM) random = (const uint8_t*)aux[1];
    }

    for (const Rela* r = __rela_start; r < __rela_end; r++) {
        if ((r->info & 0xFFFFFFFF) != R_X86_64_RELATIVE) continue;     // the only kind a static PIE needs
        *(uint64_t*)(base + r->offset) = base + (uint64_t)r->addend;
    }

    if (random) {
        uint64_t guard = 0;
        for (int i = 0; i < 8; i++) guard |= (uint64_t)random[i] << (i * 8);
        __stack_chk_guard = guard & ~0xFFull;       // a zero byte stops string overruns copying it
    }
    environ = envp;
    exit(main(argc, argv, envp));
}

} // extern "C"
