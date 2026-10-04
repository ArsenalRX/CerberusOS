// mkfs.lumfs for the build machine: formats a disk image with lumfs, or
// checks one (an independent reader of the format, used by the tests).
//
//   mkfs.lumfs [-L label] [-s size] image     create/format (size: 64M, 1G, ...)
//   mkfs.lumfs --check [--replay] image       verify; --replay first applies a
//                                             committed journal transaction
//
// The check walks the whole tree from the root: every inode's checksum and
// fields, every directory record, every indirect block, link counts, and the
// bitmap against the blocks actually in use. Exit status 0 = consistent,
// 1 = damaged, 2 = usage or I/O error. Leaked blocks or inodes (allocated
// but unreferenced, which a crash between allocation and use can leave) are
// reported as warnings, not damage.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <vector>

#include <fs/lumfs_mkfs.h>

using namespace lumfs;

namespace {

FILE* g_img = nullptr;
u64 g_blocks = 0;
int g_errors = 0, g_warnings = 0;

bool read_block(u64 b, u8* out) {
    if (b >= g_blocks) return false;
    if (fseeko(g_img, (off_t)(b * BLOCK), SEEK_SET) != 0) return false;
    return fread(out, 1, BLOCK, g_img) == BLOCK;
}

bool write_block(u64 b, const u8* data) {
    if (fseeko(g_img, (off_t)(b * BLOCK), SEEK_SET) != 0) return false;
    return fwrite(data, 1, BLOCK, g_img) == BLOCK;
}

void error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "damage: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    if (++g_errors > 50) {
        fprintf(stderr, "too many problems; stopping\n");
        exit(1);
    }
}

void warning(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void warning(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "warning: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    g_warnings++;
}

u64 parse_size(const char* s) {
    char* end;
    u64 v = strtoull(s, &end, 10);
    switch (*end) {
    case 'k': case 'K': v *= 1024; break;
    case 'm': case 'M': v *= 1024 * 1024; break;
    case 'g': case 'G': v *= 1024ull * 1024 * 1024; break;
    }
    return v;
}

// ---------------------------------------------------------------- check --
struct Checker {
    SuperBlock sb;
    std::vector<u8> used;           // per block: 1 = referenced (metadata or a file)
    std::vector<u32> found_links;   // per inode: names found + "." / ".." contributions
    std::vector<u8> visited;        // per inode
    std::vector<Inode> inodes;

    bool load_inode(u32 ino, Inode* out) {
        u8 b[BLOCK];
        if (!read_block(inode_block(sb, ino), b)) return false;
        memcpy(out, b + inode_offset(ino), sizeof *out);
        return true;
    }

    void use(u64 b, const char* what, u32 ino) {
        if (b < sb.data_start || b >= sb.total_blocks) {
            error("inode %u: %s block %llu out of range", ino, what, (unsigned long long)b);
            return;
        }
        if (used[b]) error("block %llu used twice (inode %u, %s)", (unsigned long long)b, ino, what);
        used[b] = 1;
    }

    // Collects the data blocks of an inode (and its indirect blocks).
    void file_blocks(u32 ino, const Inode& di, std::vector<u64>& out) {
        for (u32 k = 0; k < DIRECT; k++)
            if (di.direct[k]) {
                use(di.direct[k], "data", ino);
                out.push_back(di.direct[k]);
            } else {
                out.push_back(0);
            }
        auto level1 = [&](u64 blk) {
            u8 b[BLOCK];
            use(blk, "indirect", ino);
            if (!read_block(blk, b) || !block_ok(b, MAGIC_INDIRECT, ino) || ((BlockHeader*)b)->extra != 1) {
                error("inode %u: bad indirect block %llu", ino, (unsigned long long)blk);
                return;
            }
            for (u32 e = 0; e < PTRS_PER_BLOCK; e++) {
                u32 v;
                memcpy(&v, b + HEADER + e * 4, 4);
                if (v) use(v, "data", ino);
                out.push_back(v);
            }
        };
        if (di.indirect) level1(di.indirect);
        if (di.dindirect) {
            u8 b[BLOCK];
            use(di.dindirect, "double indirect", ino);
            if (!read_block(di.dindirect, b) || !block_ok(b, MAGIC_INDIRECT, ino) || ((BlockHeader*)b)->extra != 2) {
                error("inode %u: bad double-indirect block", ino);
                return;
            }
            while (out.size() < DIRECT + PTRS_PER_BLOCK) out.push_back(0);
            for (u32 e = 0; e < PTRS_PER_BLOCK; e++) {
                u32 v;
                memcpy(&v, b + HEADER + e * 4, 4);
                if (v) level1(v);
                else
                    for (u32 k = 0; k < PTRS_PER_BLOCK; k++) out.push_back(0);
            }
        }
    }

    void walk(u32 ino, u32 parent, int depth) {
        if (depth > 200) {
            error("directory tree too deep (a loop?) at inode %u", ino);
            return;
        }
        if (ino == 0 || ino > sb.inode_count) {
            error("reference to inode %u out of range", ino);
            return;
        }
        found_links[ino]++;
        if (visited[ino]) {
            if ((inodes[ino].mode & T_MASK) == T_DIR) error("directory inode %u has two names", ino);
            return;
        }
        visited[ino] = 1;
        Inode di;
        if (!load_inode(ino, &di) || di.mode == 0) {
            error("name refers to free inode %u", ino);
            return;
        }
        if (!check_inode(sb, di)) {
            error("inode %u: bad checksum or fields", ino);
            return;
        }
        inodes[ino] = di;
        std::vector<u64> blocks;
        file_blocks(ino, di, blocks);
        if ((di.mode & T_MASK) != T_DIR) return;
        u64 nblocks = di.size / BLOCK;
        bool saw_dot = false, saw_dotdot = false;
        for (u64 i = 0; i < nblocks; i++) {
            u64 b = i < blocks.size() ? blocks[i] : 0;
            if (!b) {
                error("directory %u has a hole at block %llu", ino, (unsigned long long)i);
                continue;
            }
            u8 data[BLOCK];
            if (!read_block(b, data) || !block_ok(data, MAGIC_DIR, ino)) {
                error("directory %u: bad block %llu", ino, (unsigned long long)b);
                continue;
            }
            std::vector<std::pair<u32, u8>> children;
            bool ok = each_dirrec(data, sb.inode_count, [&](const DirRec* r, u32) {
                if (!r->inode) return true;
                bool dot = r->name_len == 1 && r->name[0] == '.';
                bool dotdot = r->name_len == 2 && r->name[0] == '.' && r->name[1] == '.';
                if (dot) {
                    saw_dot = true;
                    if (r->inode != ino) error("directory %u: '.' points to %u", ino, r->inode);
                    else found_links[ino]++;
                } else if (dotdot) {
                    saw_dotdot = true;
                    if (r->inode != parent) error("directory %u: '..' points to %u, parent is %u", ino, r->inode, parent);
                    else found_links[parent]++;
                } else {
                    children.push_back({r->inode, r->type});
                }
                return true;
            });
            if (!ok) error("directory %u: malformed records in block %llu", ino, (unsigned long long)b);
            for (auto& c : children) walk(c.first, ino, depth + 1);
        }
        if (!saw_dot || !saw_dotdot) error("directory %u lacks '.' or '..'", ino);
    }
};

int check(const char* path, bool replay_journal) {
    g_img = fopen(path, replay_journal ? "r+b" : "rb");
    if (!g_img) {
        perror(path);
        return 2;
    }
    fseeko(g_img, 0, SEEK_END);
    g_blocks = (u64)ftello(g_img) / BLOCK;
    Checker c;
    u8 b[BLOCK];
    if (!read_block(0, b)) {
        fprintf(stderr, "%s: cannot read the superblock\n", path);
        return 2;
    }
    memcpy(&c.sb, b, sizeof c.sb);
    if (!check_super(c.sb, g_blocks)) {
        error("superblock invalid");
        return 1;
    }
    SuperBlock& s = c.sb;
    // Journal.
    if (!read_block(s.journal_start, b) || !block_ok(b, MAGIC_JHEAD, 0)) {
        error("journal header invalid");
        return 1;
    }
    u64 seq = ((JournalHeader*)b)->sequence;
    u8 d[BLOCK];
    if (read_block(s.journal_start + 1, d) && block_ok(d, MAGIC_JDESC, (u32)seq) && ((JournalDesc*)d)->sequence == seq) {
        u32 count = ((JournalDesc*)d)->h.extra;
        bool complete = count > 0 && count <= JOURNAL_MAX_TX && 2 + count < s.journal_blocks;
        u32 crc = 0;
        std::vector<u8> images((size_t)count * BLOCK);
        for (u32 i = 0; complete && i < count; i++) {
            complete = read_block(s.journal_start + 2 + i, &images[(size_t)i * BLOCK]);
            crc = crc32c(crc, &images[(size_t)i * BLOCK], BLOCK);
        }
        u8 cm[BLOCK];
        complete = complete && read_block(s.journal_start + 2 + count, cm) && block_ok(cm, MAGIC_JCOMMIT, (u32)seq) &&
                   ((JournalCommit*)cm)->images_crc == crc && ((JournalCommit*)cm)->h.extra == count;
        if (complete && replay_journal) {
            for (u32 i = 0; i < count; i++) write_block(((JournalDesc*)d)->targets[i], &images[(size_t)i * BLOCK]);
            JournalHeader* jh = (JournalHeader*)b;
            jh->sequence = seq + 1;
            seal_block(b);
            write_block(s.journal_start, b);
            fflush(g_img);
            printf("replayed journal transaction %llu (%u blocks)\n", (unsigned long long)seq, count);
            read_block(0, b);
            memcpy(&c.sb, b, sizeof c.sb);
            if (!check_super(c.sb, g_blocks)) {
                error("superblock invalid after replay");
                return 1;
            }
        } else if (complete) {
            printf("note: a committed journal transaction (%u blocks) is waiting to be replayed\n", count);
        }
    }
    c.used.assign(s.total_blocks, 0);
    for (u64 i = 0; i < s.data_start; i++) c.used[i] = 1;
    c.found_links.assign(s.inode_count + 1, 0);
    c.visited.assign(s.inode_count + 1, 0);
    c.inodes.assign(s.inode_count + 1, Inode{});
    c.walk(ROOT_INODE, ROOT_INODE, 0);
    c.found_links[ROOT_INODE]--;        // the walk's own entry into the root is not a name

    // Link counts and unreferenced inodes.
    u32 used_inodes = 0;
    for (u32 ino = 1; ino <= s.inode_count; ino++) {
        Inode di{};
        c.load_inode(ino, &di);
        if (di.mode == 0) continue;
        used_inodes++;
        if (!c.visited[ino]) {
            warning("inode %u is allocated but has no name (leaked)", ino);
            continue;
        }
        if (di.links != c.found_links[ino])
            error("inode %u: link count %u, but %u names refer to it", ino, di.links, c.found_links[ino]);
    }
    // Bitmap.
    u64 bitmap_used = 0;
    for (u32 bi = 0; bi < s.bitmap_blocks; bi++) {
        if (!read_block(s.bitmap_start + bi, b) || !block_ok(b, MAGIC_BITMAP, bi)) {
            error("bitmap block %u invalid", bi);
            continue;
        }
        for (u32 bit = 0; bit < BITS_PER_BITMAP_BLOCK; bit++) {
            u64 blk = (u64)bi * BITS_PER_BITMAP_BLOCK + bit;
            if (blk >= s.total_blocks) break;
            bool set = b[HEADER + bit / 8] & (1u << (bit % 8));
            bitmap_used += set;
            if (c.used[blk] && !set) error("block %llu is in use but marked free", (unsigned long long)blk);
            if (!c.used[blk] && set) warning("block %llu is marked used but nothing refers to it (leaked)", (unsigned long long)blk);
        }
    }
    if (s.total_blocks - bitmap_used != s.free_blocks)
        warning("free block count %llu, bitmap says %llu", (unsigned long long)s.free_blocks,
                (unsigned long long)(s.total_blocks - bitmap_used));
    if (s.inode_count - used_inodes != s.free_inodes)
        warning("free inode count %u, table says %u", s.free_inodes, s.inode_count - used_inodes);
    printf("%s: %s, %llu blocks (%llu free), %u inodes (%u in use), %d damage, %d warnings\n", path,
           g_errors ? "DAMAGED" : "consistent", (unsigned long long)s.total_blocks,
           (unsigned long long)s.free_blocks, s.inode_count, used_inodes, g_errors, g_warnings);
    return g_errors ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* label = "lumen";
    u64 size = 0;
    bool do_check = false, replay = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-L") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) size = parse_size(argv[++i]);
        else if (!strcmp(argv[i], "--check")) do_check = true;
        else if (!strcmp(argv[i], "--replay")) replay = true;
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: mkfs.lumfs [-L label] [-s size] image\n       mkfs.lumfs --check [--replay] image\n");
        return 2;
    }
    if (do_check) return check(path, replay);

    g_img = fopen(path, "r+b");
    if (!g_img) g_img = fopen(path, "w+b");
    if (!g_img) {
        perror(path);
        return 2;
    }
    if (size) {
        if (ftruncate(fileno(g_img), (off_t)size) != 0) {
            perror("resize");
            return 2;
        }
    }
    fseeko(g_img, 0, SEEK_END);
    u64 bytes = (u64)ftello(g_img);
    u8 uuid[16];
    FILE* r = fopen("/dev/urandom", "rb");
    if (!r || fread(uuid, 1, 16, r) != 16) {
        fprintf(stderr, "no random source for the UUID\n");
        return 2;
    }
    fclose(r);
    MkfsResult res = mkfs(bytes / BLOCK, label, uuid, (u64)time(nullptr),
                          [](u64 b, const u8* data) { return write_block(b, data); });
    fclose(g_img);
    if (!res.ok) {
        fprintf(stderr, "mkfs.lumfs: %s\n", res.error);
        return 2;
    }
    printf("%s: lumfs, %llu MiB, %u inodes, journal %u KiB, label \"%s\"\n", path,
           (unsigned long long)(bytes >> 20), res.sb.inode_count, res.sb.journal_blocks * 4, label);
    return 0;
}
