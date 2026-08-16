/* The same range fetch, ON THE WIRE, against a real public server.
 *
 * tests/unit/range_test.c proves the parsing and the verdicts against an
 * in-memory origin that can be made to misbehave on demand. It proves nothing
 * about whether a real server answers the request the way the fixture does --
 * whether Apache's Content-Range says what we expect, whether the keep-alive
 * connection survives three exchanges, whether the slice really is the slice.
 *
 * So: the SAME c/apps/browser/browser_rt.c and the SAME c/net/http/http1.c,
 * with the six socket syscalls under them wired to real BSD sockets instead of
 * a pipe. Plain HTTP only -- an https origin would need the OS's own TLS stack,
 * which does not exist on the host, and that is the stated limit of this file.
 *
 * The assertion is not "it returned 206". It downloads the whole resource once
 * with no Range, then asks for two disjoint slices, and requires each slice to
 * be byte-for-byte the corresponding window of the whole file. A server that
 * returned the right LENGTH from the wrong offset passes a status check and
 * fails this one.
 *
 * Not a prerequisite of anything: it needs the network.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "logit_abi.h"
#include "http1.h"
#include "bfetch.h"

int zlib_decompress(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)
{ (void)in; (void)inlen; (void)out; (void)outcap; if (outlen) *outlen = 0; return -1; }
int inflate_raw(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)
{ (void)in; (void)inlen; (void)out; (void)outcap; if (outlen) *outlen = 0; return -1; }

/* ---- the six syscalls, over real sockets ---- */

#define NSOCK 8
struct wsock { int used, fd, connected, eof, err; };
static struct wsock g_sk[NSOCK];

int hstub_open(const char *host, int port, int flags)
{
    if (flags & SOCK_F_TLS) return SOCK_E_ARG;      /* stated limit: plain HTTP */
    char svc[16];
    snprintf(svc, sizeof svc, "%d", port);
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, svc, &hints, &res) != 0 || !res) return SOCK_E_DNS;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return SOCK_E_CONN; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0 && errno != EINPROGRESS) { close(fd); return SOCK_E_CONN; }
    for (int i = 0; i < NSOCK; i++) {
        if (g_sk[i].used) continue;
        memset(&g_sk[i], 0, sizeof g_sk[i]);
        g_sk[i].used = 1;
        g_sk[i].fd = fd;
        return i;
    }
    close(fd);
    return SOCK_E_NOSLOT;
}

static struct wsock *sk(int h)
{ return (h >= 0 && h < NSOCK && g_sk[h].used) ? &g_sk[h] : 0; }

int hstub_poll(int h)
{
    struct wsock *s = sk(h);
    if (!s) return -1;
    int bits = 0;
    if (s->err) return SOCK_P_ERROR;
    if (!s->connected) {
        int e = 0; socklen_t el = sizeof e;
        getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &e, &el);
        fd_set w; FD_ZERO(&w); FD_SET(s->fd, &w);
        struct timeval tv = { 0, 0 };
        if (select(s->fd + 1, 0, &w, 0, &tv) > 0) {
            getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &e, &el);
            if (e) { s->err = 1; return SOCK_P_ERROR; }
            s->connected = 1;
        } else {
            return 0;
        }
    }
    bits = SOCK_P_CONNECTED | SOCK_P_WRITABLE;
    fd_set rd; FD_ZERO(&rd); FD_SET(s->fd, &rd);
    struct timeval tv = { 0, 0 };
    if (select(s->fd + 1, &rd, 0, 0, &tv) > 0) bits |= SOCK_P_READABLE;
    if (s->eof) bits |= SOCK_P_EOF;
    return bits;
}

int hstub_send(int h, const void *buf, int len)
{
    struct wsock *s = sk(h);
    if (!s) return -1;
    int n = (int)send(s->fd, buf, (size_t)len, MSG_NOSIGNAL);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    return n;
}

int hstub_recv(int h, void *buf, int max)
{
    struct wsock *s = sk(h);
    if (!s) return -1;
    int n = (int)recv(s->fd, buf, (size_t)max, 0);
    if (n == 0) { s->eof = 1; return 0; }           /* EOF: poll reports it */
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        s->err = 1;
        return -1;
    }
    return n;
}

int hstub_close(int h)
{
    struct wsock *s = sk(h);
    if (!s) return -1;
    close(s->fd);
    memset(s, 0, sizeof *s);
    return 0;
}

int hstub_alpn(int h, char *buf, int max)
{
    (void)h;
    const char *p = "http/1.1";
    int n = (int)strlen(p);
    if (n >= max) n = max - 1;
    memcpy(buf, p, (size_t)n); buf[n] = 0;
    return n;
}

unsigned long long hstub_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000);
}

/* ---- the test ---- */

static int fails, checks;
#define OK(cond) do { checks++; if (cond) printf("ok   %s\n", #cond); \
                      else { printf("FAIL %s\n", #cond); fails++; } } while (0)

static int settle(int id)
{
    unsigned long long t0 = hstub_now();
    while (bfetch_state(id) == BF_PENDING) {
        bfetch_pump();
        if (hstub_now() - t0 > 30000) break;
        usleep(500);
    }
    return bfetch_state(id);
}

int main(int argc, char **argv)
{
    const char *url = argc > 1 ? argv[1] : getenv("RANGE_WIRE_URL");
    if (!url || !*url) url = "http://deb.debian.org/debian/README";
    printf("range_wire: %s\n", url);

    bfetch_init();
    bfetch_set_base(url);

    /* 1. the whole thing, no Range. This is the reference. */
    unsigned char *whole = 0;
    int wlen = 0;
    if (bfetch_sync(url, &whole, &wlen) != 0 || wlen < 512) {
        printf("SKIP: could not fetch %s (network down, or the resource moved) -- "
               "nothing is asserted\n", url);
        return 77;
    }
    printf("     whole resource: %d bytes\n", wlen);
    OK(wlen >= 512);

    /* 2. two disjoint slices, each asserted against the reference window. */
    struct { long long f, l; } want[2] = { { 0, 63 }, { 200, 399 } };
    for (int i = 0; i < 2; i++) {
        long long f = want[i].f, l = want[i].l;
        if (l >= wlen) l = wlen - 1;
        int id = bfetch_start_range(url, f, l);
        OK(id >= 0);
        OK(settle(id) == BF_DONE);
        OK(bfetch_status(id) == 206);
        long long gf = -9, gl = -9, gt = -9;
        OK(bfetch_range_result(id, &gf, &gl, &gt) == BF_R_PARTIAL);
        OK(gf == f && gl == l);
        OK(gt == (long long)wlen);       /* the server's own length, cross-checked */
        int n = 0;
        const unsigned char *p = bfetch_body(id, &n);
        printf("     bytes=%lld-%lld -> %d bytes, Content-Range total %lld\n", f, l, n, gt);
        OK(n == (int)(l - f + 1));
        /* THE assertion: the right bytes, not merely the right count. */
        OK(p && memcmp(p, whole + f, (size_t)n) == 0);
        bfetch_release(id);
    }

    /* 3. a range past the end really is 416 on a real server. */
    {
        int id = bfetch_start_range(url, (long long)wlen + 100000, (long long)wlen + 100100);
        int st = settle(id);
        printf("     past-the-end -> state %d status %d (%s)\n",
               st, bfetch_status(id), bfetch_error(id));
        OK(st == BF_FAILED && bfetch_status(id) == 416);
        bfetch_release(id);
    }

    /* 4. the point of the pool: all of the above on ONE connection. */
    {
        int dials = 0, reuses = 0, reqs = 0;
        bfetch_stats(&dials, &reuses, &reqs);
        printf("     %d requests, %d dials, %d reuses\n", reqs, dials, reuses);
        OK(reuses > 0);
    }

    free(whole);
    bfetch_close_all();
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
