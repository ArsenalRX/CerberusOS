// Threads in one process (SPEC phase 11): creation and joining, a mutex
// that keeps a counter exact, thread-local variables and errno, condition
// variables, the allocator under contention, and how a process with several
// threads ends.
#include <cerberus.h>

static int fail(const char* what) {
    printf("threadtest: FAIL: %s (errno %d)\n", what, errno);
    return 1;
}

static const int THREADS = 8;
static const unsigned long ROUNDS = 100000;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned long g_counter;
static __thread unsigned long t_private = 42;      // initialised: comes from the program's template
static __thread unsigned long t_zero;               // zero-initialised
static volatile int g_tls_errors;

static void* count_up(void* arg) {
    unsigned long id = (unsigned long)arg;
    // Every thread starts with its own fresh copy.
    if (t_private != 42 || t_zero != 0) __atomic_fetch_add(&g_tls_errors, 1, __ATOMIC_RELAXED);
    t_private = id;
    errno = (int)(1000 + id);
    for (unsigned long i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&g_lock);
        g_counter = g_counter + 1;
        pthread_mutex_unlock(&g_lock);
        t_zero++;
    }
    if (t_private != id || t_zero != ROUNDS || errno != (int)(1000 + id))
        __atomic_fetch_add(&g_tls_errors, 1, __ATOMIC_RELAXED);
    return (void*)(id * 3);
}

// Many small allocations from every thread at once.
static void* churn(void* arg) {
    unsigned long seed = (unsigned long)arg * 2654435761u + 1;
    void* held[64] = {};
    for (int i = 0; i < 20000; i++) {
        seed = seed * 6364136223846793005ul + 1442695040888963407ul;
        unsigned slot = (seed >> 33) % 64;
        if (held[slot]) {
            unsigned char* p = (unsigned char*)held[slot];
            if (p[0] != (unsigned char)slot) return (void*)1;       // somebody else wrote here
            free(p);
            held[slot] = nullptr;
        } else {
            size_t n = 1 + (seed >> 40) % 300;
            unsigned char* p = (unsigned char*)malloc(n);
            if (!p) return (void*)2;
            memset(p, (int)slot, n);
            held[slot] = p;
        }
    }
    for (void* p : held) free(p);
    return nullptr;
}

// Ping-pong through a condition variable.
static pthread_mutex_t g_turn_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_turn_changed = PTHREAD_COND_INITIALIZER;
static int g_turn;
static int g_exchanges;

static void* ponger(void*) {
    pthread_mutex_lock(&g_turn_lock);
    for (int i = 0; i < 1000; i++) {
        while (g_turn != 1) pthread_cond_wait(&g_turn_changed, &g_turn_lock);
        g_turn = 0;
        g_exchanges++;
        pthread_cond_broadcast(&g_turn_changed);
    }
    pthread_mutex_unlock(&g_turn_lock);
    return nullptr;
}

static void* spin_forever(void*) {
    for (;;) asm volatile("pause");
    return nullptr;
}

static void* sleep_forever(void*) {
    uint32_t word = 0;
    for (;;) futex_wait(&word, 0, WAIT_FOREVER);
    return nullptr;
}

static void* crash(void*) {
    volatile int* volatile bad = (volatile int*)8;
    *bad = 1;
    return nullptr;
}

// Runs `body` in a child process and returns its wait status.
static int in_child(int (*body)()) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) exit(body());
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return status;
}

static int main_returns_with_threads_running() {
    pthread_t t;
    for (int i = 0; i < 3; i++)
        if (pthread_create(&t, nullptr, i == 0 ? sleep_forever : spin_forever, nullptr) != 0) return 90;
    sleep_ms(30);
    return 5;       // exit(): the busy and the sleeping threads go with the process
}

static int thread_faults() {
    pthread_t t;
    if (pthread_create(&t, nullptr, crash, nullptr) != 0) return 90;
    pthread_join(t, nullptr);
    return 91;      // not reached: the fault ends the whole process
}

static int last_thread_exits() {
    pthread_t t;
    if (pthread_create(&t, nullptr, [](void*) -> void* {
            sleep_ms(30);
            exit(6);
        }, nullptr) != 0)
        return 90;
    pthread_exit(nullptr);      // the first thread leaves; the process lives on until the other ends it
}

int main(int, char**, char**) {
    // 1. Counting under a mutex, thread-local state, results through join.
    pthread_t threads[THREADS];
    uint64_t start = time_ms();
    for (long i = 0; i < THREADS; i++)
        if (pthread_create(&threads[i], nullptr, count_up, (void*)(i + 1)) != 0) return fail("pthread_create");
    for (long i = 0; i < THREADS; i++) {
        void* result = nullptr;
        if (pthread_join(threads[i], &result) != 0) return fail("pthread_join");
        if ((unsigned long)result != (unsigned long)(i + 1) * 3) return fail("a thread's result");
    }
    if (g_counter != THREADS * ROUNDS) {
        printf("threadtest: FAIL: the counter is %lu, expected %lu\n", g_counter, THREADS * ROUNDS);
        return 1;
    }
    if (g_tls_errors) return fail("thread-local variables were shared or lost");
    if (t_private != 42) return fail("the first thread's own variable changed");
    printf("threadtest: %d threads added %lu each under a mutex: exactly %lu (%lu ms)\n", THREADS, ROUNDS, g_counter,
           (unsigned long)(time_ms() - start));
    printf("threadtest: every thread kept its own thread-local variables and errno\n");

    // 2. The allocator from every thread at once.
    for (long i = 0; i < THREADS; i++)
        if (pthread_create(&threads[i], nullptr, churn, (void*)(i + 1)) != 0) return fail("pthread_create");
    for (long i = 0; i < THREADS; i++) {
        void* result = nullptr;
        if (pthread_join(threads[i], &result) != 0 || result) return fail("the allocator under contention");
    }
    printf("threadtest: %d threads allocated and freed at once without stepping on each other\n", THREADS);

    // 3. A condition variable.
    pthread_t other;
    if (pthread_create(&other, nullptr, ponger, nullptr) != 0) return fail("pthread_create");
    pthread_mutex_lock(&g_turn_lock);
    for (int i = 0; i < 1000; i++) {
        g_turn = 1;
        pthread_cond_broadcast(&g_turn_changed);
        while (g_turn != 0) pthread_cond_wait(&g_turn_changed, &g_turn_lock);
    }
    pthread_mutex_unlock(&g_turn_lock);
    pthread_join(other, nullptr);
    if (g_exchanges != 1000) return fail("condition variable exchanges");
    printf("threadtest: two threads took 1000 turns through a condition variable\n");

    // 4. How a process with several threads ends.
    int status = in_child(main_returns_with_threads_running);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 5) return fail("exit with other threads running");
    status = in_child(thread_faults);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) return fail("a fault in one thread");
    status = in_child(last_thread_exits);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 6) return fail("the first thread leaving early");
    printf("threadtest: exit and a fault each ended every thread of their process\n");

    // 5. A forked child has only the forking thread, and its thread-local state.
    t_private = 77;
    status = in_child([]() -> int { return t_private == 77 ? 9 : 1; });
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 9) return fail("thread-local state across fork");

    printf("threadtest: PASS\n");
    return 0;
}
