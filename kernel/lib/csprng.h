// The kernel's one source of randomness (SPEC §19.3, §19.9): a ChaCha20-based
// generator with fast key erasure. Every 64-byte block of keystream is split
// in two: the first half becomes the next key, so a later compromise of the
// state cannot reveal earlier output.
//
// Seeding: RDSEED and RDRAND when the CPU has them, always mixed with the
// time-stamp counter, the reference clock and the timing of interrupts.
// More entropy is folded in as interrupts arrive and the key is renewed
// regularly. On a CPU with neither RDSEED nor RDRAND the early output rests
// on timing alone; csprng_init says so in the log.
//
// All functions are interrupt-safe, safe on any CPU, and never sleep.
#pragma once

#include <lib/types.h>

// First thing at boot. Needs only CPUID and the time-stamp counter.
void csprng_init();
// Fills buf with n random bytes.
void csprng_bytes(void* buf, usize n);
u64 csprng_u64();
// A uniform value in [0, bound); bound must be nonzero.
u64 csprng_below(u64 bound);
// Called from interrupt handlers: folds the arrival time into the pool.
void csprng_add_timing();
// The raw ChaCha20 block function, exposed for the self-test against the
// RFC 8439 vector. `out` receives 64 bytes.
void chacha20_block(const u32 key[8], u32 counter, const u32 nonce[3], u8 out[64]);
