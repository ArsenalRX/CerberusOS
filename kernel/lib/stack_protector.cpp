// Stack-smashing protection for the kernel (-fstack-protector-strong with a
// global guard, SPEC §19.3). Every function with a local array or an
// address-taken local keeps a copy of __stack_chk_guard between its locals
// and its return address and checks it before returning; an overflow that
// reaches the return address changes the copy first.
//
// The guard is set from the CSPRNG as the very first thing kernel_main does.
// It must not change afterwards: a function that was entered under the old
// value would fail its check on return. kernel_main itself never returns.
#include <lib/panic.h>
#include <lib/types.h>

extern "C" {

u64 __stack_chk_guard = 0x595E9FBD94FDA766ull;     // replaced at boot; never used as is

[[noreturn]] void __stack_chk_fail() {
    PANIC("stack smashing detected: a kernel function overran a local buffer");
}

} // extern "C"
