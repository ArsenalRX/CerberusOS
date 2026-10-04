// The host-side fuzz driver behind `make fuzz` (SPEC §19.11). It is linked
// with one target (fuzz_elf.cpp, fuzz_ustar.cpp, ...) and the kernel source
// file that target exercises, built for the host with AddressSanitizer and
// the undefined-behaviour checks.
//
//   fuzz-<name> <seconds> <seed-or-regression file>...
//
// Every file given is first run unchanged (so files under tests/fuzz/corpus
// act as regression tests), then mutated copies are run until the time is
// up. Each input is handed over in a heap block of exactly its size, so a
// read one byte past the end is caught. If the target crashes, the input is
// saved as crash-<name>.bin in the current directory; copy it into
// tests/fuzz/corpus/<name>/ before fixing the bug.
//
// FUZZ_SEED=<number> in the environment repeats a run.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

extern "C" void fuzz_one(const uint8_t* data, size_t size);
extern "C" const char* const fuzz_name;
extern "C" void __sanitizer_set_death_callback(void (*callback)(void));

namespace {

using Bytes = std::vector<uint8_t>;

const uint8_t* g_current = nullptr;
size_t g_current_size = 0;

void save_crash() {
    char path[128];
    snprintf(path, sizeof path, "crash-%s.bin", fuzz_name);
    if (FILE* f = fopen(path, "wb")) {
        fwrite(g_current, 1, g_current_size, f);
        fclose(f);
        fprintf(stderr, "fuzz-%s: the crashing input (%zu bytes) was saved as %s\n", fuzz_name, g_current_size, path);
    }
}

uint64_t g_rng;
uint64_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}
uint64_t below(uint64_t n) { return n ? rnd() % n : 0; }

const uint64_t INTERESTING[] = {0,          1,          0x7F,       0x80,        0xFF,        0x100,
                                0x1FF,      0x200,      0xFFF,      0x1000,      0x7FFF,      0x8000,
                                0xFFFF,     0x10000,    0x7FFFFFFF, 0x80000000,  0xFFFFFFFF,  0x100000000,
                                1ull << 30, 1ull << 47, 1ull << 62, 1ull << 63,  ~0ull,       ~0ull - 0xFFF,
                                ~0ull >> 1, '7',        ' ',        0x3737373737373737ull};

// Where a mutation lands: half the time in the first 512 bytes, where file
// formats keep their headers, so that large seeds still get their structure
// exercised.
size_t pick_offset(size_t size) { return (rnd() & 1) ? below(size < 512 ? size : 512) : below(size); }

void mutate(Bytes& b) {
    switch (below(8)) {
    case 0:     // flip a bit
        if (!b.empty()) b[pick_offset(b.size())] ^= (uint8_t)(1u << below(8));
        break;
    case 1:     // random byte
        if (!b.empty()) b[pick_offset(b.size())] = (uint8_t)rnd();
        break;
    case 2:
    case 3: {   // an interesting value, 1 to 8 bytes wide, little-endian
        if (b.empty()) break;
        uint64_t v = INTERESTING[below(sizeof INTERESTING / sizeof INTERESTING[0])];
        if (rnd() & 1) v += below(17) - 8;
        size_t width = (size_t)1 << below(4), at = pick_offset(b.size());
        for (size_t i = 0; i < width && at + i < b.size(); i++) b[at + i] = (uint8_t)(v >> (8 * i));
        break;
    }
    case 4:     // cut the end off
        b.resize(below(b.size() + 1));
        break;
    case 5: {   // grow
        size_t extra = below(1024);
        for (size_t i = 0; i < extra; i++) b.push_back((rnd() & 3) ? 0 : (uint8_t)rnd());
        break;
    }
    case 6: {   // copy a run of bytes over another place
        if (b.size() < 2) break;
        size_t from = pick_offset(b.size()), to = pick_offset(b.size());
        size_t len = below(b.size() - (from > to ? from : to)) % 600;
        memmove(b.data() + to, b.data() + from, len);
        break;
    }
    case 7: {   // fill a run with one byte
        if (b.empty()) break;
        size_t at = pick_offset(b.size()), len = below(b.size() - at) % 600;
        memset(b.data() + at, (rnd() & 1) ? 0 : (int)(rnd() & 0xFF), len);
        break;
    }
    }
}

void run(const Bytes& b) {
    // A block of exactly the input's size: the sanitizer's red zone starts
    // at the first byte past it.
    uint8_t* exact = (uint8_t*)malloc(b.size() ? b.size() : 1);
    if (!b.empty()) memcpy(exact, b.data(), b.size());
    g_current = exact;
    g_current_size = b.size();
    fuzz_one(exact, b.size());
    g_current = nullptr;
    free(exact);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <seconds> <seed file>...\n", argv[0]);
        return 2;
    }
    double seconds = atof(argv[1]);
    std::vector<Bytes> seeds;
    for (int i = 2; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) {
            fprintf(stderr, "fuzz-%s: cannot read %s\n", fuzz_name, argv[i]);
            return 2;
        }
        Bytes b;
        uint8_t chunk[65536];
        for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0;) b.insert(b.end(), chunk, chunk + n);
        fclose(f);
        seeds.push_back(b);
    }
    const char* fixed = getenv("FUZZ_SEED");
    uint64_t seed = fixed ? strtoull(fixed, nullptr, 0) : (uint64_t)time(nullptr) * 0x9E3779B97F4A7C15ull;
    g_rng = seed ? seed : 1;
    __sanitizer_set_death_callback(save_crash);

    for (const Bytes& s : seeds) run(s);
    run(Bytes{});

    timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t runs = 0;
    for (;;) {
        if ((runs & 0xFF) == 0) {
            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9 >= seconds) break;
        }
        Bytes b = seeds[below(seeds.size())];
        for (uint64_t m = 1 + below(below(4) ? 4 : 32); m; m--) mutate(b);
        run(b);
        runs++;
    }
    printf("fuzz-%s: %llu mutated inputs from %zu seed file(s) in %.0f s, no crash (FUZZ_SEED=%llu)\n", fuzz_name,
           (unsigned long long)runs, seeds.size(), seconds, (unsigned long long)seed);
    return 0;
}
