// Address-space layout randomisation: prints where its stack, heap mapping
// and code are, then runs a second copy of itself and checks that all three
// moved.
#include <lumen.h>

int main(int argc, char** argv, char**) {
    int on_stack = 0;
    void* map = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned long stack = (unsigned long)&on_stack, mapped = (unsigned long)map, code = (unsigned long)&main;
    printf("aslr: pid %d: stack %#lx, mmap %#lx, code %#lx\n", getpid(), stack, mapped, code);

    if (argc == 4) {                    // second run: compare with the first run's addresses
        unsigned long s = strtoul(argv[1], nullptr, 16), m = strtoul(argv[2], nullptr, 16), c = strtoul(argv[3], nullptr, 16);
        bool differ = s != stack && m != mapped && c != code;
        printf("aslr: %s\n", differ ? "all three differ from the previous run: PASS"
                                    : "FAIL: an address repeated");
        return differ ? 0 : 1;
    }

    char a[32], b[32], c[32];
    snprintf(a, sizeof a, "%lx", stack);
    snprintf(b, sizeof b, "%lx", mapped);
    snprintf(c, sizeof c, "%lx", code);
    pid_t pid = fork();
    if (pid == 0) {
        char* args[] = {(char*)"aslr", a, b, c, nullptr};
        execve("/bin/aslr", args, nullptr);
        exit(98);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
