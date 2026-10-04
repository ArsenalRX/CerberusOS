// cat [file...]: copies files (or nothing, without arguments) to the output.
#include <cerberus.h>

int main(int argc, char** argv) {
    int rc = 0;
    static char buf[16384];
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(2, "cat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        for (;;) {
            long n = read(fd, buf, sizeof buf);
            if (n < 0) {
                dprintf(2, "cat: %s: %s\n", argv[i], strerror(errno));
                rc = 1;
                break;
            }
            if (n == 0) break;
            for (long done = 0; done < n;) {
                long w = write(1, buf + done, (size_t)(n - done));
                if (w <= 0) return 1;
                done += w;
            }
        }
        close(fd);
    }
    return rc;
}
