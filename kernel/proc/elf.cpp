// See elf.h.
#include <proc/elf.h>

namespace {

constexpr u64 PAGE = 4096;
constexpr u16 ET_DYN = 3;
constexpr u16 EM_X86_64 = 62;
constexpr u32 PT_LOAD = 1;
constexpr u32 PF_X = 1, PF_W = 2;

// Fields are read byte by byte: the file may be at any alignment, and this
// makes the byte order explicit.
u16 rd16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
u64 rd64(const u8* p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }

// a + b, or false on overflow.
bool add(u64 a, u64 b, u64* out) {
    if (a > ~0ull - b) return false;
    *out = a + b;
    return true;
}

} // namespace

ElfError elf_parse(const u8* file, usize size, ElfImage* out) {
    constexpr usize EHDR = 64, PHDR = 56;
    if (size < EHDR) return ElfError::TooSmall;
    if (file[0] != 0x7F || file[1] != 'E' || file[2] != 'L' || file[3] != 'F') return ElfError::NotElf;
    if (file[4] != 2 /* 64-bit */ || file[5] != 1 /* little-endian */ || file[6] != 1 /* version */)
        return ElfError::WrongFormat;
    if (rd16(file + 18) != EM_X86_64 || rd32(file + 20) != 1) return ElfError::WrongFormat;
    if (rd16(file + 16) != ET_DYN) return ElfError::NotPie;

    u64 entry = rd64(file + 24);
    u64 phoff = rd64(file + 32);
    u16 phentsize = rd16(file + 54);
    u16 phnum = rd16(file + 56);
    if (phentsize != PHDR || phnum == 0) return ElfError::BadHeader;
    u64 table_bytes = (u64)phnum * PHDR;        // at most 65535 * 56: cannot overflow
    u64 table_end;
    if (!add(phoff, table_bytes, &table_end) || table_end > size) return ElfError::BadHeader;

    ElfImage img{};
    u64 highest = 0;
    for (u16 i = 0; i < phnum; i++) {
        const u8* ph = file + phoff + (u64)i * PHDR;
        if (rd32(ph) != PT_LOAD) continue;
        u32 flags = rd32(ph + 4);
        u64 offset = rd64(ph + 8), vaddr = rd64(ph + 16), filesz = rd64(ph + 32), memsz = rd64(ph + 40);
        if (memsz == 0) continue;
        if (img.segment_count == ELF_MAX_SEGMENTS) return ElfError::TooManySegments;
        if ((flags & PF_W) && (flags & PF_X)) return ElfError::WritableAndExecutable;

        u64 file_end, mem_end;
        if (filesz > memsz) return ElfError::BadSegment;
        if (!add(offset, filesz, &file_end) || file_end > size) return ElfError::BadSegment;
        if (!add(vaddr, memsz, &mem_end) || mem_end > ELF_MAX_IMAGE_SPAN) return ElfError::BadSegment;
        // The file bytes must land at the same offset within a page as they
        // have in the file's address, or the segment cannot be mapped.
        if ((offset % PAGE) != (vaddr % PAGE)) return ElfError::BadSegment;

        ElfSegment& s = img.segments[img.segment_count];
        s.vaddr = vaddr / PAGE * PAGE;
        s.data_skew = vaddr - s.vaddr;
        s.mem_size = (mem_end - s.vaddr + PAGE - 1) / PAGE * PAGE;
        s.file_offset = offset;
        s.file_size = filesz;
        s.writable = flags & PF_W;
        s.executable = flags & PF_X;
        // Page-rounded segments must not overlap one another.
        for (u32 j = 0; j < img.segment_count; j++) {
            const ElfSegment& o = img.segments[j];
            if (s.vaddr < o.vaddr + o.mem_size && o.vaddr < s.vaddr + s.mem_size) return ElfError::Overlap;
        }
        if (s.vaddr + s.mem_size > highest) highest = s.vaddr + s.mem_size;
        img.segment_count++;
    }
    if (img.segment_count == 0) return ElfError::NoSegments;

    bool entry_ok = false;
    for (u32 j = 0; j < img.segment_count; j++) {
        const ElfSegment& s = img.segments[j];
        if (s.executable && entry >= s.vaddr + s.data_skew && entry < s.vaddr + s.data_skew + s.file_size)
            entry_ok = true;
    }
    if (!entry_ok) return ElfError::BadEntry;

    img.entry = entry;
    img.span = highest;
    *out = img;
    return ElfError::Ok;
}

const char* elf_error_name(ElfError e) {
    switch (e) {
    case ElfError::Ok: return "ok";
    case ElfError::TooSmall: return "file too small";
    case ElfError::NotElf: return "not an ELF file";
    case ElfError::WrongFormat: return "not a 64-bit x86-64 ELF";
    case ElfError::NotPie: return "not position-independent";
    case ElfError::BadHeader: return "bad program header table";
    case ElfError::BadSegment: return "bad segment";
    case ElfError::TooManySegments: return "too many segments";
    case ElfError::Overlap: return "overlapping segments";
    case ElfError::WritableAndExecutable: return "segment is writable and executable";
    case ElfError::NoSegments: return "no loadable segments";
    case ElfError::BadEntry: return "entry point outside the code";
    }
    return "unknown";
}
