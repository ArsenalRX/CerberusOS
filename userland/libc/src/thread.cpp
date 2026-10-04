// Threads (SPEC phase 11): thread-local storage, a pthread-shaped interface
// over thread_spawn / thread_join, and locks built on futexes.
//
// A thread's memory is one mapping:
//
//   [ unmapped page | stack, growing down ...... | thread-local variables | control block ]
//                                                                         ^ thread pointer (FS base)
//
// The joiner unmaps it. Locks take the kernel's help only when they have to
// wait: an uncontended lock or unlock is one atomic instruction.
#include "internal.h"

namespace {

constexpr size_t PAGE = 4096;
constexpr size_t STACK_SIZE = 256 * 1024;
constexpr uint64_t FOREVER = ~0ull;

const void* g_tls_image = nullptr;      // initial values of the thread-local variables
size_t g_tls_filesz = 0;                // bytes of them that are not zero-initialised
size_t g_tls_memsz = 0;
size_t g_tls_align = 1;                 // as the linker assumed when it computed the variables' offsets

inline size_t round_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }

inline __tcb* self_tcb() {
    __tcb* t;
    asm("movq %%fs:0, %0" : "=r"(t));
    return t;
}

__attribute__((noreturn)) void thread_start(__tcb* tcb) {
    tcb->result = tcb->fn(tcb->arg);
    syscall6(SYS_thread_exit, 0, 0, 0, 0, 0, 0);
    for (;;) asm volatile("ud2");
}

inline uint32_t cmpxchg(volatile uint32_t* p, uint32_t expect, uint32_t value) {
    __atomic_compare_exchange_n(p, &expect, value, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    return expect;      // what was there
}

} // namespace

extern "C" {

void __tls_locate(uint64_t base) {
    // The ELF header and program headers are part of the loaded image.
    const uint8_t* ehdr = (const uint8_t*)base;
    uint64_t phoff = *(const uint64_t*)(ehdr + 32);
    uint16_t phentsize = *(const uint16_t*)(ehdr + 54);
    uint16_t phnum = *(const uint16_t*)(ehdr + 56);
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t* ph = ehdr + phoff + (size_t)i * phentsize;
        if (*(const uint32_t*)ph != 7) continue;            // PT_TLS
        g_tls_image = (const void*)(base + *(const uint64_t*)(ph + 16));
        g_tls_filesz = *(const uint64_t*)(ph + 32);
        g_tls_memsz = *(const uint64_t*)(ph + 40);
        uint64_t align = *(const uint64_t*)(ph + 48);
        if (align > 1 && align <= PAGE) g_tls_align = align;
    }
}

// The variables end at the thread pointer and start round_up(size, align)
// below it: that is the layout the linker computed their offsets for. The
// control block is aligned to 16 bytes, or more if the variables need it.
static size_t tls_block_align(void) { return g_tls_align > 16 ? g_tls_align : 16; }
static size_t tls_block_size(void) { return round_up(round_up(g_tls_memsz, g_tls_align), tls_block_align()); }

size_t __tls_area_size(void) { return tls_block_size() + round_up(sizeof(__tcb), tls_block_align()); }

struct __tcb* __tls_init(void* area) {
    __tcb* tcb = (__tcb*)((char*)area + tls_block_size());
    if (g_tls_filesz) memcpy((char*)tcb - round_up(g_tls_memsz, g_tls_align), g_tls_image, g_tls_filesz);
    tcb->self = tcb;
    return tcb;
}

// ------------------------------------------------------------------ threads --

int pthread_create(pthread_t* out, const void* attr, void* (*fn)(void*), void* arg) {
    (void)attr;
    size_t tls = __tls_area_size();         // a multiple of its alignment, which divides the page size
    size_t size = round_up(PAGE + STACK_SIZE + tls, PAGE);
    long map = syscall6(SYS_mmap, 0, (long)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map < 0 && map > -4096) return (int)-map;
    // A hole below the stack, so running off its end faults instead of
    // writing into whatever is mapped there.
    syscall6(SYS_munmap, map, PAGE, 0, 0, 0, 0);

    char* area = (char*)map + size - tls;
    __tcb* tcb = __tls_init(area);
    tcb->map = (char*)map + PAGE;
    tcb->map_size = size - PAGE;
    tcb->fn = fn;
    tcb->arg = arg;
    // A function expects the stack as a call leaves it: 16-byte aligned
    // before the return address was pushed.
    uint64_t stack = ((uint64_t)area & ~0xFull) - 8;
    long tid = syscall6(SYS_thread_spawn, (long)thread_start, (long)tcb, (long)stack, (long)tcb, 0, 0);
    if (tid < 0) {
        syscall6(SYS_munmap, (long)tcb->map, (long)tcb->map_size, 0, 0, 0, 0);
        return (int)-tid;
    }
    tcb->tid = (int)tid;
    *out = tcb;
    return 0;
}

int pthread_join(pthread_t t, void** result) {
    long r;
    do r = syscall6(SYS_thread_join, t->tid, 0, 0, 0, 0, 0);
    while (r == -EINTR);
    if (r < 0) return (int)-r;
    if (result) *result = t->result;
    // The thread is gone from its stack for good once the join returns.
    syscall6(SYS_munmap, (long)t->map, (long)t->map_size, 0, 0, 0, 0);
    return 0;
}

pthread_t pthread_self(void) { return self_tcb(); }

__attribute__((noreturn)) void pthread_exit(void* result) {
    self_tcb()->result = result;
    syscall6(SYS_thread_exit, 0, 0, 0, 0, 0, 0);
    for (;;) asm volatile("ud2");
}

// -------------------------------------------------------------------- futex --

int futex_wait(uint32_t* addr, uint32_t val, uint64_t timeout_ms) {
    long r = syscall6(SYS_futex_wait, (long)addr, val, (long)timeout_ms, 0, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return 0;
}

int futex_wake(uint32_t* addr, int count) {
    long r = syscall6(SYS_futex_wake, (long)addr, count, 0, 0, 0, 0);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return (int)r;
}

// -------------------------------------------------------------------- mutex --
// state: 0 free, 1 locked, 2 locked and somebody may be waiting.

int pthread_mutex_init(pthread_mutex_t* m, const void* attr) {
    (void)attr;
    m->state = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t* m) {
    (void)m;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t* m) { return cmpxchg(&m->state, 0, 1) == 0 ? 0 : EBUSY; }

int pthread_mutex_lock(pthread_mutex_t* m) {
    uint32_t c = cmpxchg(&m->state, 0, 1);
    if (c == 0) return 0;
    // Contended: mark it so, and sleep while it stays that way. The raw
    // call is used so errno is left alone.
    if (c != 2) c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        syscall6(SYS_futex_wait, (long)&m->state, 2, (long)FOREVER, 0, 0, 0);
        c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t* m) {
    if (__atomic_fetch_sub(&m->state, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
        syscall6(SYS_futex_wake, (long)&m->state, 1, 0, 0, 0, 0);
    }
    return 0;
}

// ------------------------------------------------------ condition variables --

int pthread_cond_init(pthread_cond_t* c, const void* attr) {
    (void)attr;
    c->seq = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(m);
    // If a signal came between the unlock and here, seq has moved on and
    // the wait returns at once.
    syscall6(SYS_futex_wait, (long)&c->seq, seq, (long)FOREVER, 0, 0, 0);
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_signal(pthread_cond_t* c) {
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    syscall6(SYS_futex_wake, (long)&c->seq, 1, 0, 0, 0, 0);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t* c) {
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    syscall6(SYS_futex_wake, (long)&c->seq, 0x7FFFFFFF, 0, 0, 0, 0);
    return 0;
}

} // extern "C"
