// See ustar.h. A tar archive is a sequence of 512-byte headers, each followed
// by the file's data padded to a multiple of 512; two zero blocks end it.
#include <fs/ustar.h>

namespace {

constexpr usize BLOCK = 512;
constexpr usize NAME_LEN = 100, PREFIX_LEN = 155;
constexpr usize OFF_NAME = 0, OFF_MODE = 100, OFF_SIZE = 124, OFF_TYPE = 156, OFF_MAGIC = 257, OFF_PREFIX = 345;

// Octal field of `len` bytes, terminated by NUL or space. False if it holds
// anything else or would overflow.
bool parse_octal(const u8* field, usize len, u64* out) {
    u64 v = 0;
    usize i = 0;
    while (i < len && field[i] == ' ') i++;
    bool any = false;
    for (; i < len && field[i] != 0 && field[i] != ' '; i++) {
        if (field[i] < '0' || field[i] > '7') return false;
        if (v >> 61) return false;              // the next shift would overflow
        v = (v << 3) | (u64)(field[i] - '0');
        any = true;
    }
    *out = v;
    return any;
}

bool is_zero_block(const u8* b) {
    for (usize i = 0; i < BLOCK; i++)
        if (b[i]) return false;
    return true;
}

const char* skip_prefix(const char* p) {
    for (;;) {
        if (p[0] == '/') p++;
        else if (p[0] == '.' && p[1] == '/') p += 2;
        else return p;
    }
}

// Builds "prefix/name" into out (NUL-terminated). Both fields may lack a NUL.
void entry_name(const u8* h, char out[NAME_LEN + PREFIX_LEN + 2]) {
    usize n = 0;
    if (h[OFF_PREFIX]) {
        for (usize i = 0; i < PREFIX_LEN && h[OFF_PREFIX + i]; i++) out[n++] = (char)h[OFF_PREFIX + i];
        out[n++] = '/';
    }
    for (usize i = 0; i < NAME_LEN && h[OFF_NAME + i]; i++) out[n++] = (char)h[OFF_NAME + i];
    out[n] = 0;
}

// Compares two paths, ignoring one trailing '/' on the archive's side
// (directories are stored as "dir/").
bool same_path(const char* entry, const char* want) {
    usize i = 0;
    while (entry[i] && want[i] && entry[i] == want[i]) i++;
    if (!entry[i] && !want[i]) return true;
    return entry[i] == '/' && !entry[i + 1] && !want[i];
}

} // namespace

usize ustar_each(const u8* archive, usize archive_size, bool (*fn)(const char*, const UstarEntry&, void*), void* ctx) {
    usize visited = 0;
    usize off = 0;
    while (off <= archive_size && archive_size - off >= BLOCK) {
        const u8* h = archive + off;
        if (is_zero_block(h)) break;
        if (h[OFF_MAGIC] != 'u' || h[OFF_MAGIC + 1] != 's' || h[OFF_MAGIC + 2] != 't' || h[OFF_MAGIC + 3] != 'a' ||
            h[OFF_MAGIC + 4] != 'r')
            break;
        u64 size = 0, mode = 0;
        if (!parse_octal(h + OFF_SIZE, 12, &size)) break;
        if (!parse_octal(h + OFF_MODE, 8, &mode)) mode = 0;
        usize data_off = off + BLOCK;
        if (size > archive_size - data_off) break;          // data would run past the archive
        u8 type = h[OFF_TYPE];
        if (type == '0' || type == 0 || type == '5') {
            char name[NAME_LEN + PREFIX_LEN + 2];
            entry_name(h, name);
            UstarEntry e{archive + data_off, type == '5' ? 0 : (usize)size, (u32)(mode & 07777), type == '5'};
            visited++;
            if (!fn(skip_prefix(name), e, ctx)) break;
        }
        usize padded = (usize)((size + BLOCK - 1) / BLOCK * BLOCK);
        if (padded > archive_size - data_off) break;
        off = data_off + padded;
    }
    return visited;
}

namespace {
struct FindCtx {
    const char* want;
    UstarEntry* out;
    bool found;
};
bool find_cb(const char* name, const UstarEntry& e, void* ctx) {
    FindCtx* c = (FindCtx*)ctx;
    if (!same_path(name, c->want)) return true;
    *c->out = e;
    c->found = true;
    return false;
}
} // namespace

bool ustar_find(const u8* archive, usize archive_size, const char* path, UstarEntry* out) {
    FindCtx c{skip_prefix(path), out, false};
    ustar_each(archive, archive_size, find_cb, &c);
    return c.found;
}
