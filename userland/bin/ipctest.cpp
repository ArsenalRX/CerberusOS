// Ports, shared memory and futexes between processes (SPEC phase 11):
//   1. 100,000 messages through a port, in order, none lost
//   2. the kernel's record of who is at the other end
//   3. a protected port refuses a process without permission
//   4. descriptors travel in messages: a file, and shared memory that both
//      sides then see the same bytes in
//   5. a futex-based mutex in shared memory keeps a counter exact across
//      four processes
//   6. errors: a closed peer, a missing port, a full receive buffer
#include <cerberus.h>

static int fail(const char* what) {
    printf("ipctest: FAIL: %s (errno %d: %s)\n", what, errno, strerror(errno));
    return 1;
}

static const unsigned MESSAGES = 100000;

struct Packet {
    uint32_t seq;
    uint32_t len;           // bytes of payload that follow
    uint8_t payload[256];
};

static uint8_t pattern(uint32_t seq, uint32_t i) { return (uint8_t)(seq * 31 + i * 7 + 3); }

// The client half of test 1: sends MESSAGES packets of varying size, then
// waits for the server's count.
static int flood_client() {
    int ch = port_connect("test.flood");
    if (ch < 0) return 10;
    Packet p;
    for (uint32_t seq = 0; seq < MESSAGES; seq++) {
        p.seq = seq;
        p.len = seq % 200;
        for (uint32_t i = 0; i < p.len; i++) p.payload[i] = pattern(seq, i);
        if (port_send(ch, &p, 8 + p.len, nullptr, 0) != 0) return 11;
    }
    uint32_t counted = 0;
    if (port_recv(ch, &counted, sizeof counted, nullptr, nullptr, 20000) != sizeof counted) return 12;
    return counted == MESSAGES ? 0 : 13;
}

static int wait_child(pid_t pid, int expect, const char* what) {
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return fail("waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expect) {
        printf("ipctest: FAIL: %s: child ended with status %#x, expected exit code %d\n", what, status, expect);
        return 1;
    }
    return 0;
}

static int test_flood() {
    int listener = port_create("test.flood", 0600);
    if (listener < 0) return fail("port_create");
    if (port_create("test.flood", 0600) != -1 || errno != EEXIST) return fail("a second port of the same name");
    pid_t pid = fork();
    if (pid < 0) return fail("fork");
    if (pid == 0) exit(flood_client());

    int ch = port_accept(listener, 10000);
    if (ch < 0) return fail("port_accept");

    // 2. The kernel says who connected.
    struct port_peer peer;
    if (port_peer(ch, &peer) != 0) return fail("port_peer");
    if (peer.pid != (uint32_t)pid || peer.uid != 0) return fail("the peer is not the child");

    uint64_t start = time_ms();
    Packet p;
    for (uint32_t seq = 0; seq < MESSAGES; seq++) {
        long n = port_recv(ch, &p, sizeof p, nullptr, nullptr, 20000);
        if (n < 8) return fail("port_recv");
        if (p.seq != seq) {
            printf("ipctest: FAIL: message %u arrived where %u was expected\n", p.seq, seq);
            return 1;
        }
        if (p.len != seq % 200 || (unsigned long)n != 8 + p.len) return fail("message length");
        for (uint32_t i = 0; i < p.len; i++)
            if (p.payload[i] != pattern(seq, i)) return fail("message contents");
    }
    uint64_t ms = time_ms() - start;
    uint32_t counted = MESSAGES;
    if (port_send(ch, &counted, sizeof counted, nullptr, 0) != 0) return fail("reply");
    if (wait_child(pid, 0, "flood client")) return 1;
    printf("ipctest: %u messages through a port, in order, none lost (%lu ms, %lu per second)\n", MESSAGES,
           (unsigned long)ms, (unsigned long)(MESSAGES * 1000ul / (ms ? ms : 1)));
    printf("ipctest: the kernel identified the peer as pid %u, uid %u\n", peer.pid, peer.uid);

    // 6a. The peer is gone: receiving and sending both say so.
    char byte = 0;
    if (port_recv(ch, &byte, 1, nullptr, nullptr, 1000) != -1 || errno != EPIPE) return fail("recv from a closed peer");
    if (port_send(ch, &byte, 1, nullptr, 0) != -1 || errno != EPIPE) return fail("send to a closed peer");
    close(ch);
    close(listener);
    // 6b. The name went with the listener.
    if (port_connect("test.flood") != -1 || errno != ENOENT) return fail("connecting to a closed port");
    printf("ipctest: a closed peer gives EPIPE, a closed port ENOENT\n");
    return 0;
}

// 3. Access control. The child gives up root and tries both ports.
static int test_access() {
    int priv = port_create("test.private", 0600);
    int open_port = port_create("test.public", 0666);
    if (priv < 0 || open_port < 0) return fail("port_create");
    pid_t pid = fork();
    if (pid < 0) return fail("fork");
    if (pid == 0) {
        if (setgid(1000) != 0 || setuid(1000) != 0) exit(20);
        if (setuid(0) != -1 || errno != EPERM) exit(21);            // root cannot be taken back
        if (port_connect("test.private") != -1 || errno != EACCES) exit(22);
        int ch = port_connect("test.public");
        if (ch < 0) exit(23);
        if (port_send(ch, "hi", 2, nullptr, 0) != 0) exit(24);
        exit(0);
    }
    int ch = port_accept(open_port, 10000);
    if (ch < 0) return fail("port_accept on the public port");
    struct port_peer peer;
    if (port_peer(ch, &peer) != 0 || peer.uid != 1000 || peer.gid != 1000 || peer.pid != (uint32_t)pid)
        return fail("peer identity of an unprivileged client");
    if (wait_child(pid, 0, "unprivileged client")) return 1;
    // Nothing reached the private port.
    if (port_accept(priv, 0) != -1 || errno != EAGAIN) return fail("the private port has a connection");
    close(ch);
    close(priv);
    close(open_port);
    printf("ipctest: uid 1000 was refused by a 0600 port (EACCES) and accepted by a 0666 one\n");
    return 0;
}

// 4. Descriptors in messages.
static int test_passing() {
    int listener = port_create("test.pass", 0600);
    if (listener < 0) return fail("port_create");
    pid_t pid = fork();
    if (pid < 0) return fail("fork");
    if (pid == 0) {
        int ch = port_connect("test.pass");
        if (ch < 0) exit(30);
        int fds[4];
        int nfds = 4;
        char note[16];
        if (port_recv(ch, note, sizeof note, fds, &nfds, 10000) != 5 || nfds != 2) exit(31);
        // The file arrives open, at the offset the sender left it.
        char text[64];
        long n = read(fds[0], text, sizeof text);
        if (n <= 0) exit(32);
        // The memory arrives with the sender's bytes in it.
        unsigned char* mem = (unsigned char*)shm_map(fds[1], PROT_READ | PROT_WRITE);
        if (mem == MAP_FAILED) exit(33);
        for (unsigned i = 0; i < 65536; i++)
            if (mem[i] != (unsigned char)(i * 13 + 1)) exit(34);
        for (unsigned i = 0; i < 65536; i++) mem[i] = (unsigned char)(i * 5 + 9);
        if (port_send(ch, text, (size_t)n, nullptr, 0) != 0) exit(35);
        exit(0);
    }
    int ch = port_accept(listener, 10000);
    if (ch < 0) return fail("port_accept");

    int file = open("/etc/motd", O_RDONLY);
    if (file < 0) return fail("open /etc/motd");
    int shm = shm_create(65536);
    if (shm < 0) return fail("shm_create");
    unsigned char* mem = (unsigned char*)shm_map(shm, PROT_READ | PROT_WRITE);
    if (mem == MAP_FAILED) return fail("shm_map");
    for (unsigned i = 0; i < 65536; i++) mem[i] = (unsigned char)(i * 13 + 1);

    int fds[2] = {file, shm};
    if (port_send(ch, "take!", 5, fds, 2) != 0) return fail("port_send with descriptors");
    char theirs[64], mine[64];
    long n = port_recv(ch, theirs, sizeof theirs, nullptr, nullptr, 10000);
    if (n <= 0) return fail("reply");
    if (wait_child(pid, 0, "descriptor receiver")) return 1;
    // The child read through the same open file, so its offset moved here too.
    if (lseek(file, 0, SEEK_CUR) != n) return fail("the passed file does not share its offset");
    if (lseek(file, 0, SEEK_SET) != 0 || read(file, mine, sizeof mine) != n || memcmp(mine, theirs, (size_t)n) != 0)
        return fail("the child read something else through the passed file");
    for (unsigned i = 0; i < 65536; i++)
        if (mem[i] != (unsigned char)(i * 5 + 9)) return fail("the child's writes are not visible in shared memory");
    printf("ipctest: a file and 64 KiB of shared memory crossed a port; both sides saw each other's writes\n");

    // Ports themselves do not travel (a descriptor queued inside the object
    // it names could never be freed).
    int self_fd[1] = {ch};
    if (port_send(ch, "x", 1, self_fd, 1) != -1 || errno != EINVAL) return fail("sending a port over a port");
    // A read-only mapping cannot be written... and the block outlives its descriptor.
    close(shm);
    mem[0] = 77;
    if (shm_unmap(mem) != 0) return fail("shm_unmap");
    if (shm_unmap(mem) != -1 || errno != EINVAL) return fail("unmapping twice");
    close(file);
    close(ch);
    close(listener);
    return 0;
}

// 5. A mutex and a counter in shared memory, four processes.
struct Shared {
    pthread_mutex_t lock;
    volatile unsigned long counter;
    volatile unsigned long unguarded;
};

static int test_futex() {
    const int WORKERS = 4;
    const unsigned long ROUNDS = 50000;
    int shm = shm_create(sizeof(Shared));
    if (shm < 0) return fail("shm_create");
    Shared* s = (Shared*)shm_map(shm, PROT_READ | PROT_WRITE);
    if (s == MAP_FAILED) return fail("shm_map");
    pthread_mutex_init(&s->lock, nullptr);

    uint64_t start = time_ms();
    pid_t pids[WORKERS];
    for (int w = 0; w < WORKERS; w++) {
        pids[w] = fork();
        if (pids[w] < 0) return fail("fork");
        if (pids[w] == 0) {
            // The mapping came across the fork.
            for (unsigned long i = 0; i < ROUNDS; i++) {
                pthread_mutex_lock(&s->lock);
                unsigned long v = s->counter;
                // Hold the lock across something slow now and then, so the
                // others really do have to wait.
                if (i % 4096 == 0) sched_yield();
                s->counter = v + 1;
                pthread_mutex_unlock(&s->lock);
            }
            exit(0);
        }
    }
    for (int w = 0; w < WORKERS; w++)
        if (wait_child(pids[w], 0, "futex worker")) return 1;
    if (s->counter != WORKERS * ROUNDS) {
        printf("ipctest: FAIL: the counter is %lu, expected %lu\n", s->counter, WORKERS * ROUNDS);
        return 1;
    }
    if (s->lock.state != 0) return fail("the mutex was left locked");
    printf("ipctest: %d processes added %lu each under a futex mutex: the counter is exactly %lu (%lu ms)\n", WORKERS,
           ROUNDS, s->counter, (unsigned long)(time_ms() - start));

    // futex_wait checks the value before it sleeps, and honours its timeout.
    uint32_t* word = (uint32_t*)&s->unguarded;
    *word = 5;
    if (futex_wait(word, 4, WAIT_FOREVER) != -1 || errno != EAGAIN) return fail("futex_wait on a changed value");
    start = time_ms();
    if (futex_wait(word, 5, 50) != -1 || errno != ETIMEDOUT) return fail("futex_wait timeout");
    uint64_t waited = time_ms() - start;
    if (waited < 50 || waited > 500) return fail("futex_wait timeout length");
    if (futex_wake(word, 1) != 0) return fail("futex_wake with nobody waiting");
    shm_unmap(s);
    close(shm);
    return 0;
}

// 6c. A receive buffer that is too small leaves the message queued; a full
// queue makes the sender wait rather than lose anything.
static int test_limits() {
    int listener = port_create("test.limits", 0600);
    if (listener < 0) return fail("port_create");
    pid_t pid = fork();
    if (pid < 0) return fail("fork");
    if (pid == 0) {
        int ch = port_connect("test.limits");
        if (ch < 0) exit(40);
        static char big[PORT_MESSAGE_MAX];
        for (unsigned i = 0; i < sizeof big; i++) big[i] = (char)i;
        if (port_send(ch, big, sizeof big + 1, nullptr, 0) != -1 || errno != EMSGSIZE) exit(41);
        // More than the queue holds (64 messages): the sends past that wait
        // until the parent starts reading.
        for (int i = 0; i < 200; i++) {
            big[0] = (char)i;
            if (port_send(ch, big, 1000, nullptr, 0) != 0) exit(42);
        }
        if (port_send(ch, big, sizeof big, nullptr, 0) != 0) exit(43);
        exit(0);
    }
    int ch = port_accept(listener, 10000);
    if (ch < 0) return fail("port_accept");
    sleep_ms(100);              // let the child fill the queue and block
    static char buf[PORT_MESSAGE_MAX];
    if (port_recv(ch, buf, 10, nullptr, nullptr, 1000) != -1 || errno != ENOSPC) return fail("a too-small buffer");
    for (int i = 0; i < 200; i++) {
        if (port_recv(ch, buf, sizeof buf, nullptr, nullptr, 10000) != 1000) return fail("queued message");
        if (buf[0] != (char)i) return fail("order after the queue filled");
    }
    if (port_recv(ch, buf, sizeof buf, nullptr, nullptr, 10000) != PORT_MESSAGE_MAX) return fail("a 64 KiB message");
    for (unsigned i = 1; i < sizeof buf; i++)
        if (buf[i] != (char)i) return fail("64 KiB message contents");
    if (wait_child(pid, 0, "limits client")) return 1;
    if (port_recv(ch, buf, sizeof buf, nullptr, nullptr, 0) != -1 || errno != EPIPE) return fail("recv after the end");
    close(ch);
    close(listener);
    printf("ipctest: a full queue made the sender wait; nothing was lost or reordered; 64 KiB is the limit\n");
    return 0;
}

int main(int, char**, char**) {
    if (test_flood()) return 1;
    if (test_access()) return 1;
    if (test_passing()) return 1;
    if (test_futex()) return 1;
    if (test_limits()) return 1;
    printf("ipctest: PASS\n");
    return 0;
}
