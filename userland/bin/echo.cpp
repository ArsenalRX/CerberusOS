// echo [-n] [word...]: prints its arguments separated by spaces.
#include <cerberus.h>

int main(int argc, char** argv) {
    int first = 1;
    bool newline = true;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = false;
        first = 2;
    }
    for (int i = first; i < argc; i++) {
        if (i > first) write(1, " ", 1);
        write(1, argv[i], strlen(argv[i]));
    }
    if (newline) write(1, "\n", 1);
    return 0;
}
