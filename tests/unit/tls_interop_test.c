/* Host-side TLS 1.3 interop driver: runs c/net/tls/tls.c -- the real record
 * layer, the real key schedule, the real X.509 chain verification -- against a
 * real `openssl s_server` over a real socket.
 *
 * WHY this exists rather than a canned-transcript test: the things that were
 * broken (no HelloRetryRequest, one key-exchange group, no ALPN) are exactly
 * the things a recorded transcript cannot exercise, because the server's
 * behaviour is what varies. `openssl s_server -groups P-256` is the only honest
 * way to prove we can now reach a server that insists on P-256; a fixture would
 * just be us asserting our own opinion of what such a server sends.
 *
 * The kernel side is unmodified. Everything below the record layer -- TCP,
 * timers, the CSPRNG, kprintf -- is replaced here by the smallest possible
 * host implementation with the same contracts (in particular tcp_recv/tcp_send
 * are non-blocking and return 0 for "would block", which is what tls.c
 * assumes). Trust comes from a bundle generated for the throwaway CA that
 * run-tls-interop.sh creates, so the chain check is the real one, not a stub.
 *
 * Usage: tls_interop_test <host> <port> <sni> [--alpn LIST] [--expect-alpn P]
 *                         [--expect-fail] [--blocking]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "tls.h"

/* ------------------------------------------------------------ host "kernel" */

static int g_fd = -1;

void kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* TIMER_HZ is 100 in the kernel, so a tick is 10 ms. Keep that so tls.c's
 * 3000-tick handshake deadline means the same ~30 s it means on the target. */
uint64_t timer_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 100u + (uint64_t)(ts.tv_nsec / 10000000);
}

void net_poll(void) { }
void net_idle(void)
{
    /* The blocking tls_connect() path calls this instead of spinning. On the
     * target it is sti;hlt until the next tick; here, waiting on the socket is
     * the same idea and keeps the test fast. */
    struct pollfd p = { g_fd, POLLIN, 0 };
    poll(&p, 1, 10);
}

/* Not the kernel DRBG -- but this is a test client whose keys are thrown away
 * at the end of the process, and the point of the exercise is the protocol. */
void kernel_random_bytes(uint8_t *out, int len)
{
    static FILE *ur;
    if (!ur) ur = fopen("/dev/urandom", "rb");
    if (ur && fread(out, 1, (size_t)len, ur) == (size_t)len) return;
    for (int i = 0; i < len; i++) out[i] = (uint8_t)rand();
}
int rng_strong(void) { return 1; }

/* --- the TCP contract tls.c is written against: never blocks, 0 = would
 *     block, -1 = closed and drained. --- */
int tcp_send(int id, const void *buf, int len)
{
    (void)id;
    ssize_t n = send(g_fd, buf, (size_t)len, MSG_NOSIGNAL);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}
int tcp_recv(int id, void *buf, int max)
{
    (void)id;
    ssize_t n = recv(g_fd, buf, (size_t)max, 0);
    if (n > 0) return (int)n;
    if (n == 0) return -1;                       /* peer closed */
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}
void tcp_close(int id) { (void)id; }
int  tcp_alive(int id) { (void)id; return 1; }

static int dial(const char *host, const char *port)
{
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) { close(fd); freeaddrinfo(res); return -1; }
    freeaddrinfo(res);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

/* ------------------------------------------------------------------- driver */

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s host port sni [opts]\n", argv[0]); return 2; }
    const char *host = argv[1], *port = argv[2], *sni = argv[3];
    const char *alpn = 0, *want_alpn = 0;
    int expect_fail = 0, blocking = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--alpn") && i + 1 < argc)             alpn = argv[++i];
        else if (!strcmp(argv[i], "--expect-alpn") && i + 1 < argc) want_alpn = argv[++i];
        else if (!strcmp(argv[i], "--expect-fail"))                 expect_fail = 1;
        else if (!strcmp(argv[i], "--blocking"))                    blocking = 1;
    }

    g_fd = dial(host, port);
    if (g_fd < 0) { printf("RESULT: FAIL (connect)\n"); return 1; }

    int64_t now = (int64_t)time(0);
    int id;

    if (blocking) {
        /* Exercise the compatibility wrapper too: http_get and the existing
         * boot harnesses still go through this entry point, so a regression
         * here would be invisible to the stepped tests. */
        id = tls_connect(0, sni, now);
        if (id < 0) {
            printf("RESULT: %s (tls_connect rc=%d)\n", expect_fail ? "PASS-EXPECTED-FAIL" : "FAIL", id);
            return expect_fail ? 0 : 1;
        }
    } else {
        id = tls_start(0, sni, alpn, now);
        if (id < 0) {
            printf("RESULT: %s (tls_start rc=%d)\n", expect_fail ? "PASS-EXPECTED-FAIL" : "FAIL", id);
            return expect_fail ? 0 : 1;
        }
        for (;;) {
            int rc = tls_step(id);
            if (rc == TLS_DONE) break;
            if (rc < 0) {
                printf("RESULT: %s (tls_step rc=%d)\n", expect_fail ? "PASS-EXPECTED-FAIL" : "FAIL", rc);
                tls_close(id);
                return expect_fail ? 0 : 1;
            }
            struct pollfd p = { g_fd, (short)(rc == TLS_WANT_WRITE ? POLLOUT : POLLIN), 0 };
            if (poll(&p, 1, 15000) == 0) { printf("RESULT: FAIL (step timeout)\n"); return 1; }
        }
    }

    if (expect_fail) {
        printf("RESULT: FAIL (handshake succeeded but should not have)\n");
        tls_close(id); return 1;
    }

    char sel[32];
    tls_alpn(id, sel, sizeof sel);
    printf("ALPN: '%s'\n", sel);
    if (want_alpn && strcmp(sel, want_alpn)) {
        printf("RESULT: FAIL (alpn '%s' != expected '%s')\n", sel, want_alpn);
        tls_close(id); return 1;
    }

    /* Prove the application-data direction works too, not just the handshake:
     * a short GET must come back as a decryptable response. */
    char req[256];
    int rl = snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", sni);
    int off = 0;
    while (off < rl) {
        int n = tls_send(id, req + off, rl - off);
        if (n < 0) { printf("RESULT: FAIL (tls_send)\n"); tls_close(id); return 1; }
        if (n == 0) { struct pollfd p = { g_fd, POLLOUT, 0 }; poll(&p, 1, 5000); continue; }
        off += n;
    }

    char buf[4096]; int total = 0; int sawhttp = 0;
    uint64_t stop = timer_ticks() + 800;
    while (timer_ticks() < stop && total < 200000) {
        int n = tls_recv(id, buf, (int)sizeof buf - 1);
        if (n < 0) break;
        if (n == 0) { struct pollfd p = { g_fd, POLLIN, 0 }; poll(&p, 1, 200); continue; }
        if (!total) { buf[n] = 0; sawhttp = strncmp(buf, "HTTP/1.", 7) == 0; }
        total += n;
    }
    tls_close(id);

    printf("BYTES: %d\n", total);
    if (!sawhttp || total <= 0) { printf("RESULT: FAIL (no HTTP response)\n"); return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
