// SHA-256 (FIPS 180-4), for the lock-screen password until phase 15 brings
// Argon2id. Freestanding; no allocation.
#pragma once

#include <lib/types.h>

struct Sha256 {
    u32 h[8];
    u8 block[64];
    u64 total;
    u32 fill;
    void init();
    void update(const void* data, usize n);
    void final(u8 out[32]);
};

void sha256(const void* data, usize n, u8 out[32]);
