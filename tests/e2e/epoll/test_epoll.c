// epoll interest is keyed by (open file description, fd number), as on Linux.
// Every case prints a fixed line; expected.txt is the output on real Linux
// (arm64 Alpine). Events of one wait are printed sorted by data, since the
// order epoll_wait returns them in differs between kernels.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>

static const char *err(int ret) {
    if (ret >= 0)
        return "ok";
    switch (errno) {
        case EEXIST: return "EEXIST";
        case ENOENT: return "ENOENT";
        case EBADF: return "EBADF";
        case EINVAL: return "EINVAL";
        default: return strerror(errno);
    }
}

static int ctl(int ep, int op, int fd, unsigned events, int data) {
    struct epoll_event ev = {.events = events, .data.u64 = data};
    return epoll_ctl(ep, op, fd, &ev);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int cmp_ev(const void *a, const void *b) {
    return (int) ((const struct epoll_event *) a)->data.u64 -
        (int) ((const struct epoll_event *) b)->data.u64;
}

// Waits once and prints "label: IN 1, OUT 2" (or "none").
static int show(const char *label, int ep, int maxevents, int timeout) {
    struct epoll_event ev[8];
    int n = epoll_wait(ep, ev, maxevents, timeout);
    printf("%s:", label);
    if (n < 0) {
        printf(" %s\n", err(n));
        return n;
    }
    if (n == 0)
        printf(" none");
    qsort(ev, n, sizeof(ev[0]), cmp_ev);
    for (int i = 0; i < n; i++) {
        printf("%s", i ? "," : "");
        if (ev[i].events & EPOLLIN) printf(" IN");
        if (ev[i].events & EPOLLOUT) printf(" OUT");
        if (ev[i].events & EPOLLHUP) printf(" HUP");
        if (ev[i].events & EPOLLERR) printf(" ERR");
        printf(" %d", (int) ev[i].data.u64);
    }
    printf("\n");
    return n;
}

// A child writes one byte to fd after 50 ms, while the caller blocks.
static pid_t write_later(int fd) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        usleep(50 * 1000);
        if (write(fd, "x", 1) != 1)
            _exit(1);
        _exit(0);
    }
    return pid;
}

// A host event that is not fanned out to the right item is only noticed by
// iSH's 1 s re-check, so a blocking wait that takes that long missed it.
static void show_prompt(const char *label, int ep, int fd) {
    pid_t pid = write_later(fd);
    long start = now_ms();
    show(label, ep, 8, 3000);
    long elapsed = now_ms() - start;
    waitpid(pid, NULL, 0);
    printf("%s: %s\n", label, elapsed < 700 ? "prompt" : "slow");
}

static void drain(int fd) {
    char buf[64];
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    while (read(fd, buf, sizeof(buf)) > 0)
        ;
}

// fds 10 and 11 dup'd from one pty slave, like a shell's stdin and stdout.
// A tty is not host-backed in iSH; this is the case Bun hit.
static void test_pty(void) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
        printf("pty: setup failed\n");
        return;
    }
    int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    struct termios tio;
    tcgetattr(slave, &tio);
    cfmakeraw(&tio);
    tcsetattr(slave, TCSANOW, &tio);
    dup2(slave, 10);
    dup2(slave, 11);
    close(slave);
    fcntl(10, F_SETFL, O_NONBLOCK);

    int ep = epoll_create1(0);
    printf("pty: add 10 IN: %s\n", err(ctl(ep, EPOLL_CTL_ADD, 10, EPOLLIN, 1)));
    printf("pty: add 11 OUT: %s\n", err(ctl(ep, EPOLL_CTL_ADD, 11, EPOLLOUT, 2)));
    printf("pty: add 10 again: %s\n", err(ctl(ep, EPOLL_CTL_ADD, 10, EPOLLIN, 1)));
    show("pty idle", ep, 8, 0);
    write(master, "x", 1);
    // a pty delivers input asynchronously on Linux
    struct pollfd pfd = {.fd = 10, .events = POLLIN};
    poll(&pfd, 1, 1000);
    show("pty input", ep, 8, 0);
    printf("pty: mod 11 IN: %s\n", err(ctl(ep, EPOLL_CTL_MOD, 11, EPOLLIN, 2)));
    show("pty mod", ep, 8, 0);
    printf("pty: del 10: %s\n", err(ctl(ep, EPOLL_CTL_DEL, 10, 0, 0)));
    show("pty del", ep, 8, 0);
    close(ep);
    close(10);
    close(11);
    close(master);
}

// Two items on one host-backed description (a socket), so in iSH they share
// one host registration.
static void test_socket_dup(void) {
    int s[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, s);
    int a = s[0], b = dup(a);
    int ep = epoll_create1(0);
    printf("sock: add a IN: %s\n", err(ctl(ep, EPOLL_CTL_ADD, a, EPOLLIN, 5)));
    printf("sock: add b OUT: %s\n", err(ctl(ep, EPOLL_CTL_ADD, b, EPOLLOUT, 6)));
    printf("sock: add a again: %s\n", err(ctl(ep, EPOLL_CTL_ADD, a, EPOLLIN, 5)));
    show("sock idle", ep, 8, 0);
    write(s[1], "x", 1);
    show("sock input", ep, 8, 0);
    printf("sock: del b: %s\n", err(ctl(ep, EPOLL_CTL_DEL, b, 0, 0)));
    printf("sock: del b again: %s\n", err(ctl(ep, EPOLL_CTL_DEL, b, 0, 0)));
    show("sock del", ep, 8, 0);
    close(ep);
    close(a);
    close(b);
    close(s[1]);
}

// A level-triggered item and an edge-triggered one on a dup of the same pipe
// read end: one host registration, and a host event must wake both. Then
// either one alone must still be woken after the other is deleted.
static void test_pipe_fanout(void) {
    int p[2];
    pipe(p);
    int d = dup(p[0]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    int ep = epoll_create1(0);
    printf("fanout: add LT: %s\n", err(ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN, 10)));
    printf("fanout: add ET: %s\n", err(ctl(ep, EPOLL_CTL_ADD, d, EPOLLIN | EPOLLET, 11)));
    show_prompt("fanout both", ep, p[1]);
    drain(p[0]);
    printf("fanout: del LT: %s\n", err(ctl(ep, EPOLL_CTL_DEL, p[0], 0, 0)));
    show_prompt("fanout ET only", ep, p[1]);
    drain(p[0]);
    printf("fanout: re-add LT: %s\n", err(ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN, 10)));
    printf("fanout: del ET: %s\n", err(ctl(ep, EPOLL_CTL_DEL, d, 0, 0)));
    show_prompt("fanout LT only", ep, p[1]);
    drain(p[0]);
    close(ep);
    close(d);
    close(p[0]);
    close(p[1]);
}

// A fired EPOLLONESHOT item is disabled, not removed.
static void test_oneshot(void) {
    int p[2];
    pipe(p);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    int ep = epoll_create1(0);
    printf("oneshot: add: %s\n", err(ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN | EPOLLONESHOT, 20)));
    write(p[1], "x", 1);
    show("oneshot fire", ep, 8, 0);
    show("oneshot disabled", ep, 8, 0);
    printf("oneshot: add again: %s\n", err(ctl(ep, EPOLL_CTL_ADD, p[0], EPOLLIN | EPOLLONESHOT, 20)));
    printf("oneshot: mod: %s\n", err(ctl(ep, EPOLL_CTL_MOD, p[0], EPOLLIN | EPOLLONESHOT, 20)));
    show("oneshot rearmed", ep, 8, 0);
    drain(p[0]);
    printf("oneshot: mod: %s\n", err(ctl(ep, EPOLL_CTL_MOD, p[0], EPOLLIN | EPOLLONESHOT, 20)));
    show_prompt("oneshot wake", ep, p[1]);
    show("oneshot disabled", ep, 8, 0);
    drain(p[0]);

    // re-arming an edge-triggered oneshot item that is still ready reports
    // it again: Linux re-polls the file on MOD
    write(p[1], "xy", 2);
    printf("oneshot et: mod: %s\n", err(ctl(ep, EPOLL_CTL_MOD, p[0], EPOLLIN | EPOLLET | EPOLLONESHOT, 20)));
    show("oneshot et fire", ep, 8, 0);
    char c;
    read(p[0], &c, 1);
    printf("oneshot et: mod: %s\n", err(ctl(ep, EPOLL_CTL_MOD, p[0], EPOLLIN | EPOLLET | EPOLLONESHOT, 20)));
    show("oneshot et still ready", ep, 8, 0);
    drain(p[0]);
    close(ep);
    close(p[0]);
    close(p[1]);

    // the same on an fd that is not host-backed
    int e = eventfd(1, EFD_NONBLOCK);
    ep = epoll_create1(0);
    printf("eventfd: add: %s\n", err(ctl(ep, EPOLL_CTL_ADD, e, EPOLLIN | EPOLLET | EPOLLONESHOT, 3)));
    show("eventfd fire", ep, 8, 0);
    show("eventfd disabled", ep, 8, 0);
    printf("eventfd: add again: %s\n", err(ctl(ep, EPOLL_CTL_ADD, e, EPOLLIN | EPOLLET | EPOLLONESHOT, 3)));
    printf("eventfd: mod: %s\n", err(ctl(ep, EPOLL_CTL_MOD, e, EPOLLIN | EPOLLET | EPOLLONESHOT, 3)));
    show("eventfd rearmed", ep, 8, 0);
    close(ep);
    close(e);

    // maxevents=1 with two ready oneshot items: the one that did not fit is
    // not lost
    int q[2], r[2];
    pipe(q);
    pipe(r);
    ep = epoll_create1(0);
    ctl(ep, EPOLL_CTL_ADD, q[0], EPOLLIN | EPOLLONESHOT, 30);
    ctl(ep, EPOLL_CTL_ADD, r[0], EPOLLIN | EPOLLONESHOT, 31);
    write(q[1], "x", 1);
    write(r[1], "x", 1);
    struct epoll_event ev[2];
    int n1 = epoll_wait(ep, ev, 1, 0);
    int n2 = epoll_wait(ep, ev + 1, 1, 0);
    int sum = (n1 == 1 ? ev[0].data.u64 : 0) + (n2 == 1 ? ev[1].data.u64 : 0);
    printf("maxevents 1: %d then %d, data sum %d\n", n1, n2, sum);
    show("maxevents 1 third", ep, 1, 0);
    close(ep);
    close(q[0]);
    close(q[1]);
    close(r[0]);
    close(r[1]);
}

// An item lives as long as its description, not its fd number.
static void test_close(void) {
    int c[2];
    pipe(c);
    int a = c[0];
    int ep = epoll_create1(0);
    printf("close: add a: %s\n", err(ctl(ep, EPOLL_CTL_ADD, a, EPOLLIN, 7)));
    int b = dup(a);
    close(a);
    write(c[1], "x", 1);
    show("close a", ep, 8, 0);
    printf("close: del a: %s\n", err(ctl(ep, EPOLL_CTL_DEL, a, 0, 0)));
    drain(b);
    int n[2];
    pipe(n);
    printf("close: number reused: %s\n", n[0] == a ? "yes" : "no");
    printf("close: add new: %s\n", err(ctl(ep, EPOLL_CTL_ADD, n[0], EPOLLIN, 8)));
    write(c[1], "x", 1);
    write(n[1], "x", 1);
    show("close both", ep, 8, 0);
    drain(n[0]);
    close(b);
    show("close b", ep, 8, 0);
    show_prompt("close new", ep, n[1]);
    close(ep);
    close(c[1]);
    close(n[0]);
    close(n[1]);
}

// select() with two fd numbers of one socket: the second must not take over
// the first's host registration.
static void test_select(void) {
    int s[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, s);
    int d = dup(s[0]);
    fd_set rfds, xfds;
    FD_ZERO(&rfds);
    FD_ZERO(&xfds);
    FD_SET(s[0], &rfds);
    FD_SET(d, &xfds);
    struct timeval tv = {.tv_sec = 3};
    pid_t pid = write_later(s[1]);
    long start = now_ms();
    int n = select((s[0] > d ? s[0] : d) + 1, &rfds, NULL, &xfds, &tv);
    long elapsed = now_ms() - start;
    waitpid(pid, NULL, 0);
    printf("select: %d r=%d x=%d %s\n", n, FD_ISSET(s[0], &rfds), FD_ISSET(d, &xfds),
            elapsed < 700 ? "prompt" : "slow");
    close(d);
    close(s[0]);
    close(s[1]);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_pty();
    test_socket_dup();
    test_pipe_fanout();
    test_oneshot();
    test_close();
    test_select();
    return 0;
}
