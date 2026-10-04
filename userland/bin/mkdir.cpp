// mkdir [-p] dir...: creates directories (-p: with missing parents, no
// error if one exists).
#include <lumen.h>

namespace {

int make_parents(const char* path) {
    char buf[256];
    size_t len = strlcpy(buf, path, sizeof buf);
    if (len >= sizeof buf) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] != '/' && buf[i] != 0) continue;
        char saved = buf[i];
        buf[i] = 0;
        if (mkdir(buf, 0777) < 0 && errno != EEXIST) return -1;
        buf[i] = saved;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool parents = argc > 1 && strcmp(argv[1], "-p") == 0;
    int first = parents ? 2 : 1;
    if (first >= argc) {
        dprintf(2, "usage: mkdir [-p] dir...\n");
        return 2;
    }
    int rc = 0;
    for (int i = first; i < argc; i++) {
        int r = parents ? make_parents(argv[i]) : mkdir(argv[i], 0777);
        if (r < 0) {
            dprintf(2, "mkdir: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
