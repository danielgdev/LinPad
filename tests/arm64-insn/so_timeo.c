// SO_RCVTIMEO/SO_SNDTIMEO with the 64-bit Linux option numbers (20/21, the ones
// musl aarch64 and Rust std use) must work: herdr's client and server set them on
// every unix socket and failed with EINVAL ("herdr: Invalid argument (os error 22)",
// or "Broken pipe" when the server dropped the connection). Also checks the
// time64 numbers (66/67), the get round trip, Linux's argument checks, that
// blocking recv(), read() and write() honour the timeouts with EAGAIN (also a
// sub-millisecond one), and that the options can still be set after the peer
// has closed, before or after accept() (Darwin refuses that with EINVAL).
// Guest-only:  gcc -O1 -o so_timeo so_timeo.c && ./so_timeo    expected: "so_timeo ok"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SO_RCVTIMEO_OLD 20
#define SO_SNDTIMEO_OLD 21
#define SO_RCVTIMEO_NEW 66
#define SO_SNDTIMEO_NEW 67

struct tv64 { long long sec, usec; };

static int fails;

static void check(const char *what, int ok) {
    if (!ok) {
        printf("%s: failed (errno %d, %s)\n", what, errno, strerror(errno));
        fails++;
    }
}

static int set_tv(int fd, int opt, long long sec, long long usec) {
    struct tv64 tv = {sec, usec};
    return setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof(tv));
}

// Values that are whole jiffies at HZ 100, 250 and 1000 read back exactly.
static void round_trip(int fd, int opt, long long sec, long long usec, long long want_sec, long long want_usec) {
    char what[64];
    snprintf(what, sizeof(what), "setsockopt(%d, {%lld, %lld})", opt, sec, usec);
    check(what, set_tv(fd, opt, sec, usec) == 0);
    struct tv64 got;
    memset(&got, 0xff, sizeof(got));
    socklen_t len = sizeof(got);
    snprintf(what, sizeof(what), "getsockopt(%d)", opt);
    check(what, getsockopt(fd, SOL_SOCKET, opt, &got, &len) == 0);
    if (len != sizeof(got) || got.sec != want_sec || got.usec != want_usec) {
        printf("getsockopt(%d): len %u {%lld, %lld}, want 16 {%lld, %lld}\n",
                opt, len, got.sec, got.usec, want_sec, want_usec);
        fails++;
    }
}

static void on_alarm(int sig) {
    (void) sig;
    static const char msg[] = "so_timeo: timed out (a blocking call ignored its timeout)\n";
    write(1, msg, sizeof(msg) - 1);
    _exit(1);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// A blocking call on a socket with a timeout fails with EAGAIN after
// min_ms..max_ms. kind: 0 recv, 1 read.
static void expect_timeout(const char *what, int fd, int kind, long min_ms, long max_ms) {
    char c;
    long start = now_ms();
    errno = 0;
    ssize_t n = kind == 0 ? recv(fd, &c, 1, 0) : read(fd, &c, 1);
    long took = now_ms() - start;
    if (n != -1 || (errno != EAGAIN && errno != EWOULDBLOCK) || took < min_ms || took > max_ms) {
        printf("%s: got %zd errno %d after %ld ms, want EAGAIN after %ld..%ld ms\n",
                what, n, errno, took, min_ms, max_ms);
        fails++;
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    signal(SIGPIPE, SIG_IGN);
    alarm(30);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        perror("socketpair");
        return 1;
    }
    round_trip(sv[0], SO_RCVTIMEO_OLD, 2, 500000, 2, 500000);
    round_trip(sv[0], SO_SNDTIMEO_OLD, 5, 0, 5, 0);
    round_trip(sv[1], SO_RCVTIMEO_NEW, 1, 200000, 1, 200000);
    round_trip(sv[1], SO_SNDTIMEO_NEW, 0, 0, 0, 0);
    // A timeout too long to count is none, and reads back as {0, 0}.
    round_trip(sv[1], SO_RCVTIMEO_OLD, INT64_MAX, 0, 0, 0);

    // Linux's argument checks.
    struct tv64 tv = {0, 0};
    errno = 0;
    check("setsockopt(optlen 8) gives EINVAL",
            setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO_OLD, &tv, 8) == -1 && errno == EINVAL);
    errno = 0;
    check("setsockopt(tv_usec 1000000) gives EDOM",
            set_tv(sv[0], SO_RCVTIMEO_OLD, 0, 1000000) == -1 && errno == EDOM);
    errno = 0;
    check("setsockopt(tv_usec 0x100000005) gives EDOM",
            set_tv(sv[0], SO_RCVTIMEO_NEW, 0, 0x100000005LL) == -1 && errno == EDOM);
    errno = 0;
    check("setsockopt(tv_usec -1) gives EDOM",
            set_tv(sv[0], SO_SNDTIMEO_OLD, 0, -1) == -1 && errno == EDOM);

    // Blocking recv() and read() time out with EAGAIN.
    check("setsockopt(200ms)", set_tv(sv[0], SO_RCVTIMEO_OLD, 0, 200000) == 0);
    expect_timeout("recv with 200ms SO_RCVTIMEO", sv[0], 0, 150, 5000);
    expect_timeout("read with 200ms SO_RCVTIMEO", sv[0], 1, 150, 5000);
    // A sub-millisecond timeout still expires.
    check("setsockopt(500us)", set_tv(sv[0], SO_RCVTIMEO_OLD, 0, 500) == 0);
    expect_timeout("recv with 500us SO_RCVTIMEO", sv[0], 0, 0, 2000);
    expect_timeout("read with 500us SO_RCVTIMEO", sv[0], 1, 0, 2000);
    // A negative tv_sec is accepted (Linux warns) and makes the call fail at once.
    check("setsockopt(tv_sec -1)", set_tv(sv[0], SO_RCVTIMEO_OLD, -1, 0) == 0);
    expect_timeout("recv with negative SO_RCVTIMEO", sv[0], 0, 0, 2000);

    // A blocking write() into a full socket times out too.
    check("setsockopt(SO_SNDTIMEO 200ms)", set_tv(sv[1], SO_SNDTIMEO_OLD, 0, 200000) == 0);
    static char chunk[65536];
    ssize_t w;
    long start = now_ms();
    while ((w = write(sv[1], chunk, sizeof(chunk))) > 0 && now_ms() - start < 10000)
        ;
    if (w != -1 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        printf("write into a full socket with SO_SNDTIMEO: got %zd errno %d, want EAGAIN\n", w, errno);
        fails++;
    }

    // After the peer closes, Linux still accepts the option; the next recv is EOF.
    close(sv[1]);
    char c;
    while (recv(sv[0], chunk, sizeof(chunk), MSG_DONTWAIT) > 0)
        ;
    check("setsockopt after peer close",
            set_tv(sv[0], SO_RCVTIMEO_OLD, 0, 200000) == 0);
    check("recv after peer close is EOF", recv(sv[0], &c, 1, 0) == 0);
    close(sv[0]);

    // The same for a connection whose client closed before it was accepted.
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/so_timeo.%d.sock", (int) getpid());
    unlink(addr.sun_path);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    check("bind", bind(lfd, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    check("listen", listen(lfd, 4) == 0);
    int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    check("connect", connect(cfd, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    close(cfd);
    int afd = accept(lfd, NULL, NULL);
    check("accept", afd >= 0);
    if (afd >= 0) {
        check("setsockopt on an accepted socket whose peer closed",
                set_tv(afd, SO_RCVTIMEO_OLD, 0, 200000) == 0);
        check("recv on it is EOF", recv(afd, &c, 1, 0) == 0);
        close(afd);
    }
    close(lfd);
    unlink(addr.sun_path);

    if (fails)
        return 1;
    puts("so_timeo ok");
    return 0;
}
