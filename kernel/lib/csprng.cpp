// See csprng.h. ChaCha20 as specified in RFC 8439; the generator around it
// follows the "fast key erasure" construction.
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/cpuid.h>
#include <lib/csprng.h>
#include <lib/kprintf.h>
#include <lib/string.h>

namespace {

u32 g_key[8];
u64 g_counter = 0;              // block counter, also varies the nonce
u64 g_pool = 0;                 // timing entropy collected since the last reseed
u32 g_pool_events = 0;
u64 g_blocks_since_reseed = 0;
bool g_have_rdrand = false, g_have_rdseed = false;

constexpr u32 RESEED_EVENTS = 256;          // fold the pool in after this many interrupts
constexpr u64 RESEED_BLOCKS = 1 << 16;      // and at least this often by output volume

inline u32 rotl(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

inline void quarter(u32& a, u32& b, u32& c, u32& d) {
    a += b; d ^= a; d = rotl(d, 16);
    c += d; b ^= c; b = rotl(b, 12);
    a += b; d ^= a; d = rotl(d, 8);
    c += d; b ^= c; b = rotl(b, 7);
}

bool hw_random(bool seed, u64* out) {
    for (int tries = 0; tries < 16; tries++) {
        u64 v = 0;
        u8 ok = 0;
        if (seed) asm volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
        else asm volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) {
            *out = v;
            return true;
        }
        cpu_relax();
    }
    return false;
}

// Mixes 64 bits into the key: XOR into two key words, then run the block
// function over the result and take a fresh key from its output, so the
// input is diffused through the whole key.
void mix(u64 v) {
    static u32 slot = 0;
    g_key[slot % 8] ^= (u32)v;
    g_key[(slot + 3) % 8] ^= (u32)(v >> 32);
    slot++;
    u32 nonce[3] = {0x6d6978, (u32)g_counter, (u32)(g_counter >> 32)};      // "mix"
    u8 block[64];
    chacha20_block(g_key, 0xFFFFFFFF, nonce, block);
    memcpy(g_key, block, sizeof g_key);
    g_counter++;
    volatile u8* wipe = block;
    for (usize i = 0; i < sizeof block; i++) wipe[i] = 0;
}

void reseed() {
    u64 v;
    if (g_have_rdseed && hw_random(true, &v)) mix(v);
    if (g_have_rdrand && hw_random(false, &v)) mix(v);
    mix(g_pool ^ rdtsc());
    g_pool = 0;
    g_pool_events = 0;
    g_blocks_since_reseed = 0;
}

// One block of output: 32 bytes replace the key, 32 bytes go to the caller.
void next_block(u8 out[32]) {
    if (g_blocks_since_reseed++ >= RESEED_BLOCKS || g_pool_events >= RESEED_EVENTS) reseed();
    u32 nonce[3] = {0, (u32)g_counter, (u32)(g_counter >> 32)};
    u8 block[64];
    chacha20_block(g_key, 0, nonce, block);
    g_counter++;
    memcpy(g_key, block, 32);
    memcpy(out, block + 32, 32);
    volatile u8* wipe = block;
    for (usize i = 0; i < sizeof block; i++) wipe[i] = 0;
}

} // namespace

void chacha20_block(const u32 key[8], u32 counter, const u32 nonce[3], u8 out[64]) {
    u32 s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                 key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                 counter, nonce[0], nonce[1], nonce[2]};
    u32 w[16];
    memcpy(w, s, sizeof w);
    for (int round = 0; round < 10; round++) {
        quarter(w[0], w[4], w[8], w[12]);
        quarter(w[1], w[5], w[9], w[13]);
        quarter(w[2], w[6], w[10], w[14]);
        quarter(w[3], w[7], w[11], w[15]);
        quarter(w[0], w[5], w[10], w[15]);
        quarter(w[1], w[6], w[11], w[12]);
        quarter(w[2], w[7], w[8], w[13]);
        quarter(w[3], w[4], w[9], w[14]);
    }
    for (int i = 0; i < 16; i++) {
        u32 v = w[i] + s[i];
        out[i * 4 + 0] = (u8)v;
        out[i * 4 + 1] = (u8)(v >> 8);
        out[i * 4 + 2] = (u8)(v >> 16);
        out[i * 4 + 3] = (u8)(v >> 24);
    }
}

void csprng_init() {
    CpuidRegs l1 = cpuid(1);
    g_have_rdrand = l1.ecx & (1u << 30);
    g_have_rdseed = cpuid_max_leaf() >= 7 && (cpuid(7, 0).ebx & (1u << 18));
    // Start from something that differs per boot even with no hardware RNG,
    // then pour in everything available. Several rounds of each source: the
    // key is 256 bits and each call supplies 64.
    for (int i = 0; i < 8; i++) g_key[i] = (u32)(rdtsc() >> (i & 3)) * 0x9E3779B1u;
    for (int round = 0; round < 4; round++) {
        u64 v;
        if (g_have_rdseed && hw_random(true, &v)) mix(v);
        if (g_have_rdrand && hw_random(false, &v)) mix(v);
        mix(rdtsc());
    }
}

void csprng_bytes(void* buf, usize n) {
    u8* out = (u8*)buf;
    u64 irq = interrupts_save();
    while (n) {
        u8 block[32];
        next_block(block);
        usize take = n < sizeof block ? n : sizeof block;
        memcpy(out, block, take);
        out += take;
        n -= take;
        volatile u8* wipe = block;
        for (usize i = 0; i < sizeof block; i++) wipe[i] = 0;
    }
    interrupts_restore(irq);
}

u64 csprng_u64() {
    u64 v;
    csprng_bytes(&v, sizeof v);
    return v;
}

u64 csprng_below(u64 bound) {
    // Rejection sampling: discard the values that would make some results
    // more likely than others.
    u64 limit = ~0ull - (~0ull % bound);
    u64 v;
    do v = csprng_u64();
    while (v >= limit);
    return v % bound;
}

void csprng_add_timing() {
    // The low bits of the time-stamp counter at interrupt arrival are the
    // unpredictable part; rotate so successive samples land on different bits.
    g_pool = ((g_pool << 7) | (g_pool >> 57)) ^ rdtsc();
    g_pool_events++;
}
