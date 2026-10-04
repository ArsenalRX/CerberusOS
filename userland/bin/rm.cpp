// rm [-r] [-f] path...: removes files (-r: directories and everything in
// them; -f: no error for missing files).
#include <lumen.h>

namespace {

bool g_recursive = false, g_force = false;

int remove_path(const char* path) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (g_force && errno == ENOENT) return 0;
        dprintf(2, "rm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!g_recursive) {
            dprintf(2, "rm: %s: is a directory (use -r)\n", path);
            return 1;
        }
        int rc = 0;
        // Removing while reading would shift the listing: restart until empty.
        for (bool again = true; again;) {
            again = false;
            DIR* d = opendir(path);
            if (!d) {
                dprintf(2, "rm: %s: %s\n", path, strerror(errno));
                return 1;
            }
            while (struct dirent* e = readdir(d)) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                char child[512];
                snprintf(child, sizeof child, "%s/%s", path, e->d_name);
                if (remove_path(child)) rc = 1;
                else again = true;
                break;
            }
            closedir(d);
            if (rc) return rc;
        }
        if (rmdir(path) < 0) {
            dprintf(2, "rm: %s: %s\n", path, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (unlink(path) < 0) {
        dprintf(2, "rm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; first++)
        for (const char* f = argv[first] + 1; *f; f++) {
            if (*f == 'r' || *f == 'R') g_recursive = true;
            else if (*f == 'f') g_force = true;
        }
    if (first == argc) {
        dprintf(2, "usage: rm [-r] [-f] path...\n");
        return 2;
    }
    int rc = 0;
    for (int i = first; i < argc; i++) rc |= remove_path(argv[i]);
    return rc;
}
