// SHA-256 against the FIPS 180-4 test vectors, including a message that
// crosses a block boundary and a streamed update.
#include <kernel/ktest.h>
#include <lib/kprintf.h>
#include <lib/sha256.h>
#include <lib/string.h>

namespace {

int check(const char* name, const char* msg, const char* hex_expected, bool streamed) {
    u8 out[32];
    if (streamed) {
        Sha256 s;
        s.init();
        usize n = strlen(msg);
        for (usize i = 0; i < n; i += 7) s.update(msg + i, n - i < 7 ? n - i : 7);
        s.final(out);
    } else {
        sha256(msg, strlen(msg), out);
    }
    char hex[65];
    for (int i = 0; i < 32; i++) ksnprintf(hex + i * 2, 3, "%02x", out[i]);
    if (strcmp(hex, hex_expected) != 0) {
        kprintf("  %s: got %s\n", name, hex);
        return 1;
    }
    return 0;
}

} // namespace

int ktest_sha256(int, char**) {
    int fails = 0;
    fails += check("empty", "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", false);
    fails += check("abc", "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", false);
    fails += check("448 bits", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                   "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", false);
    fails += check("streamed", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                   "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", true);
    kprintf("sha256: %d vectors, %d failed\n", 4, fails);
    return fails ? 1 : 0;
}
