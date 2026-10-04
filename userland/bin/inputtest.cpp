// inputtest [kbd|mouse] [count]: prints events read from /dev/input/kbd0 (or
// mouse0) as they arrive, then exits after `count` events (default 4).
// Phase 10 test tool; root only, like the devices themselves.
#include <cerberus.h>

int main(int argc, char** argv) {
    bool mouse = argc > 1 && !strcmp(argv[1], "mouse");
    unsigned long count = argc > 2 ? strtoul(argv[2], nullptr, 10) : 4;
    const char* path = mouse ? "/dev/input/mouse0" : "/dev/input/kbd0";
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(2, "inputtest: %s: %s\n", path, strerror(errno));
        return 1;
    }
    printf("inputtest: reading %lu event(s) from %s\n", count, path);
    for (unsigned long seen = 0; seen < count;) {
        struct input_event ev[8];
        long n = read(fd, ev, sizeof ev);
        if (n < (long)sizeof ev[0]) {
            dprintf(2, "inputtest: read: %s\n", n < 0 ? strerror(errno) : "short read");
            return 1;
        }
        for (long i = 0; i < n / (long)sizeof ev[0] && seen < count; i++, seen++) {
            const struct input_event& e = ev[i];
            if (e.type == EV_KEY) {
                char c = e.unicode >= 0x20 && e.unicode < 0x7F ? (char)e.unicode : '.';
                printf("inputtest: key %#x %s '%c' mods %#x\n", e.code,
                       e.value == 1 ? "press" : e.value == 2 ? "repeat" : "release", c, e.mods);
            } else if (e.type == EV_REL) {
                printf("inputtest: move %s %d\n", e.code == REL_X ? "x" : e.code == REL_Y ? "y" : "wheel", e.value);
            } else if (e.type == EV_BUTTON) {
                printf("inputtest: button %u %s\n", e.code, e.value ? "press" : "release");
            }
        }
    }
    close(fd);
    printf("inputtest: done\n");
    return 0;
}
