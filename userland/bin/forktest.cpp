// fork + execve + waitpid, copy-on-write separation, and FPU state kept
// apart between processes.
#include <lumen.h>

static int g_value = 1;

static int fail(const char* what) {
    printf("forktest: FAIL: %s (errno %d)\n", what, errno);
    return 1;
}

// Loads `value` into xmm15, makes a yield system call with the value held
// only in that register, and reads it back. The compiler cannot keep
// anything else there because the register is declared clobbered.
static bool sse_survives_yield(unsigned long value) {
    unsigned long back = 0;
    long nr = SYS_yield;
    asm volatile("movq %[in], %%xmm15\n"
                 "syscall\n"
                 "movq %%xmm15, %[out]\n"
                 : [out] "=&r"(back), "+a"(nr)
                 : [in] "r"(value)
                 : "rcx", "r11", "xmm15", "memory");
    return back == value;
}

int main(int, char**, char**) {
    // 1. A child's writes are not seen by the parent, and its exit code arrives.
    pid_t pid = fork();
    if (pid < 0) return fail("fork");
    if (pid == 0) {
        g_value = 2;
        exit(g_value == 2 ? 7 : 99);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return fail("waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 7) return fail("child exit status");
    if (g_value != 1) return fail("child's write leaked into the parent");
    printf("forktest: child %d changed its copy and exited with 7; the parent's copy is unchanged\n", pid);

    // 2. fork, then the child replaces itself with another program.
    pid = fork();
    if (pid < 0) return fail("fork");
    if (pid == 0) {
        char* argv[] = {(char*)"args", (char*)"one", (char*)"two", nullptr};
        char* envp[] = {(char*)"GREETING=hi", nullptr};
        execve("/bin/args", argv, envp);
        exit(98);       // only reached if execve failed
    }
    if (waitpid(pid, &status, 0) != pid) return fail("waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 3) return fail("exec'd child exit status");
    printf("forktest: child %d ran /bin/args through execve and exited with 3\n", pid);

    // 3. execve of something that is not there fails and leaves the caller running.
    char* none[] = {(char*)"x", nullptr};
    if (execve("/bin/does-not-exist", none, nullptr) != -1 || errno != ENOENT) return fail("execve of a missing file");

    // 4. SSE registers belong to each process: two processes each park a
    // different value in a register, give up the CPU to the other, and
    // check that the value is still theirs.
    pid = fork();
    if (pid < 0) return fail("fork");
    unsigned long mine = pid == 0 ? 0x1111222233334444ul : 0xAAAABBBBCCCCDDDDul;
    bool ok = true;
    for (int i = 0; i < 2000; i++) ok &= sse_survives_yield(mine + (unsigned long)i);
    if (pid == 0) exit(ok ? 0 : 1);
    if (waitpid(pid, &status, 0) != pid) return fail("waitpid");
    if (!ok || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return fail("floating-point state mixed between processes");
    printf("forktest: floating-point state stayed separate across 4000 switches\n");

    // 5. No child left: waitpid says so.
    if (waitpid(-1, &status, 0) != -1 || errno != ECHILD) return fail("waitpid with no children");
    printf("forktest: PASS\n");
    return 0;
}
