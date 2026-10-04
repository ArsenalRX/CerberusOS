// Signals (SPEC phase 11): handlers and what they interrupt, the default
// actions, faults as signals, and who may signal whom.
#include <cerberus.h>

static int fail(const char* what) {
    printf("sigtest: FAIL: %s (errno %d)\n", what, errno);
    return 1;
}

static volatile int g_usr1, g_usr2, g_chld;
static void on_usr1(int sig) {
    if (sig == SIGUSR1) g_usr1 = g_usr1 + 1;
}
static void on_usr2(int sig) {
    if (sig == SIGUSR2) g_usr2 = g_usr2 + 1;
}
static void on_chld(int sig) {
    if (sig == SIGCHLD) g_chld = g_chld + 1;
}
static void on_segv(int sig) {
    // Returning would run the faulting instruction again; a handler for a
    // fault reports and leaves.
    exit(sig == SIGSEGV ? 42 : 43);
}

static int status_of(pid_t pid) {
    int status = -1;
    while (waitpid(pid, &status, 0) != pid)
        if (errno != EINTR) return -1;
    return status;
}

// Sums with every general register and a vector register in use while
// signals arrive; a handler that disturbed any of them changes the result.
// (noipa: the compiler must not notice that two calls give the same answer
// and make only one.)
__attribute__((noipa)) static unsigned long busy_sum(unsigned long rounds) {
    unsigned long a = 1, b = 2, c = 3, d = 5, e = 7, f = 11;
    double x = 1.0;
    for (unsigned long i = 0; i < rounds; i++) {
        a += b ^ i;
        b += c + (a >> 3);
        c ^= d * 31;
        d += e + i;
        e ^= f + a;
        f += a ^ c;
        x = x * 1.0000001 + 0.5;
    }
    return a ^ b ^ c ^ d ^ e ^ f ^ (unsigned long)x;
}

int main(int, char**, char**) {
    // 1. A handler runs, and the program carries on where it was.
    if (signal(SIGUSR1, on_usr1) != SIG_DFL) return fail("signal");
    if (raise(SIGUSR1) != 0) return fail("raise");
    if (g_usr1 != 1) return fail("the handler did not run before kill returned");
    if (signal(SIGUSR1, on_usr1) != on_usr1) return fail("signal did not return the previous handler");
    if (signal(SIGKILL, on_usr1) != SIG_ERR || errno != EINVAL) return fail("SIGKILL can be handled");
    printf("sigtest: a handler ran and returned\n");

    // 2. Registers survive handlers: the same sum with and without signals.
    const unsigned long ROUNDS = 60000000;
    unsigned long quiet = busy_sum(ROUNDS);
    signal(SIGUSR2, on_usr2);
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) return fail("fork");
    if (pid == 0) {
        for (int i = 0; i < 200; i++) {
            kill(parent, i % 2 ? SIGUSR1 : SIGUSR2);
            sleep_ms(1);
        }
        exit(0);
    }
    g_usr1 = 0;
    g_usr2 = 0;
    unsigned long noisy = busy_sum(ROUNDS);
    int taken = g_usr1 + g_usr2;
    if (status_of(pid) != 0) return fail("the signalling child");
    if (noisy != quiet) return fail("a handler disturbed the interrupted computation");
    if (taken == 0) return fail("no signal arrived during the computation");
    printf("sigtest: a computation interrupted by handlers gave the same result\n");

    // 3. A waiting system call returns EINTR.
    pid = fork();
    if (pid < 0) return fail("fork");
    if (pid == 0) {
        sleep_ms(50);
        kill(parent, SIGUSR1);
        exit(0);
    }
    g_usr1 = 0;
    uint64_t start = time_ms();
    int r = sleep_ms(5000);
    uint64_t waited = time_ms() - start;
    if (r != -1 || errno != EINTR || g_usr1 != 1) return fail("sleep was not interrupted");
    if (waited > 2000) return fail("the interrupted sleep ran on");
    if (status_of(pid) != 0) return fail("the child");
    printf("sigtest: a sleep of 5 s was interrupted by a signal and returned EINTR\n");

    // 4. Default actions and SIGKILL.
    pid = fork();
    if (pid == 0) {
        for (;;) sleep_ms(1000);
    }
    sleep_ms(20);
    if (kill(pid, SIGTERM) != 0) return fail("kill");
    int status = status_of(pid);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) return fail("SIGTERM's default action");

    pid = fork();
    if (pid == 0) {
        signal(SIGTERM, SIG_IGN);
        signal(SIGUSR1, SIG_IGN);
        for (;;) sleep_ms(1000);
    }
    sleep_ms(20);
    kill(pid, SIGTERM);
    kill(pid, SIGUSR1);
    sleep_ms(50);
    if (waitpid(pid, &status, WNOHANG) != 0) return fail("an ignored signal ended the process");
    if (kill(pid, SIGKILL) != 0) return fail("kill");
    status = status_of(pid);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) return fail("SIGKILL");
    printf("sigtest: SIGTERM ended a process, was ignored on request; SIGKILL cannot be ignored\n");

    // 5. A fault is a signal too.
    pid = fork();
    if (pid == 0) {
        signal(SIGSEGV, on_segv);
        volatile int* volatile bad = (volatile int*)16;
        *bad = 1;
        exit(1);
    }
    status = status_of(pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) return fail("the SIGSEGV handler");
    printf("sigtest: a SIGSEGV handler caught a bad pointer\n");

    // 6. SIGCHLD when a child ends.
    signal(SIGCHLD, on_chld);
    g_chld = 0;
    pid = fork();
    if (pid == 0) exit(0);
    if (status_of(pid) != 0) return fail("the child");
    if (g_chld != 1) return fail("SIGCHLD was not delivered");
    signal(SIGCHLD, SIG_DFL);
    printf("sigtest: SIGCHLD arrived when a child exited\n");

    // 7. Only the same user (or root) may signal a process.
    pid = fork();
    if (pid == 0) {
        if (setgid(1000) != 0 || setuid(1000) != 0) exit(1);
        if (kill(parent, SIGTERM) != -1 || errno != EPERM) exit(2);
        if (kill(1, 0) != -1 || errno != EPERM) exit(3);
        if (kill(getpid(), 0) != 0) exit(4);
        exit(0);
    }
    if (status_of(pid) != 0) return fail("an unprivileged process signalled root's");
    if (kill(99999, SIGTERM) != -1 || errno != ESRCH) return fail("kill of a missing process");
    printf("sigtest: uid 1000 could not signal a root process\n");

    // 8. A signal ends a process whose threads are all asleep.
    pid = fork();
    if (pid == 0) {
        pthread_t t;
        for (int i = 0; i < 3; i++)
            pthread_create(&t, nullptr, [](void*) -> void* {
                uint32_t word = 0;
                for (;;) futex_wait(&word, 0, WAIT_FOREVER);
            }, nullptr);
        for (;;) sleep_ms(1000);
    }
    sleep_ms(50);
    start = time_ms();
    kill(pid, SIGTERM);
    status = status_of(pid);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) return fail("ending a sleeping multi-threaded process");
    if (time_ms() - start > 1000) return fail("ending it took too long");
    printf("sigtest: SIGTERM ended a process with four sleeping threads\n");

    printf("sigtest: PASS\n");
    return 0;
}
