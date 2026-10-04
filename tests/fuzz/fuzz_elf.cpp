// Fuzz target for the ELF parser (kernel/proc/elf.cpp). Besides not
// crashing, a file the parser accepts must satisfy everything the loader
// relies on without checking again; a violation aborts, which the driver
// reports as a crash.
#include <proc/elf.h>

extern "C" const char* const fuzz_name = "elf";

#define REQUIRE(cond) \
    do { \
        if (!(cond)) __builtin_trap(); \
    } while (0)

extern "C" void fuzz_one(const u8* data, usize size) {
    ElfImage img{};
    ElfError e = elf_parse(data, size, &img);
    (void)elf_error_name(e);
    if (e != ElfError::Ok) return;

    REQUIRE(img.segment_count >= 1 && img.segment_count <= ELF_MAX_SEGMENTS);
    REQUIRE(img.span % PAGE_SIZE == 0 && img.span != 0 && img.span <= ELF_MAX_IMAGE_SPAN + PAGE_SIZE);
    bool entry_in_code = false;
    volatile u8 sink = 0;
    for (u32 i = 0; i < img.segment_count; i++) {
        const ElfSegment& s = img.segments[i];
        REQUIRE(s.vaddr % PAGE_SIZE == 0 && s.mem_size % PAGE_SIZE == 0 && s.mem_size != 0);
        REQUIRE(s.vaddr + s.mem_size <= img.span);
        REQUIRE(!(s.writable && s.executable));
        REQUIRE(s.data_skew < PAGE_SIZE);
        // The bytes the loader will copy lie inside the file and inside the segment.
        REQUIRE(s.file_offset <= size && s.file_size <= size - s.file_offset);
        REQUIRE(s.data_skew + s.file_size <= s.mem_size);
        if (s.file_size) sink = sink + data[s.file_offset] + data[s.file_offset + s.file_size - 1];
        for (u32 j = 0; j < i; j++) {
            const ElfSegment& o = img.segments[j];
            REQUIRE(s.vaddr + s.mem_size <= o.vaddr || o.vaddr + o.mem_size <= s.vaddr);
        }
        if (s.executable && img.entry >= s.vaddr && img.entry < s.vaddr + s.mem_size) entry_in_code = true;
    }
    REQUIRE(entry_in_code);
}
