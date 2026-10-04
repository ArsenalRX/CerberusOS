// A small general-purpose allocator on top of mmap. Memory comes from the
// kernel in 256 KiB arenas; each arena is a run of blocks with a size header,
// and free blocks sit on one address-ordered list so neighbours can be
// merged when freed. Requests of 128 KiB or more get their own mapping and
// go straight back to the kernel on free. One lock makes it safe to call
// from several threads.
#include <cerberus.h>

namespace {

constexpr size_t ALIGN = 16;
constexpr size_t ARENA = 256 * 1024;
constexpr size_t BIG = 128 * 1024;
constexpr size_t PAGE = 4096;
constexpr size_t FLAG_BIG = 1;          // low bit of the size: the block is its own mapping

struct Block {
    size_t size;        // whole block including this header; low bit = FLAG_BIG
    size_t pad;         // keeps the payload 16-byte aligned
};
struct FreeBlock {
    size_t size;
    FreeBlock* next;    // next free block by address
};

FreeBlock* g_free = nullptr;
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

inline size_t round_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }

// Adds a block to the free list in address order, merging with neighbours.
void insert_free(FreeBlock* b) {
    FreeBlock** link = &g_free;
    FreeBlock* prev = nullptr;
    while (*link && *link < b) {
        prev = *link;
        link = &(*link)->next;
    }
    b->next = *link;
    *link = b;
    if (b->next && (char*)b + b->size == (char*)b->next) {      // merge with the block after
        b->size += b->next->size;
        b->next = b->next->next;
    }
    if (prev && (char*)prev + prev->size == (char*)b) {         // merge with the block before
        prev->size += b->size;
        prev->next = b->next;
    }
}

} // namespace

extern "C" {

static void* malloc_locked(size_t n) {
    if (n == 0) return nullptr;
    if (n > (size_t)-1 - sizeof(Block) - PAGE) {
        errno = ENOMEM;
        return nullptr;
    }
    size_t need = round_up(n + sizeof(Block), ALIGN);

    if (need >= BIG) {
        size_t len = round_up(need, PAGE);
        void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return nullptr;
        Block* b = (Block*)p;
        b->size = len | FLAG_BIG;
        return b + 1;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        for (FreeBlock** link = &g_free; *link; link = &(*link)->next) {
            FreeBlock* f = *link;
            if (f->size < need) continue;
            if (f->size - need >= sizeof(FreeBlock) + ALIGN) {      // split: the tail stays free
                FreeBlock* rest = (FreeBlock*)((char*)f + need);
                rest->size = f->size - need;
                rest->next = f->next;
                *link = rest;
            } else {
                need = f->size;
                *link = f->next;
            }
            Block* b = (Block*)f;
            b->size = need;
            return b + 1;
        }
        // Nothing fits: take another arena from the kernel and look again.
        void* arena = mmap(nullptr, ARENA, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (arena == MAP_FAILED) return nullptr;
        FreeBlock* fresh = (FreeBlock*)arena;
        fresh->size = ARENA;
        insert_free(fresh);
    }
    errno = ENOMEM;
    return nullptr;
}

void* malloc(size_t n) {
    pthread_mutex_lock(&g_lock);
    void* p = malloc_locked(n);
    pthread_mutex_unlock(&g_lock);
    return p;
}

void free(void* p) {
    if (!p) return;
    Block* b = (Block*)p - 1;
    if (b->size & FLAG_BIG) {
        munmap(b, b->size & ~FLAG_BIG);
        return;
    }
    FreeBlock* f = (FreeBlock*)b;
    f->size = b->size;
    pthread_mutex_lock(&g_lock);
    insert_free(f);
    pthread_mutex_unlock(&g_lock);
}

void* calloc(size_t count, size_t size) {
    if (size && count > (size_t)-1 / size) {
        errno = ENOMEM;
        return nullptr;
    }
    void* p = malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}

void* realloc(void* p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) {
        free(p);
        return nullptr;
    }
    Block* b = (Block*)p - 1;
    size_t have = (b->size & ~FLAG_BIG) - sizeof(Block);
    if (n <= have) return p;
    void* fresh = malloc(n);
    if (!fresh) return nullptr;
    memcpy(fresh, p, have);
    free(p);
    return fresh;
}

} // extern "C"
