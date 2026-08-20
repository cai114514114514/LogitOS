/* Host driver for c/net/tls/tls_server.c: the real server state machine, the
 * real record layer, the real key schedule and a certificate this machine
 * generated itself, over a real socket.
 *
 * TWO MODES, and they answer different questions:
 *
 *   serve  -- listen, run ONE handshake, then echo exactly N bytes and close.
 *             The peer is `openssl s_client`. This is the half that cannot be
 *             satisfied by agreeing with ourselves: openssl shares no code
 *             with this tree, so a ServerHello we build wrong, a transcript we
 *             hash wrong or a CertificateVerify we sign wrong is caught by a
 *             program with no stake in our opinion of the protocol.
 *
 *   pair   -- our client (c/net/tls/tls.c) and our server in ONE process, over
 *             a loopback TCP connection, stepped alternately. This is the
 *             reverse of tests/unit/run-tls-interop.sh, and it is the case
 *             that diffs the two TLS 1.3 key schedules in this tree against
 *             each other -- see the SHARING note at the top of tls_server.c
 *             for why that duplication exists and why this case is what bounds
 *             its cost.
 *
 * TRUST. The client half needs an anchor for a certificate that does not exist
 * until the process is running, so the trust store is defined HERE and filled
 * at runtime from the generated certificate's own public key. Nothing about
 * the chain verification is stubbed: x509.c's real x509_verify_chain runs, the
 * real name check runs, and pointing the anchor at a DIFFERENT key makes the
 * client refuse -- which --wrong-anchor exercises, so "the check ran" is
 * observable rather than assumed.
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
#include "tls_server.h"
#include "x509.h"
#include "roots.h"

/* ------------------------------------------------------ the trust store ----
 * `logit_roots` is const and its `ec` pointer is a const pointer to a buffer
 * that is NOT const -- so the row is a compile-time constant and the KEY
 * MATERIAL is written at runtime, with no cast and no aliasing trick. eclen is
 * 64 because the anchor is always the P-256 point X||Y that tlss_self_signed
 * produces; a different curve would need a different row, which is exactly the
 * kind of thing that should not be silently flexible in a trust store. */
static uint8_t g_anchor[64];
const struct root_ca logit_roots[] = {
    { ROOT_EC, 256, g_anchor, 64, 0, 0, 0, 0 },
};
const int logit_nroots = 1;
const char *const logit_root_names[] = { "tls-server-test-selfsigned" };
const char *const logit_roots_skipped[] = { 0 };
const int logit_nroots_skipped = 0;

/* ------------------------------------------------------------ host kernel */

/* A tiny fd table so a client and a server can live in one process: tls.c and
 * tls_server.c both call tcp_send(s->tcp, ...), so the id has to select the
 * socket. The single global fd the interop driver uses would make `pair` mode
 * send the client's bytes to the server's own socket. */
#define NFD 4
static int g_fd[NFD] = { -1, -1, -1, -1 };

static int g_quiet;
void kprintf(const char *fmt, ...)
{
    if (g_quiet) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

uint64_t timer_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 100u + (uint64_t)(ts.tv_nsec / 10000000);
}
uint64_t timer_ms(void) { return timer_ticks() * 10u; }

void net_poll(void) { }
void net_idle(void) { struct pollfd p = { g_fd[0], POLLIN, 0 }; poll(&p, 1, 10); }

void kernel_random_bytes(uint8_t *out, int len)
{
    static FILE *ur;
    if (!ur) ur = fopen("/dev/urandom", "rb");
    if (ur && fread(out, 1, (size_t)len, ur) == (size_t)len) return;
    for (int i = 0; i < len; i++) out[i] = (uint8_t)rand();
}
int rng_strong(void) { return 1; }

int tcp_send(int id, const void *buf, int len)
{
    if (id < 0 || id >= NFD || g_fd[id] < 0) return -1;
    ssize_t n = send(g_fd[id], buf, (size_t)len, MSG_NOSIGNAL);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}
int tcp_recv(int id, void *buf, int max)
{
    if (id < 0 || id >= NFD || g_fd[id] < 0) return -1;
    ssize_t n = recv(g_fd[id], buf, (size_t)max, 0);
    if (n > 0) return (int)n;
    if (n == 0) return -1;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}
void tcp_close(int id) { (void)id; }
int  tcp_alive(int id) { (void)id; return 1; }

/* ------------------------------------------------------------- identity --- */

static uint8_t g_certbuf[4096];
static struct tls_ident g_ident;

/* Fill the trust anchor from the certificate we just made. Parsing our own DER
 * with the production parser rather than remembering where the point sits is
 * the point: if tlss_self_signed emitted a SubjectPublicKeyInfo x509.c cannot
 * read, this fails HERE, before any handshake, instead of as an opaque
 * "untrusted anchor" three states later. */
static int arm_anchor(void)
{
    struct cert c;
    if (x509_parse(g_ident.chain[0], g_ident.chainlen[0], &c) != 0) {
        fprintf(stderr, "FATAL: our own certificate does not parse\n");
        return -1;
    }
    if (c.key_type != KEY_EC || c.key_curve != 256 || c.publen != 65 || c.pub[0] != 0x04) {
        fprintf(stderr, "FATAL: unexpected key shape in our certificate\n");
        return -1;
    }
    memcpy(g_anchor, c.pub + 1, 64);
    return 0;
}

static int make_ident(const char *cn, int64_t now)
{
    int n = tlss_self_signed(&g_ident, cn, now, 30, g_certbuf, sizeof g_certbuf);
    if (n < 0) { fprintf(stderr, "FATAL: could not generate a certificate\n"); return -1; }
    return arm_anchor();
}

/* --------------------------------------------------------------- helpers -- */

static void nonblock(int fd) { fcntl(fd, F_SETFL, O_NONBLOCK); }

static int listen_on(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    if (listen(fd, 4) != 0) { close(fd); return -1; }
    return fd;
}

static int dial(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    nonblock(fd);
    return fd;
}

static void wait_io(int fd, int rc, int ms)
{
    struct pollfd p = { fd, (short)(rc == TLS_WANT_WRITE ? POLLOUT : POLLIN), 0 };
    poll(&p, 1, ms);
}

/* --------------------------------------------------------------- serve ---- */

static int mode_serve(int port, const char *alpn, const char *certout,
                      long echo_bytes, const char *expect_alpn)
{
    if (certout) {
        FILE *f = fopen(certout, "wb");
        if (!f) { perror("certout"); return 1; }
        fwrite(g_ident.chain[0], 1, (size_t)g_ident.chainlen[0], f);
        fclose(f);
    }
    int lfd = listen_on(port);
    if (lfd < 0) { fprintf(stderr, "FATAL: cannot listen on %d\n", port); return 1; }
    /* Only now is the port live, and the certificate is already on disk -- the
     * harness waits for the port and can rely on the file being there. */
    printf("LISTENING %d\n", port); fflush(stdout);

    struct sockaddr_in ca; socklen_t cl = sizeof ca;
    int fd = accept(lfd, (struct sockaddr *)&ca, &cl);
    close(lfd);
    if (fd < 0) { fprintf(stderr, "FATAL: accept failed\n"); return 1; }
    nonblock(fd);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    g_fd[0] = fd;

    int id = tlss_start(0, &g_ident, alpn, (int64_t)time(0));
    if (id < 0) { printf("RESULT: FAIL (tlss_start %d)\n", id); return 1; }

    uint64_t t0 = timer_ticks();
    for (;;) {
        int rc = tlss_step(id);
        if (rc == TLS_DONE) break;
        if (rc < 0) { printf("RESULT: FAIL (tlss_step %d)\n", rc); tlss_close(id); return 1; }
        if (timer_ticks() - t0 > 2000) { printf("RESULT: FAIL (handshake stalled)\n"); return 1; }
        wait_io(fd, rc, 50);
    }

    char sel[32]; tlss_alpn(id, sel, sizeof sel);
    char sni[256]; tlss_sni(id, sni, sizeof sni);
    printf("VERSION: 0x%04x\n", tlss_version(id));
    printf("ALPN: %s\n", sel);
    printf("SNI: %s\n", sni);
    if (expect_alpn && strcmp(sel, expect_alpn) != 0) {
        printf("RESULT: FAIL (alpn \"%s\", expected \"%s\")\n", sel, expect_alpn);
        tlss_close(id); return 1;
    }

    /* Echo exactly echo_bytes and then close. A byte count rather than
     * "until the peer closes" because `openssl s_client -quiet` implies
     * -ign_eof and does NOT close on stdin EOF, so waiting for its close is
     * waiting forever -- a hang that reads like a protocol bug. */
    long done = 0;
    uint64_t t1 = timer_ticks();
    while (done < echo_bytes) {
        char buf[8192];
        int n = tlss_recv(id, buf, (int)sizeof buf);
        if (n < 0) { printf("RESULT: FAIL (peer closed after %ld of %ld bytes)\n", done, echo_bytes); tlss_close(id); return 1; }
        if (n == 0) {
            if (timer_ticks() - t1 > 2000) { printf("RESULT: FAIL (echo stalled at %ld)\n", done); tlss_close(id); return 1; }
            wait_io(fd, TLS_WANT_READ, 20);
            continue;
        }
        t1 = timer_ticks();
        int off = 0;
        while (off < n) {
            int w = tlss_send(id, buf + off, n - off);
            if (w < 0) { printf("RESULT: FAIL (send failed)\n"); tlss_close(id); return 1; }
            if (w == 0) { wait_io(fd, TLS_WANT_WRITE, 20); continue; }
            off += w;
        }
        done += n;
    }
    printf("ECHOED: %ld\n", done);
    printf("RESULT: OK\n");
    fflush(stdout);
    tlss_close(id);
    /* Linger briefly so the close_notify and the last record reach the peer
     * before the process exit resets the connection. */
    struct pollfd p = { fd, 0, 0 }; poll(&p, 1, 50);
    close(fd);
    return 0;
}

/* ---------------------------------------------------------------- pair ---- */

/* `expect_cert_fail`: the client MUST refuse, and specifically with
 * TLS_E_CERT. Not "anything negative" -- a failure at the record layer is also
 * negative and would mean the case proved nothing about chain verification,
 * which is the only thing these two controls exist to demonstrate. */
static int mode_pair(const char *alpn_srv, const char *alpn_cli,
                     const char *expect_alpn, int wrong_anchor, const char *sni,
                     int expect_cert_fail)
{
    int lfd = listen_on(0);
    if (lfd < 0) { fprintf(stderr, "FATAL: cannot listen\n"); return 1; }
    struct sockaddr_in a; socklen_t al = sizeof a;
    getsockname(lfd, (struct sockaddr *)&a, &al);
    int port = ntohs(a.sin_port);

    int cfd = dial(port);
    if (cfd < 0) { fprintf(stderr, "FATAL: cannot connect\n"); return 1; }
    struct sockaddr_in pa; socklen_t pl = sizeof pa;
    int sfd = accept(lfd, (struct sockaddr *)&pa, &pl);
    close(lfd);
    if (sfd < 0) { fprintf(stderr, "FATAL: accept failed\n"); return 1; }
    nonblock(sfd);
    int one = 1;
    setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    g_fd[0] = cfd;                    /* tcp id 0 = the client's socket */
    g_fd[1] = sfd;                    /* tcp id 1 = the server's        */

    if (wrong_anchor) {
        /* Flip one bit of the trust anchor. Everything else is identical, so a
         * client that still completes is not checking the chain -- which is
         * the only way to tell "verified" from "parsed". */
        g_anchor[0] ^= 1;
    }

    int64_t now = (int64_t)time(0);
    int sid = tlss_start(1, &g_ident, alpn_srv, now);
    if (sid < 0) { printf("RESULT: FAIL (tlss_start %d)\n", sid); return 1; }
    int cid = tls_start(0, sni, alpn_cli, now);
    if (cid < 0) { printf("RESULT: FAIL (tls_start %d)\n", cid); return 1; }

    int crc = TLS_WANT_WRITE, src = TLS_WANT_READ;
    uint64_t t0 = timer_ticks();
    while (crc != TLS_DONE || src != TLS_DONE) {
        if (crc >= 0 && crc != TLS_DONE) crc = tls_step(cid);
        if (src >= 0 && src != TLS_DONE) src = tlss_step(sid);
        if (crc < 0 || src < 0) break;
        if (timer_ticks() - t0 > 2000) {
            printf("RESULT: FAIL (stalled: client %d, server %d)\n", crc, src);
            return 1;
        }
        if (crc != TLS_DONE || src != TLS_DONE) {
            struct pollfd p[2] = { { cfd, POLLIN, 0 }, { sfd, POLLIN, 0 } };
            poll(p, 2, 5);
        }
    }

    if (expect_cert_fail) {
        if (crc == TLS_E_CERT) { printf("RESULT: OK (client refused: TLS_E_CERT)\n"); return 0; }
        printf("RESULT: FAIL (client=%d server=%d, wanted TLS_E_CERT=%d)\n",
               crc, src, TLS_E_CERT);
        return 1;
    }
    if (crc != TLS_DONE || src != TLS_DONE) {
        printf("RESULT: FAIL (client %d, server %d)\n", crc, src);
        return 1;
    }

    char cs[32], ss[32];
    tls_alpn(cid, cs, sizeof cs);
    tlss_alpn(sid, ss, sizeof ss);
    char gotsni[256]; tlss_sni(sid, gotsni, sizeof gotsni);
    printf("VERSION: client 0x%04x server 0x%04x\n", tls_version(cid), tlss_version(sid));
    printf("ALPN: client \"%s\" server \"%s\"\n", cs, ss);
    printf("SNI: %s\n", gotsni);
    if (tls_version(cid) != TLS_VER_13 || tlss_version(sid) != TLS_VER_13) {
        printf("RESULT: FAIL (not TLS 1.3 on both ends)\n"); return 1;
    }
    if (strcmp(cs, ss) != 0) { printf("RESULT: FAIL (ALPN disagreement)\n"); return 1; }
    if (expect_alpn && strcmp(cs, expect_alpn) != 0) {
        printf("RESULT: FAIL (alpn \"%s\", expected \"%s\")\n", cs, expect_alpn); return 1;
    }
    if (strcmp(gotsni, sni) != 0) {
        printf("RESULT: FAIL (server saw sni \"%s\", client sent \"%s\")\n", gotsni, sni); return 1;
    }

    /* --- the byte stream, BOTH directions, and long enough to be split across
     *     records: SEND_REC_MAX is 4096, so 10000 bytes is three records in
     *     each direction and exercises the partial-send path that a short
     *     "hello" cannot reach. --- */
    enum { N = 10000 };
    static char payload[N], echoed[N];
    for (int i = 0; i < N; i++) payload[i] = (char)('0' + (i * 7 + i / 251) % 64);

    int sent = 0, got = 0, srv_relayed = 0;
    static char srvbuf[N];
    int srv_have = 0, srv_sent = 0;
    t0 = timer_ticks();
    while (got < N) {
        if (sent < N) {
            int w = tls_send(cid, payload + sent, N - sent);
            if (w < 0) { printf("RESULT: FAIL (client send)\n"); return 1; }
            sent += w;
        }
        /* server: read what it can, echo what it has */
        int n = tlss_recv(sid, srvbuf + srv_have, N - srv_have);
        if (n > 0) srv_have += n;
        else if (n < 0) { printf("RESULT: FAIL (server recv, %d relayed)\n", srv_relayed); return 1; }
        while (srv_sent < srv_have) {
            int w = tlss_send(sid, srvbuf + srv_sent, srv_have - srv_sent);
            if (w < 0) { printf("RESULT: FAIL (server send)\n"); return 1; }
            if (w == 0) break;
            srv_sent += w; srv_relayed += w;
        }
        int r = tls_recv(cid, echoed + got, N - got);
        if (r > 0) got += r;
        else if (r < 0) { printf("RESULT: FAIL (client recv after %d)\n", got); return 1; }
        if (timer_ticks() - t0 > 3000) {
            printf("RESULT: FAIL (echo stalled: sent %d, relayed %d, got %d)\n",
                   sent, srv_relayed, got);
            return 1;
        }
        struct pollfd p[2] = { { cfd, POLLIN, 0 }, { sfd, POLLIN, 0 } };
        poll(p, 2, 2);
    }
    if (memcmp(payload, echoed, N) != 0) {
        int i = 0; while (i < N && payload[i] == echoed[i]) i++;
        printf("RESULT: FAIL (echo differs at byte %d)\n", i);
        return 1;
    }
    printf("ECHOED: %d bytes both directions, byte for byte\n", N);
    printf("RESULT: OK\n");
    tls_close(cid);
    tlss_close(sid);
    return 0;
}

/* ---------------------------------------------------------------- bench ---
 * What one identity costs: a P-256 keygen plus one ECDSA signature over the
 * TBSCertificate, through the same bignum the kernel uses. Reported because
 * the header of tls_server.h argues that generating on first use is cheap
 * enough to prefer over shipping a key, and that argument needs a number. */
static int mode_bench(int iters)
{
    if (iters < 1) iters = 20;
    struct timespec a, b;
    struct tls_ident id;
    static uint8_t buf[4096];
    g_quiet = 1;
    /* One warm-up: curves_init() builds three Barrett reciprocals on the first
     * call, which is a one-off this bench should not charge to every cert. */
    tlss_self_signed(&id, "localhost", (int64_t)time(0), 30, buf, sizeof buf);
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < iters; i++)
        if (tlss_self_signed(&id, "localhost", (int64_t)time(0), 30, buf, sizeof buf) < 0)
            return 1;
    clock_gettime(CLOCK_MONOTONIC, &b);
    double us = ((double)(b.tv_sec - a.tv_sec) * 1e6 +
                 (double)(b.tv_nsec - a.tv_nsec) / 1e3) / iters;
    g_quiet = 0;
    printf("tlss_self_signed: %.0f us per certificate (%d iterations, host)\n", us, iters);
    return 0;
}

/* ----------------------------------------------------------------- main --- */

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    const char *alpn = 0, *alpn_cli = 0, *expect_alpn = 0, *certout = 0;
    const char *cn = "localhost", *sni = "localhost";
    int port = 0, wrong_anchor = 0, expect_cert_fail = 0;
    long echo_bytes = 0;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--alpn") && i + 1 < argc) alpn = argv[++i];
        else if (!strcmp(argv[i], "--client-alpn") && i + 1 < argc) alpn_cli = argv[++i];
        else if (!strcmp(argv[i], "--expect-alpn") && i + 1 < argc) expect_alpn = argv[++i];
        else if (!strcmp(argv[i], "--cert-out") && i + 1 < argc) certout = argv[++i];
        else if (!strcmp(argv[i], "--echo") && i + 1 < argc) echo_bytes = atol(argv[++i]);
        else if (!strcmp(argv[i], "--cn") && i + 1 < argc) cn = argv[++i];
        else if (!strcmp(argv[i], "--sni") && i + 1 < argc) sni = argv[++i];
        else if (!strcmp(argv[i], "--wrong-anchor")) { wrong_anchor = 1; expect_cert_fail = 1; }
        else if (!strcmp(argv[i], "--expect-cert-fail")) expect_cert_fail = 1;
        else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
        else if (argv[i][0] != '-') port = atoi(argv[i]);
    }
    if (!alpn_cli) alpn_cli = alpn;

    if (make_ident(cn, (int64_t)time(0)) != 0) return 1;

    if (!strcmp(mode, "serve")) {
        if (port <= 0) { fprintf(stderr, "usage: tls_server_test serve <port> ...\n"); return 2; }
        return mode_serve(port, alpn, certout, echo_bytes, expect_alpn);
    }
    if (!strcmp(mode, "pair"))
        return mode_pair(alpn, alpn_cli, expect_alpn, wrong_anchor, sni, expect_cert_fail);
    if (!strcmp(mode, "bench")) return mode_bench(port);
    if (!strcmp(mode, "gencert")) {
        if (!certout) { fprintf(stderr, "gencert needs --cert-out\n"); return 2; }
        FILE *f = fopen(certout, "wb");
        if (!f) { perror("certout"); return 1; }
        fwrite(g_ident.chain[0], 1, (size_t)g_ident.chainlen[0], f);
        fclose(f);
        printf("CERT: %d bytes\n", g_ident.chainlen[0]);
        return 0;
    }
    fprintf(stderr, "usage: tls_server_test {serve <port>|pair|gencert} [options]\n");
    return 2;
}
