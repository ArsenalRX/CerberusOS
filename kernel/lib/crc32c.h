// CRC32C (Castagnoli), the checksum on lumfs metadata (SPEC §5A phase 9).
// Table-driven, no CPU instructions required; depends only on lib/types.h so
// the host tools and fuzzers build the same code.
#pragma once

#include <lib/types.h>

// Continues a CRC: crc32c(crc32c(0, a), b) == crc32c(0, a ++ b).
u32 crc32c(u32 crc, const void* data, usize n);
