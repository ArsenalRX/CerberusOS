// Fuzz target for the tar reader (kernel/fs/ustar.cpp): walks a damaged
// archive, reads every byte of every entry it reports, and looks names up.
#include <fs/ustar.h>

extern "C" const char* const fuzz_name = "ustar";

namespace {

struct Walk {
    const u8* archive;
    usize size;
    u64 sum;
};

bool visit(const char* name, const UstarEntry& e, void* ctx) {
    Walk* w = (Walk*)ctx;
    // The entry's bytes must lie inside the archive.
    if (e.data < w->archive || e.size > w->size || (usize)(e.data - w->archive) > w->size - e.size) __builtin_trap();
    for (usize i = 0; i < e.size; i++) w->sum += e.data[i];
    usize len = 0;
    while (name[len]) len++;                    // the name must be terminated
    if (len > 100 + 155 + 1) __builtin_trap();
    w->sum += len + e.mode + e.is_dir;
    return true;
}

} // namespace

extern "C" void fuzz_one(const u8* data, usize size) {
    Walk w{data, size, 0};
    ustar_each(data, size, visit, &w);
    const char* const paths[] = {"/bin/init", "bin/hello", "./bin", "bin/", "", "/", "no/such/file"};
    for (const char* p : paths) {
        UstarEntry e{};
        if (ustar_find(data, size, p, &e)) visit(p, e, &w);
    }
    volatile u64 sink = w.sum;
    (void)sink;
}
