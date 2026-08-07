/* h2check -- the on-device proof that HTTP/2 works against a real server, and
 * the instrument that measures what it is worth.
 *
 *     h2check <host> [path ...]        offer h2 and http/1.1, like a browser
 *     h2check --h1 <host> [path ...]   offer ONLY http/1.1
 *
 * WHY BOTH MODES.  "HTTP/2 works" is not the interesting claim; every large
 * site speaks it and a client that fetches one page proves very little. The
 * claim worth making is the one the connection-pool line made when it reported
 * 14 handshakes becoming 4: a number, for the same page, before and after. So
 * this program fetches THE SAME set of URLs twice, once over each protocol,
 * through the same sockets and the same TLS, and prints:
 *
 *   conns   sockets opened. Pooled HTTP/1.1 already made this small, so this
 *           is NOT expected to improve much -- saying otherwise would be
 *           claiming the pool's win a second time.
 *   rt      ROUND TRIPS: the longest chain of requests that had to happen one
 *           after another. On HTTP/1.1 a connection carries one request at a
 *           time, so N resources over C connections cost ceil(N/C) sequential
 *           trips. On HTTP/2 every request is in flight at once on ONE
 *           connection, so it is 1. That is the whole difference, and it is
 *           what multiplexing means in a number.
 *
 * It also asserts the thing that breaks silently: a server that does NOT
 * choose h2 must come back over HTTP/1.1 and still deliver the bytes. That is
 * the common case (plenty of hosts still answer http/1.1), and a client that
 * only ever gets tested against h2 servers ships with a dead fallback.
 *
 * The HTTP/1.1 half is the real c/net/http/http1.c client with the real
 * c/net/http/hpool.c pool, not a stub -- otherwise the comparison would be
 * between HTTP/2 and a strawman. `zlib_decompress` is stubbed because this
 * program asks for `identity` and never reaches the inflater; the browser
 * links the real one from the Rust staticlib.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "logit.h"
#include "http2.h"
#include "hpack.h"
#include "http1.h"
#include "hpool.h"

/* http1.c's content-decoding path, stubbed. Never called: every request this
 * program sends carries `Accept-Encoding: identity`, so no response reaches
 * the inflater. The browser links the real implementations from the Rust
 * staticlib; pulling that in here would double this binary for a code path
 * the measurement deliberately avoids (a decompressed body would make the
 * byte counts incomparable between the two runs). */
int zlib_decompress(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)
{ (void)in; (void)inlen; (void)out; (void)outcap; if (outlen) *outlen = 0; return -1; }
int inflate_raw(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)
{ (void)in; (void)inlen; (void)out; (void)outcap; if (outlen) *outlen = 0; return -1; }

#define MAXURL   12
/* A browser uses six per origin. Four here, because every extra connection is
 * another full TLS handshake with the certificate chain verified in software
 * on an emulated CPU, and this program has to finish inside a boot test's
 * budget. It changes the round-trip arithmetic (ceil(N/4) instead of
 * ceil(N/6)), not the conclusion. */
#define MAXCONN   4
#define TIMEOUT_MS 40000

static const char *g_host;
static int g_port = 443;

/* ------------------------------------------------------ socket transport */

/* The vtable both clients take. H1_* and H2_* use the same three values on
 * purpose, so one transport serves either protocol. */
static int s_read(void *ctx, void *buf, int len)
{
    int fd = (int)(long)ctx;
    int bits = sock_poll(fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) return -2;         /* H1_TERR / H2_TERR */
    int n = sock_recv(fd, buf, len);
    if (n > 0) return n;
    if (n < 0) return -1;                                     /* H1_EOF / H2_EOF */
    if (bits & SOCK_P_EOF) return -1;
    return 0;                                                 /* H1_AGAIN / H2_AGAIN */
}
static int s_write(void *ctx, const void *buf, int len)
{
    int fd = (int)(long)ctx;
    int bits = sock_poll(fd);
    if (bits < 0 || (bits & SOCK_P_ERROR)) return -2;
    if (!(bits & SOCK_P_CONNECTED)) return 0;                 /* still handshaking */
    return sock_send(fd, buf, len);                           /* 0 = queue full */
}

/* Open a socket and wait for the handshake. Returns the fd, or < 0. `alpn` is
 * filled with what was negotiated. */
static int dial(int offer_h2, char *alpn, int alpnmax)
{
    int flags = SOCK_F_TLS | SOCK_F_ALPN_HTTP11;
    if (offer_h2) flags |= SOCK_F_ALPN_H2;
    alpn[0] = 0;
    int fd = sock_open(g_host, g_port, flags);
    if (fd < 0) { printf("H2CHECK dial failed rc=%d\n", fd); return -1; }
    unsigned long long t0 = monotonic_ms();
    for (;;) {
        int bits = sock_poll(fd);
        if (bits < 0 || (bits & SOCK_P_ERROR)) {
            printf("H2CHECK handshake failed code=%d\n", bits < 0 ? bits : SOCK_ERR_CODE(bits));
            sock_close(fd);
            return -1;
        }
        if (bits & SOCK_P_CONNECTED) break;
        if (monotonic_ms() - t0 > TIMEOUT_MS) {
            printf("H2CHECK handshake timeout\n");
            sock_close(fd);
            return -1;
        }
        sys_yield();
    }
    int n = sock_alpn(fd, alpn, alpnmax);
    if (n <= 0) alpn[0] = 0;
    return fd;
}

/* -------------------------------------------------------------- h2 mode */

static int run_h2(int fd, const char *const *paths, int npath, long *bytes_out, int *ok_out)
{
    struct h2_conn c;
    struct h2_transport t;
    t.read = s_read; t.write = s_write; t.poll = NULL; t.ctx = (void *)(long)fd;
    if (h2_conn_start(&c, &t) != H2_OK) { printf("H2CHECK h2 start failed\n"); return -1; }
    h2_conn_stall_ms(&c, TIMEOUT_MS);

    struct hpack_list extra;
    hpack_list_init(&extra);
    hpack_list_add(&extra, "user-agent", -1, "LogitOS-h2check/1.0", -1, 0);
    hpack_list_add(&extra, "accept", -1, "*/*", -1, 0);
    hpack_list_add(&extra, "accept-encoding", -1, "identity", -1, 0);

    /* EVERY request goes out before ANY response arrives. That single loop is
     * the multiplexing: on HTTP/1.1 the second of these could not be written
     * until the first response had been read. */
    uint32_t ids[MAXURL];
    int n = 0;
    for (int i = 0; i < npath; i++) {
        int r = h2_request(&c, "GET", "https", g_host, paths[i], &extra, NULL, 0);
        if (r <= 0) { printf("H2CHECK request %d refused: %s\n", i, h2_strerror(r)); break; }
        ids[n++] = (uint32_t)r;
    }
    hpack_list_free(&extra);
    printf("H2CHECK h2 issued %d requests on 1 connection before any reply\n", n);

    unsigned long long t0 = monotonic_ms();
    long total = 0;
    int ok = 0;
    for (;;) {
        int st = h2_conn_pump(&c, (int64_t)monotonic_ms());
        int left = 0;
        for (int i = 0; i < n; i++) if (!h2_stream_done(&c, ids[i])) left++;
        if (!left) break;
        if (st == H2_C_ERROR || st == H2_C_CLOSED) break;
        if (monotonic_ms() - t0 > TIMEOUT_MS) { printf("H2CHECK h2 timeout\n"); break; }
        sys_yield();
    }

    for (int i = 0; i < n; i++) {
        int bl = 0;
        const uint8_t *b = h2_stream_body(&c, ids[i], &bl);
        int status = h2_stream_status(&c, ids[i]);
        unsigned crc = 2166136261u;
        for (int k = 0; k < bl; k++) crc = (crc ^ b[k]) * 16777619u;
        printf("H2CHECK url=%s stream=%u status=%d bytes=%d fnv=%08x err=%s\n",
               paths[i], ids[i], status, bl, crc, h2_strerror(h2_stream_err(&c, ids[i])));
        if (status >= 200 && status < 400 && bl >= 0) { ok++; total += bl; }
    }
    printf("H2CHECK h2 frames_in=%d frames_out=%d peak_streams=%d window_updates=%d\n",
           c.frames_in, c.frames_out, c.max_concurrent_seen, c.window_updates_out);
    h2_conn_free(&c);
    *bytes_out = total;
    *ok_out = ok;
    return 1;          /* round trips: everything was in flight at once */
}

/* -------------------------------------------------------------- h1 mode */

struct h1slot {
    int fd, live, busy, served;
    struct h1_conn c;
    int url;
};

/* The honest HTTP/1.1 comparison: a real pool, up to MAXCONN sockets, each
 * carrying one request at a time. Round trips are the longest chain of
 * requests any single connection had to run in sequence, because that is what
 * the page actually waits for. */
static int run_h1(int first_fd, const char *const *paths, int npath,
                  int *conns_out, long *bytes_out, int *ok_out)
{
    struct h1slot sl[MAXCONN];
    memset(sl, 0, sizeof sl);
    sl[0].fd = first_fd; sl[0].live = 1;
    int conns = 1;
    int next = 0, done = 0, ok = 0;
    long total = 0;
    unsigned long long t0 = monotonic_ms();

    while (done < npath) {
        /* Hand any free connection the next URL, dialling more up to MAXCONN. */
        for (int i = 0; i < MAXCONN && next < npath; i++) {
            if (!sl[i].live) {
                if (conns >= MAXCONN) continue;
                char alpn[16];
                int fd = dial(0, alpn, sizeof alpn);
                if (fd < 0) break;
                sl[i].fd = fd; sl[i].live = 1; conns++;
            }
            if (sl[i].busy) continue;
            char req[512];
            int rl = snprintf(req, sizeof req,
                              "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: LogitOS-h2check/1.0\r\n"
                              "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n",
                              paths[next], g_host);
            char *buf = (char *)malloc((size_t)rl + 1);
            if (!buf) return -1;
            memcpy(buf, req, (size_t)rl + 1);
            struct h1_transport t;
            t.read = s_read; t.write = s_write; t.poll = NULL; t.ctx = (void *)(long)sl[i].fd;
            if (h1_conn_start(&sl[i].c, &t, buf, rl) != H1_OK) { free(buf); return -1; }
            sl[i].busy = 1;
            sl[i].url = next++;
        }

        int progressed = 0;
        for (int i = 0; i < MAXCONN; i++) {
            if (!sl[i].busy) continue;
            int st = h1_conn_pump(&sl[i].c);
            if (st == H1_C_DONE || st == H1_C_ERROR) {
                struct h1_response *r = &sl[i].c.resp;
                unsigned crc = 2166136261u;
                for (int k = 0; k < r->body_len; k++) crc = (crc ^ r->body[k]) * 16777619u;
                printf("H2CHECK url=%s conn=%d status=%d bytes=%d fnv=%08x\n",
                       paths[sl[i].url], i, r->code, r->body_len, crc);
                if (r->code >= 200 && r->code < 400) { ok++; total += r->body_len; }
                int reusable = (st == H1_C_DONE) && r->keep_alive;
                h1_conn_free(&sl[i].c);
                sl[i].busy = 0;
                sl[i].served++;
                done++;
                progressed = 1;
                if (!reusable) { sock_close(sl[i].fd); sl[i].live = 0; }
            }
        }
        if (!progressed) {
            if (monotonic_ms() - t0 > TIMEOUT_MS) { printf("H2CHECK h1 timeout\n"); break; }
            sys_yield();
        }
    }

    int rt = 0;
    for (int i = 0; i < MAXCONN; i++) {
        if (sl[i].served > rt) rt = sl[i].served;
        if (sl[i].live) sock_close(sl[i].fd);
    }
    *conns_out = conns;
    *bytes_out = total;
    *ok_out = ok;
    return rt ? rt : 1;
}

/* ------------------------------------------------------------------ main */

static int one_run(int offer_h2, const char *const *paths, int npath, const char *label)
{
    char alpn[16];
    int fd = dial(offer_h2, alpn, sizeof alpn);
    if (fd < 0) { printf("H2CHECK %s FAILED to connect\n", label); return -1; }
    printf("H2CHECK alpn=%s (%s offered h2)\n", alpn[0] ? alpn : "(none)",
           offer_h2 ? "we" : "we did not");

    long bytes = 0; int ok = 0, conns = 1, rt;
    if (!strcmp(alpn, "h2")) {
        rt = run_h2(fd, paths, npath, &bytes, &ok);
        sock_close(fd);
        printf("H2CHECK RESULT mode=h2 host=%s requests=%d ok=%d conns=%d rt=%d bytes=%ld\n",
               g_host, npath, ok, conns, rt, bytes);
    } else {
        /* THE FALLBACK. ALPN did not choose h2 -- because we did not offer it,
         * or because the server did not want it -- so nothing here speaks
         * HTTP/2 at all and the request goes out as HTTP/1.1 on the socket we
         * already have. Getting this wrong is the silent failure: a client
         * that assumes h2 sends a connection preface to a server that answers
         * "400 Bad Request" in a language it is no longer listening in. */
        rt = run_h1(fd, paths, npath, &conns, &bytes, &ok);
        printf("H2CHECK RESULT mode=h1 host=%s requests=%d ok=%d conns=%d rt=%d bytes=%ld\n",
               g_host, npath, ok, conns, rt, bytes);
    }
    return ok;
}

int main(int argc, char **argv)
{
    int a = 1;
    int only_h1 = 0, both = 0;
    while (a < argc && argv[a][0] == '-') {
        if (!strcmp(argv[a], "--h1")) only_h1 = 1;
        else if (!strcmp(argv[a], "--both")) both = 1;
        else if (!strcmp(argv[a], "--port") && a + 1 < argc) g_port = atoi(argv[++a]);
        else { printf("h2check: unknown option %s\n", argv[a]); return 2; }
        a++;
    }
    if (a >= argc) {
        printf("usage: h2check [--h1] [--both] [--port N] <host> [path ...]\n");
        return 2;
    }
    g_host = argv[a++];

    const char *paths[MAXURL];
    int npath = 0;
    for (; a < argc && npath < MAXURL; a++) paths[npath++] = argv[a];
    if (!npath) paths[npath++] = "/";

    printf("H2CHECK BEGIN host=%s paths=%d\n", g_host, npath);

    int ok = 0;
    if (both) {
        /* The comparison: identical URLs, identical sockets, identical TLS --
         * the only difference is which protocol ALPN was allowed to choose. */
        printf("H2CHECK --- run 1: http/1.1 only ---\n");
        int a1 = one_run(0, paths, npath, "h1-only");
        printf("H2CHECK --- run 2: h2 offered ---\n");
        int a2 = one_run(1, paths, npath, "h2-offered");
        ok = (a1 > 0 && a2 > 0);
    } else {
        ok = one_run(!only_h1, paths, npath, only_h1 ? "h1-only" : "default") > 0;
    }

    printf("H2CHECK %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
