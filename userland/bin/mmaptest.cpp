// Mapping a file into memory, and mprotect (SPEC phase 11, moved from
// phase 9): a private mapping holds the file's bytes from a page-aligned
// offset, zeros past the end, and writes to it never reach the file.
#include <cerberus.h>

static int fail(const char* what) {
    printf("mmaptest: FAIL: %s (errno %d: %s)\n", what, errno, strerror(errno));
    return 1;
}

static const size_t PAGE = 4096;
static const size_t FILE_SIZE = 3 * PAGE + 100;
static unsigned char byte_at(size_t i) { return (unsigned char)(i * 7 + (i >> 8) + 1); }

// Runs `body` in a child and says whether a fault (SIGSEGV) ended it.
static bool faults(void (*body)(void*), void* arg) {
    pid_t pid = fork();
    if (pid == 0) {
        body(arg);
        exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

int main(int, char**, char**) {
    const char* path = "/tmp/mmaptest.bin";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return fail("creating the file");
    static unsigned char data[FILE_SIZE];
    for (size_t i = 0; i < FILE_SIZE; i++) data[i] = byte_at(i);
    if (write(fd, data, FILE_SIZE) != (long)FILE_SIZE) return fail("writing the file");

    // 1. Read-only, from the second page on, one page longer than the file.
    size_t len = 4 * PAGE;
    unsigned char* ro = (unsigned char*)mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, PAGE);
    if (ro == MAP_FAILED) return fail("mmap of the file");
    for (size_t i = 0; i < len; i++) {
        unsigned char want = PAGE + i < FILE_SIZE ? byte_at(PAGE + i) : 0;
        if (ro[i] != want) return fail("the mapping does not hold the file's bytes");
    }
    if (!faults([](void* p) { *(volatile unsigned char*)p = 1; }, ro)) return fail("a read-only mapping was writable");
    printf("mmaptest: a read-only mapping holds the file from its offset, zero past the end\n");

    // 2. Writable and private: the file does not change.
    unsigned char* rw = (unsigned char*)mmap(nullptr, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (rw == MAP_FAILED) return fail("a writable mapping");
    for (size_t i = 0; i < FILE_SIZE; i++) rw[i] ^= 0xFF;
    static unsigned char back[FILE_SIZE];
    if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, back, FILE_SIZE) != (long)FILE_SIZE) return fail("reading back");
    if (memcmp(back, data, FILE_SIZE) != 0) return fail("writes to a private mapping reached the file");
    if (rw[10] != (unsigned char)(byte_at(10) ^ 0xFF)) return fail("the mapping lost a write");
    printf("mmaptest: writes to a private mapping stayed out of the file\n");

    // 3. mprotect takes write access away and gives it back.
    if (mprotect(rw, PAGE, PROT_READ) != 0) return fail("mprotect to read-only");
    if (!faults([](void* p) { *(volatile unsigned char*)p = 1; }, rw)) return fail("mprotect did not remove write access");
    if (rw[PAGE] != (unsigned char)(byte_at(PAGE) ^ 0xFF)) return fail("the next page changed");
    rw[PAGE] = 5;       // the second page is still writable
    if (mprotect(rw, PAGE, PROT_READ | PROT_WRITE) != 0) return fail("mprotect back to writable");
    rw[0] = 9;
    if (mprotect(rw, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC) != -1 || errno != EINVAL)
        return fail("writable and executable together was allowed");
    printf("mmaptest: mprotect removed and restored write access; W+X is refused\n");

    // 4. What is refused.
    if (mmap(nullptr, PAGE, PROT_READ, MAP_PRIVATE, fd, 100) != MAP_FAILED || errno != EINVAL) return fail("an unaligned offset");
    if (mmap(nullptr, PAGE, PROT_READ, MAP_PRIVATE, 30, 0) != MAP_FAILED || errno != EBADF) return fail("a closed descriptor");
    if (mmap(nullptr, PAGE, PROT_READ, 0x01, fd, 0) != MAP_FAILED || errno != EINVAL) return fail("a shared file mapping");
    int dir = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (mmap(nullptr, PAGE, PROT_READ, MAP_PRIVATE, dir, 0) != MAP_FAILED || errno != ENODEV) return fail("mapping a directory");
    close(dir);
    // The file is mode 0600 with no execute bit; /tmp is mounted noexec as well.
    if (mmap(nullptr, PAGE, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0) != MAP_FAILED || errno != EACCES)
        return fail("an executable mapping of a file that may not be executed");
    int wo = open(path, O_WRONLY);
    if (mmap(nullptr, PAGE, PROT_READ, MAP_PRIVATE, wo, 0) != MAP_FAILED || errno != EACCES) return fail("mapping a write-only descriptor");
    close(wo);

    // The mapping outlives the descriptor and the name.
    close(fd);
    unlink(path);
    if (ro[5] != byte_at(PAGE + 5)) return fail("the mapping went with the file");
    if (munmap(ro, len) != 0 || munmap(rw, (FILE_SIZE + PAGE - 1) & ~(PAGE - 1)) != 0) return fail("munmap");
    printf("mmaptest: PASS\n");
    return 0;
}
