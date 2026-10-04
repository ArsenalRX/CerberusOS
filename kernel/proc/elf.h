// ELF64 parsing for user programs. The file is untrusted input: every
// offset, size and count is checked against the file size and against
// arithmetic overflow before it is used, and anything the loader does not
// positively understand is refused. This file depends only on lib/types.h
// so the same code is built for the host and fuzzed (`make fuzz`).
//
// Accepted: 64-bit little-endian x86-64, type ET_DYN (position-independent;
// the loader picks a random base), with up to ELF_MAX_SEGMENTS PT_LOAD
// segments that are page-congruent, do not overlap, and are never both
// writable and executable.
#pragma once

#include <lib/types.h>

constexpr usize ELF_MAX_SEGMENTS = 8;
constexpr u64 ELF_MAX_IMAGE_SPAN = 1ull << 30;     // 1 GiB of address space per program

enum class ElfError : u8 {
    Ok,
    TooSmall,
    NotElf,
    WrongFormat,        // not 64-bit little-endian x86-64, or a bad version
    NotPie,             // only position-independent executables are loaded
    BadHeader,          // header fields out of range
    BadSegment,         // a segment's offsets or sizes do not fit
    TooManySegments,
    Overlap,
    WritableAndExecutable,
    NoSegments,
    BadEntry,           // the entry point is not inside an executable segment
};

struct ElfSegment {
    u64 vaddr;          // page-aligned start, relative to the load base
    u64 mem_size;       // page-aligned size in memory
    u64 file_offset;    // where the bytes come from; data starts at vaddr + data_skew
    u64 data_skew;      // offset of the first file byte within the first page
    u64 file_size;      // bytes to copy from the file (the rest is zero)
    bool writable;
    bool executable;
};

struct ElfImage {
    u64 entry;          // relative to the load base
    u64 span;           // page-aligned size of the address range the image needs
    u32 segment_count;
    ElfSegment segments[ELF_MAX_SEGMENTS];
};

ElfError elf_parse(const u8* file, usize size, ElfImage* out);
const char* elf_error_name(ElfError e);
