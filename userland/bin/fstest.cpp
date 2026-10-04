// File-system test tool (phase 9 acceptance tests).
//
//   fstest write <file> <MiB>     writes a deterministic pattern, then fsync
//   fstest verify <file> <MiB>    reads it back and compares every byte
//   fstest churn <dir> [rounds]   creates, writes, renames and deletes files
//                                 forever (or for `rounds`), fsyncing as it
//                                 goes: run it, then pull the plug
//   fstest readtwice <file>       reads a file twice and prints the time of
//                                 each pass (the second should hit the cache)
#include <cerberus.h>

namespace {

constexpr size_t CHUNK = 65536;
uint8_t g_buf[CHUNK], g_want[CHUNK];

// Byte `i` of the pattern: a hash of its offset, so any misplaced block shows.
void pattern(uint64_t offset, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint64_t x = (offset + i) * 0x9E3779B97F4A7C15ull;
        x ^= x >> 29;
        out[i] = (uint8_t)(x ^ (x >> 17));
    }
}

uint64_t now_ms() {
    return (uint64_t)syscall6(SYS_time_ms, 0, 0, 0, 0, 0, 0);
}

int do_write(const char* path, uint64_t mib) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        dprintf(2, "fstest: %s: %s\n", path, strerror(errno));
        return 1;
    }
    uint64_t total = mib << 20;
    for (uint64_t off = 0; off < total; off += CHUNK) {
        pattern(off, g_buf, CHUNK);
        if (write(fd, g_buf, CHUNK) != (long)CHUNK) {
            dprintf(2, "fstest: write at %lu: %s\n", (unsigned long)off, strerror(errno));
            return 1;
        }
    }
    if (fsync(fd) < 0) {
        dprintf(2, "fstest: fsync: %s\n", strerror(errno));
        return 1;
    }
    close(fd);
    printf("fstest: wrote %lu MiB to %s\n", (unsigned long)mib, path);
    return 0;
}

int do_verify(const char* path, uint64_t mib) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(2, "fstest: %s: %s\n", path, strerror(errno));
        return 1;
    }
    struct stat st;
    fstat(fd, &st);
    uint64_t total = mib << 20;
    if (st.st_size != total) {
        printf("fstest: MISMATCH: %s is %lu bytes, expected %lu\n", path, (unsigned long)st.st_size,
               (unsigned long)total);
        return 1;
    }
    for (uint64_t off = 0; off < total; off += CHUNK) {
        long n = read(fd, g_buf, CHUNK);
        if (n != (long)CHUNK) {
            printf("fstest: MISMATCH: short read at %lu\n", (unsigned long)off);
            return 1;
        }
        pattern(off, g_want, CHUNK);
        if (memcmp(g_buf, g_want, CHUNK)) {
            for (size_t i = 0; i < CHUNK; i++)
                if (g_buf[i] != g_want[i]) {
                    printf("fstest: MISMATCH at byte %lu\n", (unsigned long)(off + i));
                    return 1;
                }
        }
    }
    close(fd);
    printf("fstest: verified %lu MiB of %s, every byte matches\n", (unsigned long)mib, path);
    return 0;
}

int do_churn(const char* dir, long rounds) {
    char a[256], b[256];
    for (long r = 0; rounds < 0 || r < rounds; r++) {
        snprintf(a, sizeof a, "%s/churn-%ld.tmp", dir, r % 8);
        snprintf(b, sizeof b, "%s/churn-%ld.dat", dir, r % 8);
        int fd = open(a, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            dprintf(2, "fstest: %s: %s\n", a, strerror(errno));
            return 1;
        }
        size_t size = 4096 + (size_t)(r % 13) * 37000;
        for (size_t done = 0; done < size;) {
            size_t n = size - done < CHUNK ? size - done : CHUNK;
            pattern(r * 1000003 + done, g_buf, n);
            if (write(fd, g_buf, n) != (long)n) {
                dprintf(2, "fstest: write: %s\n", strerror(errno));
                return 1;
            }
            done += n;
        }
        if (r % 3 == 0) fsync(fd);
        close(fd);
        if (rename(a, b) < 0) {
            dprintf(2, "fstest: rename: %s\n", strerror(errno));
            return 1;
        }
        if (r % 5 == 4) {
            snprintf(a, sizeof a, "%s/churn-%ld.dat", dir, (r + 3) % 8);
            unlink(a);
        }
        if (r % 50 == 0) {
            snprintf(a, sizeof a, "%s/sub-%ld", dir, r / 50 % 4);
            mkdir(a, 0755);
        }
        if (r % 100 == 0) printf("fstest: churn round %ld\n", r);
    }
    printf("fstest: churn done\n");
    return 0;
}

int do_readtwice(const char* path) {
    uint64_t times[3] = {0, 0, 0};
    for (int pass = 1; pass <= 2; pass++) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            dprintf(2, "fstest: %s: %s\n", path, strerror(errno));
            return 1;
        }
        uint64_t t0 = now_ms(), bytes = 0;
        for (;;) {
            long n = read(fd, g_buf, CHUNK);
            if (n <= 0) break;
            bytes += (uint64_t)n;
        }
        close(fd);
        uint64_t ms = now_ms() - t0;
        printf("fstest: pass %d read %lu MiB in %lu ms\n", pass, (unsigned long)(bytes >> 20), (unsigned long)ms);
        times[pass] = ms;
    }
    if (times[2] * 2 < times[1]) printf("fstest: the second pass came from the cache\n");
    else printf("fstest: the second pass was not faster\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "write")) return do_write(argv[2], strtoul(argv[3], nullptr, 10));
    if (argc >= 4 && !strcmp(argv[1], "verify")) return do_verify(argv[2], strtoul(argv[3], nullptr, 10));
    if (argc >= 3 && !strcmp(argv[1], "churn")) return do_churn(argv[2], argc >= 4 ? (long)strtoul(argv[3], nullptr, 10) : -1);
    if (argc >= 3 && !strcmp(argv[1], "readtwice")) return do_readtwice(argv[2]);
    dprintf(2, "usage: fstest write|verify <file> <MiB> | churn <dir> [rounds] | readtwice <file>\n");
    return 2;
}
