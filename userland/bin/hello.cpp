// The phase 7 "hello": output through write(1, ...), then an exit status the
// kernel reports back.
#include <cerberus.h>

// A table of pointers: these need fixing up for wherever the program was
// loaded, so printing through it checks the start-up relocation code.
static const char* const g_words[] = {"zero", "one", "two", "three"};

int main(int argc, char** argv, char**) {
    static const char direct[] = "hello: written with write(1, ...)\n";
    write(1, direct, sizeof direct - 1);
    volatile int index = argc & 3;
    printf("hello: user mode, pid %d, %d argument(s) (\"%s\"), program name %s\n", getpid(), argc, g_words[index],
           argv[0]);
    return 42;
}
