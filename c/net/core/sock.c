#include <stdint.h>
#include <stddef.h>
#include "sock.h"
#include "tls.h"            /* tls_send/tls_recv/tls_close: unchanged, already
                             * non-blocking-shaped (see tls_step.h for the rest) */
#include "tls_step.h"
#include "net.h"
#include "tcp.h"
#include "dns.h"
#include "arp.h"
#include "eth.h"
#include "pit.h"
#include "rtc.h"

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);

/* 16 sockets against 32 TCP slots. Deliberately fewer than NCONN: a socket that
 * has been closed hands its TCP slot to FIN_WAIT/TIME_WAIT for a while after the
 * handle is gone, so sizing the two tables equally would let a burst of opens
 * fail on tcp_connect_start even though every socket handle was free. The
 * remaining slots also keep the legacy blocking SYS_HTTP_GET path openable while
 * a pool is busy. */
#define NSOCK       16

/* One HTTP request (with cookies) or one TLS-layer write fits comfortably; the
 * ABI says sock_send may be short, so a bigger queue would buy nothing but BSS.
 * Must be a power of two -- the ring indexes with &. */
#define TXQ         4096

/* Phase deadlines, in 100 Hz ticks. TCP has its own retransmit cap and DNS its
 * own timeout; these are the backstop for the case where a phase makes no
 * progress at all, so they are generous rather than tight. The TLS budget is the
 * big one on purpose: a 4096-bit RSA chain verified by our own bignum code under
 * QEMU's TCG is genuinely slow. */
#define T_RESOLVE   400     /* 4 s  */
#define T_ARP       30      /* 0.3 s, then connect anyway and let TCP retransmit */
#define T_CONNECT   700     /* 7 s  */
#define T_TLS       2000    /* 20 s */

enum { S_FREE, S_RESOLVE, S_ARP, S_CONNECT, S_TLS, S_READY, S_DEAD };

struct sock {
    int      used;
    int      pid;               /* owning process; sockets die with it */
    int      state;
    int      flags;             /* SOCK_F_* as the app passed them */
    int      err;               /* SOCK_E_*, once state == S_DEAD with err set */
    uint32_t ip;
    uint16_t port;
    int      dnsq;              /* dns pool query id, or -1 */
    int      tcp;               /* tcp connection id, or -1 */
    int      tls;               /* tls session id, or -1 */
    uint64_t t0;                /* when the current phase began */
    uint64_t arp_last;
    int      shut;              /* app called sock_close: drain, then FIN */
    char     host[SOCK_HOST_MAX];
    char     alpn[16];          /* negotiated protocol, NUL-terminated */
    int      alpn_len;
    uint8_t  txq[TXQ];
    int      txh, txn;          /* ring head + queued byte count */
};

static struct sock socks[NSOCK];

/* Wall clock as unix-ish seconds on the same proleptic-Gregorian, no-leap-second
 * scale x509.c's parse_time uses, so cert validity windows compare correctly.
 * (net/http/http.c computes the same thing for the blocking path; that file
 * belongs to the HTTP client and is not ours to re-plumb, so the twenty lines
 * live in both places rather than one of them reaching into the other.) */
static int64_t now_unix(void)
{
    struct rtc_time t; rtc_now(&t);
    int64_t days = 0;
    for (int y = 1970; y < t.year; y++)
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    static const int md[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    for (int m = 1; m < t.month && m <= 12; m++) {
        days += md[m - 1];
        if (m == 2 && (t.year % 4 == 0 && (t.year % 100 != 0 || t.year % 400 == 0))) days++;
    }
    days += t.day - 1;
    return ((days * 24 + t.hour) * 60 + t.minute) * 60 + t.second;
}

/* ---- handle plumbing ---------------------------------------------------- */

/* A handle is only valid for the process that opened it. Without this, socket
 * numbers would be a global namespace and one process could read another's
 * response body by guessing a small integer. pid < 0 is the kernel's own
 * override (teardown paths). */
static struct sock *lookup(int fd, int pid)
{
    if (fd < 0 || fd >= NSOCK) return NULL;
    struct sock *s = &socks[fd];
    if (!s->used) return NULL;
    /* A shut socket still exists (sock_pump is flushing its tail) but is gone as
     * far as its owner is concerned -- otherwise sock_close would be followed by
     * a window in which the handle still answered. */
    if (s->shut) return NULL;
    if (pid >= 0 && s->pid != pid) return NULL;
    return s;
}

static void drop_transport(struct sock *s);

/* The handle survives -- the app still has to be able to read WHY it failed --
 * but the transport underneath it goes immediately. TCP connections and TLS
 * sessions are the scarce resources (32 and 8 of them); a socket handle is a
 * few hundred bytes. An app that notices the error and forgets to close should
 * not be able to exhaust the connection table by doing so. */
static void fail(struct sock *s, int err)
{
    drop_transport(s);
    s->err = err;
    s->state = S_DEAD;
}

/* Release the transport underneath a socket. The handle itself may live on (the
 * app still has to be told what happened); only sock_release() frees that. */
static void drop_transport(struct sock *s)
{
    if (s->dnsq >= 0) { dns_query_free(s->dnsq); s->dnsq = -1; }
    if (s->tls >= 0)  { tls_close(s->tls); s->tls = -1; }
    if (s->tcp >= 0)  { tcp_close(s->tcp); s->tcp = -1; }
}

static void sock_release(struct sock *s)
{
    drop_transport(s);
    memset(s, 0, sizeof *s);
    s->dnsq = s->tcp = s->tls = -1;
}

/* ---- the transmit ring -------------------------------------------------- */

static int txq_put(struct sock *s, const uint8_t *p, int len)
{
    int space = TXQ - s->txn;
    int n = len < space ? len : space;
    for (int i = 0; i < n; i++)
        s->txq[(s->txh + s->txn + i) & (TXQ - 1)] = p[i];
    s->txn += n;
    return n;
}

/* Hand the head of the ring to the transport. Contiguous run only: one segment
 * goes out per pump anyway, so splicing across the wrap would buy nothing. */
static void txq_flush(struct sock *s)
{
    while (s->txn > 0) {
        int run = TXQ - s->txh;                  /* bytes before the ring wraps */
        if (run > s->txn) run = s->txn;
        int n = (s->tls >= 0) ? tls_send(s->tls, s->txq + s->txh, run)
                              : tcp_send_nb(s->tcp, s->txq + s->txh, run);
        if (n < 0) { fail(s, (s->tls >= 0) ? SOCK_E_TLS : SOCK_E_CONN); return; }
        if (n == 0) return;                      /* would block: try next pump */
        s->txh = (s->txh + n) & (TXQ - 1);
        s->txn -= n;
    }
}

/* ---- the state machine -------------------------------------------------- */

/* The next hop's IP: the destination itself on our subnet, otherwise the
 * gateway. Warming ITS arp entry (not the destination's) is what stops the
 * first SYN being dropped on a cold cache. */
static uint32_t next_hop(uint32_t ip)
{
    return ((ip & net_cfg.mask) == (net_cfg.ip & net_cfg.mask)) ? ip : net_cfg.gw;
}

static void start_connect(struct sock *s)
{
    s->tcp = tcp_connect_start(s->ip, s->port);
    if (s->tcp < 0) { fail(s, SOCK_E_NOSLOT); return; }
    s->state = S_CONNECT;
    s->t0 = timer_ticks();
}

static void pump_one(struct sock *s)
{
    uint64_t now = timer_ticks();

    switch (s->state) {
    case S_RESOLVE: {
        uint32_t r = dns_query_result(s->dnsq);
        if (r == 0) {
            if (now - s->t0 > T_RESOLVE) fail(s, SOCK_E_DNS);
            return;
        }
        dns_query_free(s->dnsq); s->dnsq = -1;
        if (r == 0xFFFFFFFFu) { fail(s, SOCK_E_DNS); return; }
        s->ip = r;
        s->state = S_ARP;
        s->t0 = now;
        s->arp_last = 0;
        return;
    }

    case S_ARP: {
        /* arp_warm() would do this, but it BLOCKS -- it pumps net_poll, and we
         * are inside net_poll. So the probe is fired here instead, at arp_warm's
         * own ~100 ms rate, and the phase gives up after T_ARP: a still-cold
         * cache only costs one TCP SYN retransmit, it does not fail the open.
         *
         * The rate limit is not cosmetic. arp_resolve() BROADCASTS on a miss as
         * well as answering from the cache, so calling it once per pump would
         * put a hundred ARP requests a second on the wire per pending socket.
         * arp_last == 0 means "not probed yet", so the first pump probes at
         * once and a cache hit connects with no delay at all. */
        if (s->arp_last && now - s->arp_last < 10) return;
        s->arp_last = now;
        uint8_t mac[ETH_ALEN];
        if (arp_resolve(next_hop(s->ip), mac) == 0) { start_connect(s); return; }
        if (now - s->t0 > T_ARP) start_connect(s);
        return;
    }

    case S_CONNECT: {
        int st = tcp_connect_status(s->tcp);
        if (st == 0) {
            if (now - s->t0 > T_CONNECT) { tcp_close(s->tcp); s->tcp = -1; fail(s, SOCK_E_CONN); }
            return;
        }
        if (st < 0) { s->tcp = -1; fail(s, SOCK_E_CONN); return; }

        if (!(s->flags & SOCK_F_TLS)) { s->state = S_READY; s->t0 = now; return; }
        /* Weak (see tls_step.h): if the stepped TLS side is not linked, fail the
         * socket rather than send plaintext to port 443. */
        if (!tls_start || !tls_step || !tls_alpn) { fail(s, SOCK_E_TLS); return; }
        s->tls = tls_start(s->tcp, s->host, tls_alpn_list(s->flags), now_unix());
        if (s->tls < 0) { s->tls = -1; fail(s, SOCK_E_TLS); return; }
        s->state = S_TLS;
        s->t0 = now;
        return;
    }

    case S_TLS: {
        int rc = tls_step(s->tls);
        if (rc == TLS_DONE) {
            s->alpn_len = tls_alpn(s->tls, s->alpn, (int)sizeof s->alpn);
            if (s->alpn_len < 0 || s->alpn_len >= (int)sizeof s->alpn) s->alpn_len = 0;
            s->alpn[s->alpn_len] = 0;
            s->state = S_READY;
            s->t0 = now;
            return;
        }
        /* tls.h: anything negative is a terminal TLS_E_*, and the session slot
         * stays held until tls_close -- which drop_transport does. */
        if (rc < 0)              { fail(s, SOCK_E_TLS); return; }
        if (now - s->t0 > T_TLS) { fail(s, SOCK_E_TLS); return; }
        return;                   /* WANT_READ / WANT_WRITE: more next pump */
    }

    case S_READY:
        txq_flush(s);
        /* A closed socket the app can no longer see, kept alive only long enough
         * to push out what it had already queued -- then the handle is freed.
         * The deadline matters: a peer that never acknowledges would otherwise
         * strand the slot, which is the exact failure the blocking path needed a
         * 100-second watchdog for. */
        if (s->shut && (s->txn == 0 || now - s->t0 > T_CONNECT))
            sock_release(s);
        return;

    default:
        return;
    }
}

void sock_pump(void)
{
    /* Re-entrancy guard. Everything below is non-blocking by construction, but
     * one mistake -- a tcp_send() instead of a tcp_send_nb(), a tls_step() that
     * pumps net_poll -- would re-enter this walk with sockets half-advanced. The
     * flag makes that a no-op rather than a corrupted state machine. */
    static int pumping;
    if (pumping) return;
    pumping = 1;
    for (int i = 0; i < NSOCK; i++)
        if (socks[i].used) pump_one(&socks[i]);
    pumping = 0;
}

/* ---- the API ------------------------------------------------------------ */

int sock_open(const char *host, int port, int flags, int pid)
{
    if (!host || !*host || port <= 0 || port > 65535) return SOCK_E_ARG;
    if (!net_up()) return SOCK_E_CONN;

    int fd = -1;
    for (int i = 0; i < NSOCK; i++) if (!socks[i].used) { fd = i; break; }
    if (fd < 0) return SOCK_E_NOSLOT;
    struct sock *s = &socks[fd];
    memset(s, 0, sizeof *s);
    s->dnsq = s->tcp = s->tls = -1;

    int n = 0;
    while (host[n] && n < SOCK_HOST_MAX - 1) { s->host[n] = host[n]; n++; }
    s->host[n] = 0;
    if (host[n]) return SOCK_E_ARG;              /* host name too long */

    s->used  = 1;
    s->pid   = pid;
    s->flags = flags;
    s->port  = (uint16_t)port;
    s->state = S_RESOLVE;
    s->t0    = timer_ticks();
    s->dnsq  = dns_query_start(s->host);
    if (s->dnsq < 0) { s->used = 0; return SOCK_E_DNS; }
    /* Note there is no wait here at all: a cache hit or an IP literal is already
     * resolved and the very next net_poll() will move this socket to S_ARP. */
    return fd;
}

int sock_poll_bits(int fd, int pid)
{
    struct sock *s = lookup(fd, pid);
    if (!s) return SOCK_E_ARG;

    if (s->state == S_DEAD)
        return s->err ? (SOCK_P_ERROR | ((-s->err & 0xFF) << 8)) : SOCK_P_EOF;
    if (s->state != S_READY)
        return 0;                                 /* still getting there */

    int bits = SOCK_P_CONNECTED;
    if (s->txn < TXQ) bits |= SOCK_P_WRITABLE;

    int avail = tcp_available(s->tcp);
    if (s->tls >= 0) {
        /* Plaintext already decrypted counts even when the wire buffer is empty;
         * without tls_pending we can only see the wire, which is right except in
         * that one case (documented in tls_step.h). */
        int pend = tls_pending ? tls_pending(s->tls) : 0;
        if (pend > 0) bits |= SOCK_P_READABLE;
        else if (avail > 0) bits |= SOCK_P_READABLE;
        else if (avail < 0) bits |= SOCK_P_EOF;
    } else {
        if (avail > 0) bits |= SOCK_P_READABLE;
        else if (avail < 0) bits |= SOCK_P_EOF;
    }
    return bits;
}

int sock_send(int fd, const void *buf, int len, int pid)
{
    struct sock *s = lookup(fd, pid);
    if (!s || len < 0 || (!buf && len > 0)) return SOCK_E_ARG;
    if (s->state == S_DEAD) return SOCK_E_CONN;
    if (len == 0) return 0;
    /* Bytes written before the connection is up are legal and queued: an app
     * can hand over its request the moment it opens the socket and let
     * sock_pump() send it when the handshake finishes. */
    int n = txq_put(s, (const uint8_t *)buf, len);
    if (s->state == S_READY) txq_flush(s);       /* head start; may take nothing */
    return n;
}

int sock_recv(int fd, void *buf, int max, int pid)
{
    struct sock *s = lookup(fd, pid);
    if (!s || max <= 0 || !buf) return SOCK_E_ARG;
    if (s->state == S_DEAD) return -1;
    if (s->state != S_READY) return 0;           /* not connected yet: nothing */
    /* Straight through to the transport -- no second copy of the payload. Both
     * of these are non-blocking (tcp_recv locks and copies; tls_recv is required
     * not to block, see tls_step.h), which is what makes this safe to call from
     * inside the int 0x80 gate with interrupts off. */
    int n = (s->tls >= 0) ? tls_recv(s->tls, buf, max) : tcp_recv(s->tcp, buf, max);
    return n;
}

int sock_alpn(int fd, char *buf, int max, int pid)
{
    struct sock *s = lookup(fd, pid);
    if (!s || max <= 0 || !buf) return SOCK_E_ARG;
    int n = s->alpn_len;
    if (n > max - 1) n = max - 1;
    memcpy(buf, s->alpn, (size_t)n);
    buf[n] = 0;
    return n;
}

int sock_close(int fd, int pid)
{
    struct sock *s = lookup(fd, pid);
    if (!s) return SOCK_E_ARG;
    /* Queued bytes are not thrown away: mark the socket shut and let sock_pump()
     * finish flushing before the FIN. A request written and immediately closed
     * (the natural shape for a one-shot fetch) must still reach the server. */
    if (s->state == S_READY && s->txn > 0) { s->shut = 1; s->t0 = timer_ticks(); return 0; }
    sock_release(s);
    return 0;
}

void sock_close_owner(int pid)
{
    for (int i = 0; i < NSOCK; i++)
        if (socks[i].used && socks[i].pid == pid)
            sock_release(&socks[i]);
}
