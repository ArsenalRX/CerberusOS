// Wrappers for phase 11: signals, ports, shared memory, event queues, and
// the identity and information calls.
#include <cerberus.h>

extern "C" {

static long check(long r) {
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

#define CALL(nr, a, b, c, d, e, f) check(syscall6(nr, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f)))

// ---- signals ----
sighandler_t signal(int sig, sighandler_t handler) {
    long r = syscall6(SYS_signal, sig, (long)handler, 0, 0, 0, 0);
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return SIG_ERR;
    }
    return (sighandler_t)r;
}

int kill(pid_t pid, int sig) { return (int)CALL(SYS_kill, pid, sig, 0, 0, 0, 0); }
int raise(int sig) { return kill(getpid(), sig); }

// ---- ports ----
int port_create(const char* name, int mode) { return (int)CALL(SYS_port_create, name, mode, 0, 0, 0, 0); }
int port_connect(const char* name) { return (int)CALL(SYS_port_connect, name, 0, 0, 0, 0, 0); }

int port_send(int port, const void* msg, size_t len, const int* fds, int nfds) {
    return (int)CALL(SYS_port_send, port, msg, len, fds, nfds, 0);
}

int sigprocmask(int how, const uint64_t* set, uint64_t* oldset) {
    return (int)CALL(SYS_sigprocmask, how, set, oldset, 0, 0, 0);
}

int port_try_send(int port, const void* msg, size_t len, const int* fds, int nfds) {
    return (int)CALL(SYS_port_try_send, port, msg, len, fds, nfds, 0);
}

long port_recv(int port, void* buf, size_t len, int* fds, int* nfds, uint64_t timeout_ms) {
    return CALL(SYS_port_recv, port, buf, len, fds, nfds, timeout_ms);
}

int port_accept(int port, uint64_t timeout_ms) {
    int fd = -1, n = 1;
    if (port_recv(port, nullptr, 0, &fd, &n, timeout_ms) < 0) return -1;
    return fd;
}

int port_peer(int port, struct port_peer* out) { return (int)CALL(SYS_port_peer, port, out, 0, 0, 0, 0); }

// ---- shared memory ----
int shm_create(size_t size) { return (int)CALL(SYS_shm_create, size, 0, 0, 0, 0, 0); }

void* shm_map(int handle, int prot) {
    long r = syscall6(SYS_shm_map, handle, prot, 0, 0, 0, 0);
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return MAP_FAILED;
    }
    return (void*)r;
}

int shm_unmap(void* addr) { return (int)CALL(SYS_shm_unmap, addr, 0, 0, 0, 0, 0); }
int mprotect(void* addr, size_t len, int prot) { return (int)CALL(SYS_mprotect, addr, len, prot, 0, 0, 0); }

// ---- event queues ----
int event_create(int flags) { return (int)CALL(SYS_event_create, flags, 0, 0, 0, 0, 0); }
int event_ctl(int ev, int op, int fd, const struct event* e) { return (int)CALL(SYS_event_ctl, ev, op, fd, e, 0, 0); }

int event_wait(int ev, struct event* out, int max, uint64_t timeout_ms) {
    return (int)CALL(SYS_event_wait, ev, out, max, timeout_ms, 0, 0);
}

// ---- identity and information ----
pid_t getppid(void) { return (pid_t)syscall6(SYS_getppid, 0, 0, 0, 0, 0, 0); }
int getuid(void) { return (int)syscall6(SYS_getuid, 0, 0, 0, 0, 0, 0); }
int geteuid(void) { return (int)syscall6(SYS_geteuid, 0, 0, 0, 0, 0, 0); }
int getgid(void) { return (int)syscall6(SYS_getgid, 0, 0, 0, 0, 0, 0); }
int getegid(void) { return (int)syscall6(SYS_getegid, 0, 0, 0, 0, 0, 0); }
int setuid(int uid) { return (int)CALL(SYS_setuid, uid, 0, 0, 0, 0, 0); }
int setgid(int gid) { return (int)CALL(SYS_setgid, gid, 0, 0, 0, 0, 0); }
int umask(int mask) { return (int)syscall6(SYS_umask, mask, 0, 0, 0, 0, 0); }
int sysinfo(struct sysinfo* out) { return (int)CALL(SYS_sysinfo, out, 0, 0, 0, 0, 0); }
uint64_t time_us(void) { return (uint64_t)syscall6(SYS_time_us, 0, 0, 0, 0, 0, 0); }

} // extern "C"
