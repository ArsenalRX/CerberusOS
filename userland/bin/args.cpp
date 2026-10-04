// Prints its arguments and environment; exits with the argument count.
// forktest runs it through execve.
#include <cerberus.h>

int main(int argc, char** argv, char** envp) {
    for (int i = 0; i < argc; i++) printf("args: argv[%d] = %s\n", i, argv[i]);
    for (int i = 0; envp[i]; i++) printf("args: env %s\n", envp[i]);
    return argc;
}
