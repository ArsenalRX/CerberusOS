// Kernel entry point. Phase 0: an empty kernel that halts so the toolchain,
// linker script, and boot pipeline can be exercised end to end.

extern "C" [[noreturn]] void kernel_main() {
    for (;;) {
        asm volatile("cli; hlt");
    }
}
