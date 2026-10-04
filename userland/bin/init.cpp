// init: the first user process (pid 1). Its job for now is to exist, so
// that processes whose parent has exited have someone to collect them.
#include <cerberus.h>

int main(int, char**, char**) {
    for (;;) {
        int status = 0;
        pid_t child = waitpid(-1, &status, 0);
        if (child < 0) sleep_ms(500);       // no children at the moment
    }
}
