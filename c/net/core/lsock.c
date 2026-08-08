#include <stdint.h>
#include <stddef.h>
#include "lsock.h"
#include "file.h"
#include "tcp.h"
#include "udp.h"
#include "net.h"
#include "kernel/core/wait.h"   /* sched_sleep_ms -- the one wait here that is a sleep */

void *memset(void *, int, size_t);

/* Per-descriptor socket state. Kept in its own table rather than as new fields
 * on `struct file` on purpose: file.c is a shared, actively-edited file and the
 * fewer lines this line puts in it the better. `struct file`'s `backing` points
 * here; the three lsock_file_* hooks are the only thing file.c calls.
 *
 * 32 -- one per connection slot in tcp.c, which is the real ceiling anyway, plus
 * the listeners that produced them. ~24 bytes each. */
#define NLSOCK 40

enum {
    S_FREE = 0,
    S_UNBOUND,     /* created, no address */
    S_BOUND,       /* bind() done, not listening */
    S_LISTEN,      /* a tcp.c listener */
    S_CONN,        /* a tcp.c connection (accepted, or connected) */
    S_DGRAM,       /* a udp.c bound port */
};

struct lsock {
    int  kind;
    int  id;            /* S_LISTEN: listener id. S_CONN: conn id. S_DGRAM: udp sock. */
    int  type;          /* LOGIT_SOCK_STREAM / LOGIT_SOCK_DGRAM */
    int  pid;
    uint16_t lport;     /* what bind() asked for (0 = any) */
    unsigned rcvtimeo;  /* SO_RCVTIMEO, ms; 0 = wait indefinitely */
    int  reuse;         /* SO_REUSEADDR was set */
    int  rd_shut;       /* SHUT_RD: reads report EOF regardless of the wire */
};

static struct lsock socks[NLSOCK];
static uint32_t st_reuse_set;      /* SO_REUSEADDR requests, so it is not silent */

/* How long one park lasts before the caller re-tests. NOT a poll interval in
 * the usual sense: a wake normally arrives first and this is only the backstop
 * (see the rx_wq note in c/net/transport/tcp.c). It bounds how late a missed
 * wake can be, and 200 ms of worst-case latency on a wake that should not have
 * been missed is the right trade against waking every blocked thread more
 * often than that for nothing. */
#define PARK_MS 200

static struct lsock *sk_alloc(void)
{
    for (int i = 0; i < NLSOCK; i++)
        if (socks[i].kind == S_FREE) {
            memset(&socks[i], 0, sizeof socks[i]);
            socks[i].kind = S_UNBOUND;
            return &socks[i];
        }
    return NULL;
}

static struct lsock *sk_of(struct file *f)
{
    if (!f || f->type != F_SOCK || !f->backing) return NULL;
    return (struct lsock *)f->backing;
}

/* ------------------------------------------------------------- the fd face */

long lsock_file_read(struct file *f, void *buf, long len)
{
    struct lsock *s = sk_of(f);
    if (!s || len < 0) return -1;
    if (len == 0) return 0;

    if (s->kind == S_DGRAM) {
        /* A datagram read with no sender address: recv(), not recvfrom(). */
        return lsock_recvfrom(f, buf, (int)len, NULL);
    }
    if (s->kind != S_CONN) return -1;      /* read() on a listener is meaningless */
    if (s->rd_shut) return 0;              /* SHUT_RD: EOF by our own decision */

    int cap = len > 0x7fffffff ? 0x7fffffff : (int)len;
    for (;;) {
        int n = tcp_recv(s->id, buf, cap);
        if (n > 0) return n;
        if (n < 0) return 0;               /* peer FIN drained, or gone: EOF */
        if (f->flags & O_NONBLOCK) return EAGAIN_RC;
        /* Park. The thread is unlinked from the run ring while it waits; what
         * wakes it is the segment carrying the bytes, delivered by the NIC
         * interrupt's softirq -- so this does not depend on the window manager
         * running. (The RESPONSE going back out does; see the SYS_ACCEPT note
         * in include/abi/logit_abi.h.) */
        int r = tcp_wait_readable(s->id, s->rcvtimeo ? s->rcvtimeo : PARK_MS);
        if (r < 0) return 0;
        if (r == 0 && s->rcvtimeo) return EAGAIN_RC;   /* the caller set a deadline */
    }
}

long lsock_file_write(struct file *f, const void *buf, long len)
{
    struct lsock *s = sk_of(f);
    if (!s || len < 0) return -1;
    if (len == 0) return 0;
    if (s->kind != S_CONN) return -1;

    const uint8_t *p = (const uint8_t *)buf;
    long sent = 0;
    while (sent < len) {
        int chunk = (len - sent) > 0x7fffffff ? 0x7fffffff : (int)(len - sent);
        int n = tcp_send_nb(s->id, p + sent, chunk);
        if (n < 0) return sent > 0 ? sent : -1;      /* connection gone */
        if (n == 0) {
            if (f->flags & O_NONBLOCK) return sent > 0 ? sent : EAGAIN_RC;
            /* The send ring is full: what empties it is the peer's ACK. */
            if (tcp_wait_writable(s->id, PARK_MS) < 0)
                return sent > 0 ? sent : -1;
            continue;
        }
        sent += n;
    }
    return sent;
}

void lsock_file_release_backing(void *backing)
{
    struct lsock *s = (struct lsock *)backing;
    if (!s || s->kind == S_FREE) return;
    switch (s->kind) {
    case S_LISTEN: tcp_listen_close(s->id); break;
    case S_CONN:   tcp_close(s->id);        break;
    case S_DGRAM:  udp_close(s->id);        break;
    default: break;
    }
    s->kind = S_FREE;
}

/* ---------------------------------------------------------- the call face */

struct file *lsock_create(int domain, int type, int protocol, int pid, int *err)
{
    int e = LSK_E_ARG;
    if (domain != LOGIT_AF_INET) goto fail;
    if (type != LOGIT_SOCK_STREAM && type != LOGIT_SOCK_DGRAM) goto fail;
    if (protocol != 0) goto fail;
    if (!net_up()) { e = LSK_E_NET; goto fail; }

    struct lsock *s = sk_alloc();
    if (!s) { e = LSK_E_FULL; goto fail; }
    struct file *f = file_alloc();
    if (!f) { s->kind = S_FREE; e = LSK_E_FULL; goto fail; }
    s->type = type;
    s->pid = pid;
    f->type = F_SOCK;
    f->amode = O_RDWR;
    f->backing = s;
    if (err) *err = 0;
    return f;
fail:
    if (err) *err = e;
    return NULL;
}

int lsock_bind(struct file *f, const struct logit_sockaddr *a)
{
    struct lsock *s = sk_of(f);
    if (!s || !a) return LSK_E_ARG;
    if (a->family != LOGIT_AF_INET) return LSK_E_ARG;
    if (s->kind != S_UNBOUND) return LSK_E_STATE;
    /* addr is accepted and not enforced: this machine has exactly one address
     * (net_cfg.ip), so INADDR_ANY and its own address are the same set, and a
     * bind to some third address should fail rather than silently listen
     * everywhere. */
    if (a->addr != 0 && a->addr != net_cfg.ip) return LSK_E_ARG;

    if (s->type == LOGIT_SOCK_DGRAM) {
        int id = udp_bind(a->port);
        if (id < 0) return LSK_E_INUSE;
        s->id = id;
        s->kind = S_DGRAM;
        s->lport = a->port;
        return 0;
    }
    /* A stream socket's port is claimed at listen(): tcp.c's listener table is
     * where a TCP port is owned, and taking it here would mean a bind()-only
     * socket held a listening port it never listened on. */
    s->lport = a->port;
    s->kind = S_BOUND;
    return 0;
}

int lsock_listen(struct file *f, int backlog)
{
    struct lsock *s = sk_of(f);
    if (!s) return LSK_E_ARG;
    if (s->type != LOGIT_SOCK_STREAM) return LSK_E_STATE;
    if (s->kind == S_LISTEN) return 0;             /* idempotent, as POSIX is */
    if (s->kind != S_BOUND) return LSK_E_STATE;    /* listen() before bind() */
    int id = tcp_listen(s->lport, backlog, s->pid);
    if (id == TCP_L_E_INUSE) return LSK_E_INUSE;
    if (id == TCP_L_E_FULL)  return LSK_E_FULL;
    if (id < 0)              return LSK_E_ARG;
    s->id = id;
    s->kind = S_LISTEN;
    return 0;
}

struct file *lsock_accept(struct file *f, struct logit_sockaddr *peer,
                          unsigned timeout_ms, int *err)
{
    struct lsock *s = sk_of(f);
    if (!s) { if (err) *err = LSK_E_ARG; return NULL; }
    if (s->kind != S_LISTEN) { if (err) *err = LSK_E_STATE; return NULL; }

    int cid;
    if (f->flags & O_NONBLOCK) {
        cid = tcp_accept(s->id);
    } else if (timeout_ms) {
        cid = tcp_accept_wait(s->id, timeout_ms);
    } else {
        /* Wait indefinitely, in PARK_MS naps. Each nap is a real park -- the
         * thread is off the run ring -- and the loop exists so that a listener
         * closed under us (tcp_accept returning LSK_E_ARG) still gets out. */
        for (;;) {
            cid = tcp_accept_wait(s->id, PARK_MS);
            if (cid != TCP_L_E_AGAIN) break;
        }
    }
    if (cid == TCP_L_E_AGAIN) { if (err) *err = LSK_E_AGAIN; return NULL; }
    if (cid < 0)              { if (err) *err = LSK_E_ARG;   return NULL; }

    struct lsock *cs = sk_alloc();
    if (!cs) { tcp_close(cid); if (err) *err = LSK_E_FULL; return NULL; }
    struct file *cf = file_alloc();
    if (!cf) { cs->kind = S_FREE; tcp_close(cid); if (err) *err = LSK_E_FULL; return NULL; }
    cs->kind = S_CONN;
    cs->id = cid;
    cs->type = LOGIT_SOCK_STREAM;
    cs->pid = s->pid;
    cf->type = F_SOCK;
    cf->amode = O_RDWR;
    cf->backing = cs;
    /* The accepted descriptor does NOT inherit the listener's O_NONBLOCK. That
     * is BSD behaviour and it is also the behaviour that surprises people less:
     * a blocking accept loop that suddenly produced non-blocking connections
     * would fail as a truncated response, not as an error. */
    if (peer) {
        uint32_t ip = 0; uint16_t pt = 0;
        tcp_peer(cid, &ip, &pt);
        peer->family = LOGIT_AF_INET;
        peer->port = pt;
        peer->addr = ip;
    }
    if (err) *err = 0;
    return cf;
}

int lsock_getsockname(struct file *f, struct logit_sockaddr *out)
{
    struct lsock *s = sk_of(f);
    if (!s || !out) return LSK_E_ARG;
    out->family = LOGIT_AF_INET;
    out->addr = net_cfg.ip;
    if (s->kind == S_LISTEN) {
        int p = tcp_listen_port(s->id);
        out->port = (unsigned short)(p < 0 ? 0 : p);
    } else {
        out->port = s->lport;
    }
    return 0;
}

int lsock_getpeername(struct file *f, struct logit_sockaddr *out)
{
    struct lsock *s = sk_of(f);
    if (!s || !out) return LSK_E_ARG;
    if (s->kind != S_CONN) return LSK_E_STATE;
    uint32_t ip = 0; uint16_t pt = 0;
    if (tcp_peer(s->id, &ip, &pt) != 0) return LSK_E_STATE;
    out->family = LOGIT_AF_INET;
    out->addr = ip;
    out->port = pt;
    return 0;
}

int lsock_setsockopt(struct file *f, int level, int optname, long value)
{
    struct lsock *s = sk_of(f);
    if (!s) return LSK_E_ARG;
    if (level != LOGIT_SOL_SOCKET) return LSK_E_ARG;
    switch (optname) {
    case LOGIT_SO_REUSEADDR:
        /* Recorded, counted, and deliberately NOT made to do anything clever.
         * A listener here is torn down synchronously and holds no TIME_WAIT of
         * its own, so the state SO_REUSEADDR exists to override does not arise;
         * a port that reports LSK_E_INUSE is genuinely in use and rebinding
         * over it would be a bug, not a convenience. Accepting the call keeps
         * ported source compiling; the counter keeps the acceptance honest. */
        s->reuse = value ? 1 : 0;
        if (s->reuse) st_reuse_set++;
        return 0;
    case LOGIT_SO_RCVTIMEO:
        if (value < 0) return LSK_E_ARG;
        s->rcvtimeo = (unsigned)value;
        return 0;
    case LOGIT_TCP_NODELAY:
        if (s->kind != S_CONN) return LSK_E_STATE;
        tcp_set_nodelay(s->id, value ? 1 : 0);
        return 0;
    default:
        return LSK_E_ARG;
    }
}

int lsock_shutdown(struct file *f, int how)
{
    struct lsock *s = sk_of(f);
    if (!s) return LSK_E_ARG;
    if (s->kind != S_CONN) return LSK_E_STATE;
    if (how == LOGIT_SHUT_RD || how == LOGIT_SHUT_RDWR) s->rd_shut = 1;
    if (how == LOGIT_SHUT_WR || how == LOGIT_SHUT_RDWR) tcp_shutdown_write(s->id);
    if (how != LOGIT_SHUT_RD && how != LOGIT_SHUT_WR && how != LOGIT_SHUT_RDWR)
        return LSK_E_ARG;
    return 0;
}

int lsock_recvfrom(struct file *f, void *buf, int max, struct logit_sockaddr *from)
{
    struct lsock *s = sk_of(f);
    if (!s || !buf || max <= 0) return LSK_E_ARG;
    if (s->kind != S_DGRAM) return LSK_E_STATE;
    uint32_t src = 0; uint16_t sport = 0;
    unsigned waited = 0;
    for (;;) {
        int n = udp_recv(s->id, buf, max, &src, &sport);
        if (n > 0) {
            if (from) {
                from->family = LOGIT_AF_INET;
                from->addr = src;
                from->port = sport;
            }
            return n;
        }
        if (n < 0) return LSK_E_NET;             /* an ICMP error, reported once */
        if (f->flags & O_NONBLOCK) return LSK_E_AGAIN;
        /* UDP has no per-socket wake in c/net/transport/udp.c, so this is the
         * one wait here that really is a sleep-and-look. Said plainly rather
         * than dressed up: giving udp.c a wait queue is a change to a file this
         * line does not own, and a DNS/DHCP responder is not latency-critical
         * at 10 ms. */
        sched_sleep_ms(10);
        waited += 10;
        if (s->rcvtimeo && waited >= s->rcvtimeo) return LSK_E_AGAIN;
    }
}

int lsock_sendto(struct file *f, const void *buf, int len,
                 const struct logit_sockaddr *to)
{
    struct lsock *s = sk_of(f);
    if (!s || !buf || len < 0 || !to) return LSK_E_ARG;
    if (s->kind != S_DGRAM) return LSK_E_STATE;
    if (to->family != LOGIT_AF_INET) return LSK_E_ARG;
    if (udp_send_to(s->id, to->addr, to->port, buf, (uint16_t)len) != 0)
        return LSK_E_NET;
    return len;
}

long lsock_stat(int what)
{
    struct tcp_server_stats st;
    tcp_server_stats(&st);
    switch (what) {
    case SOCKSTAT_SYN_RECEIVED:    return st.syn_received;
    case SOCKSTAT_ACCEPTED:        return st.accepted;
    case SOCKSTAT_REFUSED_BACKLOG: return st.refused_backlog;
    case SOCKSTAT_REFUSED_SLOTS:   return st.refused_slots;
    case SOCKSTAT_REFUSED_NOPORT:  return st.refused_noport;
    case SOCKSTAT_FREE_CONNS:      return st.free_conns;
    case SOCKSTAT_LISTENERS:       return st.listeners;
    default:                       return -1;
    }
}

void lsock_close_owner(int pid)
{
    /* Listeners only. Connections are held by file descriptors, and process
     * teardown closes those already -- reaching in here would double-free
     * them. A listener is different: the port it owns lives in tcp.c and would
     * otherwise stay claimed for the rest of the boot if the fd table teardown
     * ever missed it. */
    tcp_listen_close_owner(pid);
    for (int i = 0; i < NLSOCK; i++)
        if (socks[i].kind == S_LISTEN && socks[i].pid == pid)
            socks[i].kind = S_FREE;
}
