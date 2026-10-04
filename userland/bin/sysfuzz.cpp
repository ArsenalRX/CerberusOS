// Random system calls with hostile arguments (SPEC §19.11: the in-kernel
// part of `make fuzz`). The parent forks children; each child makes `calls`
// system calls with numbers and arguments drawn from values chosen to hurt:
// kernel addresses, non-canonical addresses, wrapping lengths, real buffers
// with absurd sizes, unknown flags. A child may fail every call, unmap its
// own stack, or be killed; none of that matters. What matters is that the
// kernel survives and the parent can still collect every child.
//
//   sysfuzz [children [calls [seed]]]
//
// The seed is printed; passing it back repeats the run exactly.
//
// Left out on purpose: fork (the children would multiply; forktest covers
// it) and long sleeps (the run would never end).
#include <cerberus.h>

static unsigned long g_rng;
static unsigned long rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}
static unsigned long below(unsigned long n) { return rnd() % n; }

static const long CALLS[] = {
    SYS_write,  SYS_read,   SYS_open,     SYS_close,    SYS_mmap,   SYS_munmap,  SYS_execve, SYS_waitpid,
    SYS_getpid, SYS_yield,  SYS_sleep_ms, SYS_getrandom,
    // Files (phase 9). The paths offered are in the read-only root or under /tmp.
    SYS_openat, SYS_seek,   SYS_stat,     SYS_lstat,    SYS_fstat,  SYS_mkdir,   SYS_rmdir,  SYS_unlink,
    SYS_rename, SYS_readdir, SYS_chdir,   SYS_getcwd,   SYS_dup,    SYS_dup2,    SYS_ioctl,  SYS_truncate,
    SYS_symlink, SYS_readlink, SYS_chmod, SYS_fsync,    SYS_mount,  SYS_umount,
};
static const char* const PATHS[] = {"/bin/hello", "/tmp/fz", "/tmp/fz/a", "/tmp", "/", "/dev/null", "tmpfs", "."};

static const unsigned long HOSTILE[] = {
    0,                      1,                      2,                      3,
    (unsigned long)-1,      0x10,                   0x1000,                 0x10000,
    0x12345000,             0x7FFFFFFF,             0x80000000,             0xFFFFFFFF,
    0x00007FFFFFFFF000,     0x00007FFFFFFFFFF8,     0x00007FFFFFFFFFFF,     0x0000800000000000,
    0x8000000000000000,     0xFFFF800000000000,     0xFFFF800000100000,     0xFFFFFFFF80000000,
    0xFFFFFFFF80001000,     0xFFFFFFFFFFFFF000,     ~0ul - 8,               0x7FFFFFFFFFFFFFFF,
    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, 4096 * 16,
};

static const size_t SCRATCH = 16 * 4096;
static unsigned char* g_scratch;        // real, writable memory to aim calls at
static char* g_argv[4];

static unsigned long argument() {
    switch (below(8)) {
    case 0:
    case 1:
    case 2: return HOSTILE[below(sizeof HOSTILE / sizeof HOSTILE[0])];
    case 3: return (unsigned long)g_scratch + below(SCRATCH + 4096);                // in or just past real memory
    case 4: return (unsigned long)g_scratch + (below(17) << 12);                    // page-aligned, same
    case 5:                                                                         // a real path, a real argv
        return below(4) ? (unsigned long)PATHS[below(sizeof PATHS / sizeof PATHS[0])] : (unsigned long)g_argv;
    case 6: return below(64);
    default: return rnd();
    }
}

static void child(unsigned long calls) {
    // No console in the children: random writes would fill the screen.
    close(0);
    close(1);
    close(2);
    for (unsigned long i = 0; i < calls; i++) {
        long nr = below(2) ? CALLS[below(sizeof CALLS / sizeof CALLS[0])] : (long)(below(4) ? below(256) : rnd());
        if (nr == SYS_exit || nr == SYS_fork) continue;
        unsigned long a[6];
        for (unsigned long& v : a) v = argument();
        // The fuzzer runs as root: a valid reboot request would end the test
        // (and the machine). Invalid ones are still tried.
        if (nr == SYS_reboot && (a[0] == 1 || a[0] == 2)) continue;
        // A sleep that would be accepted is kept to one tick, and that rarely.
        if (nr == SYS_sleep_ms && a[0] <= (1ul << 31)) a[0] = below(16) == 0;
        // Unmapping most of the address space takes the child's own code
        // with it and ends the run early, so most huge lengths are cut down.
        if (nr == SYS_munmap && a[1] > (1ul << 30) && below(8)) a[1] &= 0xFFFFF;
        syscall6(nr, (long)a[0], (long)a[1], (long)a[2], (long)a[3], (long)a[4], (long)a[5]);
    }
    exit(0);
}

int main(int argc, char** argv, char**) {
    unsigned long children = argc > 1 ? strtoul(argv[1], nullptr, 10) : 40;
    unsigned long calls = argc > 2 ? strtoul(argv[2], nullptr, 10) : 500;
    unsigned long seed = argc > 3 ? strtoul(argv[3], nullptr, 10) : 0;
    while (!seed) getrandom(&seed, sizeof seed, 0);
    printf("sysfuzz: %lu children, %lu calls each, seed %lu\n", children, calls, seed);

    g_scratch = (unsigned char*)mmap(nullptr, SCRATCH, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_scratch == MAP_FAILED) {
        printf("sysfuzz: FAIL: no scratch memory\n");
        return 1;
    }
    memset(g_scratch, 'A', SCRATCH);
    g_argv[0] = (char*)"hello";
    g_argv[1] = (char*)g_scratch;       // a 64 KiB string with no terminator inside the mapping
    g_argv[2] = (char*)"x";

    pid_t me = getpid();
    unsigned long exited = 0, killed = 0;
    for (unsigned long c = 0; c < children; c++) {
        g_rng = seed + c * 0x9E3779B97F4A7C15ul;
        if (!g_rng) g_rng = 1;
        pid_t pid = fork();
        if (pid < 0) {
            printf("sysfuzz: FAIL: fork failed for child %lu (errno %d)\n", c, errno);
            return 1;
        }
        if (pid == 0) child(calls);
        int status = 0;
        if (waitpid(pid, &status, 0) != pid) {
            printf("sysfuzz: FAIL: waitpid for child %lu (errno %d)\n", c, errno);
            return 1;
        }
        if (WIFSIGNALED(status)) killed++;
        else exited++;
    }

    // The parent is intact: same pid, memory untouched, no stray children.
    bool intact = getpid() == me;
    for (size_t i = 0; i < SCRATCH; i++) intact &= g_scratch[i] == 'A';
    int status;
    intact &= waitpid(-1, &status, 0) == -1 && errno == ECHILD;
    if (!intact) {
        printf("sysfuzz: FAIL: the parent was disturbed by its children\n");
        return 1;
    }
    printf("sysfuzz: PASS: %lu children ran up to %lu random system calls (%lu exited, %lu were killed); "
           "the kernel is still running\n",
           children, children * calls, exited, killed);
    return 0;
}
