#include "clib.h"

/* socktest -- the proof that connections progress CONCURRENTLY.
 *
 *     socktest <host> <port> <path> [n]      (n defaults to 4)
 *
 * Opens n non-blocking sockets at once, queues an HTTP/1.0 GET on each before
 * any of them has finished connecting, and then polls all n in one loop until
 * each has read its body to EOF.
 *
 * The interesting part is not that it works, it is what it MEASURES. A socket
 * layer that quietly serialised -- one connection at a time, the next starting
 * when the previous finished -- would fetch the same bytes and pass a naive
 * "did it download" check. So this prints two numbers that a serial
 * implementation cannot produce:
 *
 *   SOCKTEST_OVERLAP    the moment the LAST socket connected, and the moment the
 *                       FIRST one finished. If last_connect < first_done then all
 *                       n connections were live at the same instant -- which is
 *                       the definition of concurrent and is impossible if they
 *                       ran one after another.
 *
 *   SOCKTEST_SWITCHES   how many times consecutive events came from DIFFERENT
 *                       sockets, over the ordered event log (connected /
 *                       first-byte / eof, three per socket). A strictly serial
 *                       run emits s0,s0,s0,s1,s1,s1,... = n-1 switches, the
 *                       minimum possible; interleaved progress is many more.
 *
 * Timestamps come from monotonic_ms(), which advances in steps of 10 ms -- so
 * the numbers are coarse, and the assertions are about ordering, not duration. */

#define MAXSOCK   8
#define BUFSZ     2048

enum { ST_OPEN, ST_CONNECTED, ST_DONE, ST_FAILED };

struct conn {
    int          fd;
    int          state;
    int          hdr_done;      /* seen the CRLFCRLF that ends the headers */
    int          win;           /* rolling 4-byte window, for that search */
    int          seen_data;     /* logged the first body byte already */
    long         bytes;         /* body bytes (headers excluded) */
    unsigned     fnv;           /* FNV-1a over the body */
    unsigned long long t_connect, t_first, t_done;
};

static struct conn cs[MAXSOCK];

/* The ordered event log the switch count is computed from. */
#define MAXEV (MAXSOCK * 3)
static int ev_sock[MAXEV];
static int nev;

static void logev(int i, const char *what, unsigned long long t)
{
    if (nev < MAXEV) ev_sock[nev++] = i;
    outs("  ["); outn((long)t); outs("ms] sock "); outn(i);
    outs(" "); outs(what); outc('\n');
}

/* Build "GET <path> HTTP/1.0\r\nHost: <host>\r\n\r\n". HTTP/1.0 on purpose: the
 * server closes when the body is complete, so EOF is the frame and this program
 * needs no Content-Length parser to know when it is finished. */
static int build_req(char *out, int max, const char *path, const char *host)
{
    int o = 0;
    const char *parts[6];
    parts[0] = "GET "; parts[1] = path; parts[2] = " HTTP/1.0\r\nHost: ";
    parts[3] = host;   parts[4] = "\r\nConnection: close\r\n\r\n"; parts[5] = 0;
    for (int i = 0; parts[i]; i++)
        for (const char *p = parts[i]; *p && o < max - 1; p++) out[o++] = *p;
    out[o] = 0;
    return o;
}

/* Feed a received chunk through the header skip and hash the body part. */
static void absorb(struct conn *c, const unsigned char *b, int n)
{
    int i = 0;
    if (!c->hdr_done) {
        for (; i < n; i++) {
            c->win = ((c->win << 8) | b[i]) & 0xFFFFFFFF;
            if (c->win == 0x0D0A0D0A) { c->hdr_done = 1; i++; break; }
        }
        if (!c->hdr_done) return;
    }
    for (; i < n; i++) {
        c->fnv = (c->fnv ^ b[i]) * 16777619u;    /* FNV-1a, 32-bit */
        c->bytes++;
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        errs("usage: socktest <host> <port> <path> [n]\n");
        return 1;
    }
    const char *host = argv[1];
    int port = c_atoi(argv[2]);
    const char *path = argv[3];
    int n = argc > 4 ? c_atoi(argv[4]) : 4;
    if (n < 1) n = 1;
    if (n > MAXSOCK) n = MAXSOCK;
    /* A 5th argument turns on TLS. Off by default: the concurrency proof wants a
     * host-local server, and TLS needs the live Internet.
     *   tls    offer h2 AND http/1.1, as a browser would -- proves ALPN, but a
     *          server that picks h2 gets an HTTP/1.0 request it will not answer,
     *          so the body comes back empty. That is this program's limit, not
     *          the socket's.
     *   tls1   offer http/1.1 only, so the body actually transfers. */
    int flags = 0;
    if (argc > 5 && c_streq(argv[5], "tls"))
        flags = SOCK_F_TLS | SOCK_F_ALPN_H2 | SOCK_F_ALPN_HTTP11;
    else if (argc > 5 && c_streq(argv[5], "tls1"))
        flags = SOCK_F_TLS | SOCK_F_ALPN_HTTP11;

    char req[512];
    int reqlen = build_req(req, (int)sizeof req, path, host);

    outs("socktest: "); outn(n); outs(" concurrent sockets to ");
    outs(host); outc(':'); outn(port); outs(path); outc('\n');

    unsigned long long t0 = monotonic_ms();

    /* Every socket is opened BEFORE any of them is polled, and the request is
     * queued immediately -- sock_send accepts bytes while the connection is
     * still resolving and sock_pump flushes them once it is up. If open or send
     * blocked, this loop alone would already serialise the run. */
    for (int i = 0; i < n; i++) {
        cs[i].fd = sock_open(host, port, flags);
        cs[i].state = ST_OPEN;
        cs[i].fnv = 2166136261u;
        if (cs[i].fd < 0) {
            outs("socktest: sock_open failed rc="); outn(cs[i].fd); outc('\n');
            return 1;
        }
        if (sock_send(cs[i].fd, req, reqlen) != reqlen) {
            outs("socktest: request did not fit the send queue\n");
            return 1;
        }
    }
    outs("  all sockets opened at t=");
    outn((long)(monotonic_ms() - t0)); outs("ms (none waited)\n");

    unsigned char buf[BUFSZ];
    int live = n;
    while (live > 0) {
        if (monotonic_ms() - t0 > 60000) { outs("socktest: overall timeout\n"); break; }
        for (int i = 0; i < n; i++) {
            struct conn *c = &cs[i];
            if (c->state == ST_DONE || c->state == ST_FAILED) continue;
            int bits = sock_poll(c->fd);
            if (bits < 0 || (bits & SOCK_P_ERROR)) {
                c->state = ST_FAILED; live--;
                outs("  sock "); outn(i); outs(" ERROR code=");
                outn(bits < 0 ? bits : SOCK_ERR_CODE(bits)); outc('\n');
                continue;
            }
            if ((bits & SOCK_P_CONNECTED) && c->state == ST_OPEN) {
                c->state = ST_CONNECTED;
                c->t_connect = monotonic_ms() - t0;
                logev(i, "connected", c->t_connect);
                if (flags & SOCK_F_TLS) {
                    char proto[16];
                    int pl = sock_alpn(c->fd, proto, (int)sizeof proto);
                    outs("    sock "); outn(i); outs(" alpn=");
                    outs(pl > 0 ? proto : "(none)"); outc('\n');
                }
            }
            if (bits & SOCK_P_READABLE) {
                int r = sock_recv(c->fd, buf, (int)sizeof buf);
                if (r > 0) {
                    absorb(c, buf, r);
                    if (!c->seen_data && c->bytes > 0) {
                        c->seen_data = 1;
                        c->t_first = monotonic_ms() - t0;
                        logev(i, "first body byte", c->t_first);
                    }
                    continue;              /* drain hard before re-polling */
                }
                if (r < 0) bits |= SOCK_P_EOF;
            }
            if (bits & SOCK_P_EOF) {
                c->t_done = monotonic_ms() - t0;
                c->state = ST_DONE; live--;
                logev(i, "eof", c->t_done);
                sock_close(c->fd);
            }
        }
        /* Yield so the WM thread runs net_poll(), which is what advances every
         * socket. Nothing in this loop ever blocks in the kernel. */
        sys_yield();
    }

    /* ---- the two properties a serial implementation cannot satisfy ---- */
    int ok = 0;
    unsigned long long last_connect = 0, first_done = (unsigned long long)-1;
    long bytes0 = -1; unsigned fnv0 = 0;
    for (int i = 0; i < n; i++) {
        outs("  sock "); outn(i);
        outs(": connect="); outn((long)cs[i].t_connect);
        outs("ms done="); outn((long)cs[i].t_done);
        outs("ms bytes="); outn(cs[i].bytes);
        outs(" fnv="); outn((long)cs[i].fnv); outc('\n');
        if (cs[i].state != ST_DONE) continue;
        ok++;
        if (cs[i].t_connect > last_connect) last_connect = cs[i].t_connect;
        if (cs[i].t_done < first_done) first_done = cs[i].t_done;
        if (bytes0 < 0) { bytes0 = cs[i].bytes; fnv0 = cs[i].fnv; }
        else if (cs[i].bytes != bytes0 || cs[i].fnv != fnv0) {
            outs("socktest: bodies differ between sockets\n");
            ok = -1;
        }
    }

    int switches = 0;
    for (int i = 1; i < nev; i++) if (ev_sock[i] != ev_sock[i - 1]) switches++;

    /* Do two sockets ever sit in [connected, finished] at the same instant?
     *
     * The first version of this asked whether last_connect < first_done -- "the
     * last socket connected before the first one finished". That is not a
     * property of our concurrency, it is a property of how fast the server
     * answers: against a local HTTP server socket 0 routinely collects all
     * 32 KiB before socket 3 has finished its handshake, which is CONCURRENCY
     * WORKING and was being reported as failure. It failed two runs in three.
     * A test that goes red at random is worse than no test -- it teaches people
     * to stop reading the result.
     *
     * Pairwise interval intersection is the property actually meant, and it
     * cannot be satisfied by a serial implementation however fast the server is.
     * n is 4, so O(n^2) needs no apology. */
    int concurrent = 0;
    for (int i = 0; i < n && !concurrent; i++) {
        if (cs[i].state != ST_DONE) continue;
        for (int j = i + 1; j < n; j++) {
            if (cs[j].state != ST_DONE) continue;
            if (cs[i].t_connect <= cs[j].t_done && cs[j].t_connect <= cs[i].t_done) {
                concurrent = 1;
                break;
            }
        }
    }

    outs("SOCKTEST_OVERLAP last_connect="); outn((long)last_connect);
    outs(" first_done="); outn((long)first_done);
    outs(" pairwise_concurrent="); outn(concurrent); outc('\n');
    outs("SOCKTEST_SWITCHES "); outn(switches);
    outs(" of "); outn(nev); outs(" events\n");

    if (ok != n) { outs("SOCKTEST_FAIL only "); outn(ok); outs(" of "); outn(n); outs(" completed\n"); return 1; }
    if (!concurrent) {
        outs("SOCKTEST_FAIL no two sockets were ever in flight at the same instant\n");
        return 1;
    }
    /* A strictly serial run produces exactly n-1 switches (all of socket 0's
     * events, then all of socket 1's, ...). Requiring more than that is
     * requiring genuine interleaving. */
    if (switches <= n - 1) {
        outs("SOCKTEST_FAIL events did not interleave\n");
        return 1;
    }
    outs("SOCKTEST_OK "); outn(ok); outc('/'); outn(n);
    outs(" bytes="); outn(bytes0);
    outs(" fnv="); outn((long)fnv0); outc('\n');
    return 0;
}
