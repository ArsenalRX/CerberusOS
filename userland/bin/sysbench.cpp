// Makes N getpid system calls (N from the command line) and exits. The
// kernel's `bench` command times a run of 0 and a run of N to get the cost
// of one system-call round trip.
#include <cerberus.h>

int main(int argc, char** argv, char**) {
    unsigned long n = argc > 1 ? strtoul(argv[1], nullptr, 10) : 0;
    for (unsigned long i = 0; i < n; i++) syscall6(SYS_getpid, 0, 0, 0, 0, 0, 0);
    return 0;
}
