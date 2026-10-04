// Event queues (SPEC phase 11): one wait over ports, timers and child
// exit; a waiting server uses no CPU and wakes within a millisecond.
#include <cerberus.h>

static int fail(const char* what) {
    printf("eventtest: FAIL: %s (errno %d: %s)\n", what, errno, strerror(errno));
    return 1;
}

static int watch(int ev, int fd, uint32_t events, uint64_t data, uint64_t timeout_ms = 0) {
    event e = {events, 0, data, timeout_ms};
    return event_ctl(ev, EVENT_ADD, fd, &e);
}

int main(int, char**, char**) {
    int ev = event_create(0);
    if (ev < 0) return fail("event_create");
    event out[8];

    // 1. Nothing watched, nothing happens: the timeout is honoured.
    uint64_t start = time_ms();
    if (event_wait(ev, out, 8, 50) != 0) return fail("an empty wait");
    uint64_t waited = time_ms() - start;
    if (waited < 50 || waited > 300) return fail("the timeout's length");

    // 2. A one-shot timer.
    if (watch(ev, EVENT_FD_TIMER, EVENT_TIMER, 111, 40) != 0) return fail("adding a timer");
    if (watch(ev, EVENT_FD_TIMER, EVENT_TIMER, 111, 40) != -1 || errno != EEXIST) return fail("adding it twice");
    start = time_ms();
    if (event_wait(ev, out, 8, 5000) != 1) return fail("waiting for the timer");
    waited = time_ms() - start;
    if (out[0].events != EVENT_TIMER || out[0].fd != EVENT_FD_TIMER || out[0].data != 111) return fail("the timer's event");
    if (waited < 40 || waited > 300) return fail("the timer's length");
    if (event_wait(ev, out, 8, 30) != 0) return fail("a one-shot timer fired twice");
    if (event_ctl(ev, EVENT_DEL, EVENT_FD_TIMER, nullptr) != 0) return fail("removing the timer");
    printf("eventtest: a timeout and a one-shot timer were both on time\n");

    // 3. A server waiting on a named port and its connections.
    int listener = port_create("test.events", 0600);
    if (listener < 0) return fail("port_create");
    if (watch(ev, listener, EVENT_READ, 1) != 0) return fail("watching the port");
    if (watch(ev, EVENT_FD_CHILD, EVENT_CHILD, 3) != 0) return fail("watching for children");

    const int PINGS = 50;
    pid_t pid = fork();
    if (pid < 0) return fail("fork");
    if (pid == 0) {
        sleep_ms(1200);             // the parent measures an idle second first
        int ch = port_connect("test.events");
        if (ch < 0) exit(1);
        for (int i = 0; i < PINGS; i++) {
            sleep_ms(20);           // the server is asleep in event_wait again by now
            uint64_t now = time_us();
            if (port_send(ch, &now, sizeof now, nullptr, 0) != 0) exit(2);
            uint64_t echoed = 0;
            if (port_recv(ch, &echoed, sizeof echoed, nullptr, nullptr, 5000) != sizeof echoed || echoed != now) exit(3);
        }
        exit(7);
    }

    // The idle second: this process waits, the child sleeps, nothing else
    // should be running.
    struct sysinfo before, after;
    sysinfo(&before);
    start = time_ms();
    if (event_wait(ev, out, 8, 1000) != 0) return fail("an event arrived during the idle wait");
    sysinfo(&after);
    unsigned long ticks = (unsigned long)(after.ticks - before.ticks);
    unsigned long idle = (unsigned long)(after.idle_ticks - before.idle_ticks);
    unsigned long busy_permille = ticks ? (ticks - idle) * 1000 / ticks : 1000;
    printf("eventtest: during a 1 s event_wait the %u CPUs were idle for %lu of %lu ticks\n", after.cpus, idle, ticks);
    if (ticks < 50 || busy_permille > 20) return fail("the machine was not idle while waiting");

    // The connection arrives as readiness of the listener.
    if (event_wait(ev, out, 8, 5000) != 1 || out[0].data != 1 || !(out[0].events & EVENT_READ))
        return fail("waiting for a connection");
    int ch = port_accept(listener, 0);
    if (ch < 0) return fail("port_accept after the event");
    if (watch(ev, ch, EVENT_READ, 2) != 0) return fail("watching the channel");

    // Each ping carries the time it was sent: how long until we are awake?
    unsigned long latency[PINGS];
    int pings = 0;
    bool hup = false, child = false;
    while (!hup || !child) {
        int n = event_wait(ev, out, 8, 5000);
        if (n <= 0) return fail("waiting for a message");
        uint64_t woke = time_us();
        for (int i = 0; i < n; i++) {
            if (out[i].data == 2 && (out[i].events & EVENT_READ)) {
                uint64_t sent = 0;
                long got = port_recv(ch, &sent, sizeof sent, nullptr, nullptr, 0);
                if (got == sizeof sent) {
                    if (pings < PINGS) latency[pings] = (unsigned long)(woke - sent);
                    pings++;
                    if (port_send(ch, &sent, sizeof sent, nullptr, 0) != 0) return fail("echo");
                    continue;
                }
                if (!(out[i].events & EVENT_HUP)) return fail("a readable channel had no message");
            }
            if (out[i].data == 2 && (out[i].events & EVENT_HUP)) {
                // The peer closed: stop watching, or it would be reported for ever.
                if (event_ctl(ev, EVENT_DEL, ch, nullptr) != 0) return fail("removing the channel");
                hup = true;
            } else if (out[i].data == 3 && (out[i].events & EVENT_CHILD)) {
                int status = 0;
                if (waitpid(pid, &status, WNOHANG) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 7)
                    return fail("the child's exit status");
                child = true;
            } else if (out[i].data != 2) {
                return fail("an unexpected event");
            }
        }
    }
    if (pings != PINGS) return fail("the number of messages");
    // Sort; report the median and the worst.
    for (int i = 1; i < PINGS; i++)
        for (int j = i; j > 0 && latency[j] < latency[j - 1]; j--) {
            unsigned long t = latency[j];
            latency[j] = latency[j - 1];
            latency[j - 1] = t;
        }
    printf("eventtest: woke %lu us after a message was sent (median of %d; worst %lu us)\n", latency[PINGS / 2], PINGS,
           latency[PINGS - 1]);
    if (latency[PINGS / 2] > 1000) return fail("waking took more than 1 ms");
    printf("eventtest: the peer closing and the child exiting both arrived as events\n");

    // 4. What cannot be watched.
    if (watch(ev, ev, EVENT_READ, 0) != -1 || errno != EINVAL) return fail("a queue watching itself");
    if (watch(ev, 31, EVENT_READ, 0) != -1 || errno != EBADF) return fail("watching a closed descriptor");
    if (event_ctl(ev, EVENT_DEL, listener, nullptr) != 0) return fail("removing the listener");
    if (event_ctl(ev, EVENT_DEL, listener, nullptr) != -1 || errno != EINVAL) return fail("removing it twice");
    close(ch);
    close(listener);
    close(ev);
    printf("eventtest: PASS\n");
    return 0;
}
