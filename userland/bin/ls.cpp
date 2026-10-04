// ls [-l] [-a] [path...]: lists directories (sorted) or describes files.
#include <cerberus.h>

namespace {

bool g_long = false, g_all = false;

void mode_string(uint32_t m, char* out) {
    out[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' : '-';
    const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[1 + i] = (m >> (8 - i)) & 1 ? rwx[i] : '-';
    out[10] = 0;
}

void show(const char* dir, const char* name) {
    if (!g_long) {
        printf("%s\n", name);
        return;
    }
    char path[512];
    if (dir) snprintf(path, sizeof path, "%s/%s", dir, name);
    else snprintf(path, sizeof path, "%s", name);
    struct stat st;
    if (lstat(path, &st) < 0) {
        printf("?????????? %s (%s)\n", name, strerror(errno));
        return;
    }
    char m[11];
    mode_string(st.st_mode, m);
    if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode))
        printf("%s %3u %4u %4u %4u,%4u %s", m, st.st_nlink, st.st_uid, st.st_gid, st.st_rdev >> 16,
               st.st_rdev & 0xFFFF, name);
    else
        printf("%s %3u %4u %4u %9lu %s", m, st.st_nlink, st.st_uid, st.st_gid, (unsigned long)st.st_size, name);
    if (S_ISLNK(st.st_mode)) {
        char target[256];
        long n = readlink(path, target, sizeof target - 1);
        if (n >= 0) {
            target[n] = 0;
            printf(" -> %s", target);
        }
    }
    printf("\n");
}

int list(const char* path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        dprintf(2, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        show(nullptr, path);
        return 0;
    }
    DIR* d = opendir(path);
    if (!d) {
        dprintf(2, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    // Collect, then sort names (insertion sort; directories are small).
    char** names = nullptr;
    int count = 0, cap = 0;
    while (struct dirent* e = readdir(d)) {
        if (!g_all && e->d_name[0] == '.') continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            names = (char**)realloc(names, (size_t)cap * sizeof(char*));
            if (!names) return 1;
        }
        size_t len = strlen(e->d_name) + 1;
        names[count] = (char*)malloc(len);
        memcpy(names[count++], e->d_name, len);
    }
    closedir(d);
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char* t = names[j];
            names[j] = names[j - 1];
            names[j - 1] = t;
        }
    for (int i = 0; i < count; i++) {
        show(path, names[i]);
        free(names[i]);
    }
    free(names);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++)
        for (const char* f = argv[first] + 1; *f; f++) {
            if (*f == 'l') g_long = true;
            else if (*f == 'a') g_all = true;
            else {
                dprintf(2, "usage: ls [-l] [-a] [path...]\n");
                return 2;
            }
        }
    if (first == argc) return list(".");
    int rc = 0;
    for (int i = first; i < argc; i++) {
        if (argc - first > 1) printf("%s:\n", argv[i]);
        rc |= list(argv[i]);
    }
    return rc;
}
