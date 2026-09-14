// Exercises the kprintf format engine through ksnprintf and compares against
// expected strings.
#include <kernel/ktest.h>
#include <lib/kprintf.h>
#include <lib/string.h>

namespace {

int check(const char* expect, const char* fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (strcmp(buf, expect) != 0 || n != (int)strlen(expect)) {
        kprintf("  mismatch for '%s': got '%s' (%d), want '%s'\n", fmt, buf, n, expect);
        return 1;
    }
    return 0;
}

} // namespace

int ktest_kprintf(int, char**) {
    int fails = 0;
    fails += check("42", "%d", 42);
    fails += check("-42", "%d", -42);
    fails += check("   42", "%5d", 42);
    fails += check("42   |", "%-5d|", 42);
    fails += check("00042", "%05d", 42);
    fails += check("-0042", "%05d", -42);
    fails += check("ff", "%x", 255);
    fails += check("FF", "%X", 255);
    fails += check("0xff", "%#x", 255);
    fails += check("00ff", "%04x", 255);
    fails += check("4294967295", "%u", 4294967295u);
    fails += check("18446744073709551615", "%lu", 18446744073709551615ul);
    fails += check("-9223372036854775808", "%ld", (long)(-9223372036854775807l - 1));
    fails += check("123456789abcdef0", "%llx", 0x123456789abcdef0ull);
    fails += check("12", "%zu", (usize)12);
    fails += check("0x0000000000001000", "%p", (void*)0x1000);
    fails += check("hi", "%s", "hi");
    fails += check("(null)", "%s", (const char*)nullptr);
    fails += check("  hi", "%4s", "hi");
    fails += check("hi  |", "%-4s|", "hi");
    fails += check("hel", "%.3s", "hello");
    fails += check("x", "%c", 'x');
    fails += check("100%", "100%%");
    fails += check("1010", "%b", 10);
    fails += check("17", "%o", 15);
    fails += check("  7", "%*d", 3, 7);
    fails += check("+5", "%+d", 5);
    // Truncation still reports the full length and terminates.
    char small[4];
    int n = ksnprintf(small, sizeof small, "%s", "toolong");
    if (n != 7 || strcmp(small, "too") != 0) {
        kprintf("  truncation wrong: n=%d buf='%s'\n", n, small);
        fails++;
    }
    kprintf("  %d checks failed\n", fails);
    return fails ? 1 : 0;
}
