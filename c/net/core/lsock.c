#include <stdint.h>
#include <stddef.h>
#include "lsock.h"
#include "file.h"
#include "tcp.h"
#include "udp.h"
#include "raw.h"
#include "unix.h"            /* AF_UNIX: no wire, no NIC -- see the branch
                             * in lsock_create() below */
#include "net.h"
#include "vfs.h"             /* vfs_may_create -- see lsock_bind_unix */
#include "vfs_cred.h"            /* the privilege check SOCK_RAW needs -- see lsock_create() */
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
    S_RAW,         /* a raw.c ICMP socket (LOGIT_SOCK_RAW) */
    /* AF_UNIX. It is a kind here rather than a second `struct file` type
     * because F_SOCK already means "c/net/core owns this backing pointer", and
     * a second type would mean a second dispatch arm in c/kernel/exec/file.c --
     * a shared, actively-edited file this line has no reason to touch. The
     * state itself is entirely c/net/core/unix.c's; this struct holds only the
     * handle. */
    S_UNIX,
};

struct lsock {
    int  kind;
    int  id;            /* S_LISTEN: listener id. S_CONN: conn id. S_DGRAM: udp
                         * sock. S_RAW: raw.c id. */
    int  type;          /* LOGIT_SOCK_STREAM / LOGIT_SOCK_DGRAM */
    int  pid;
    uint16_t lport;     /* what bind() asked for (0 = any) */
    unsigned rcvtimeo;  /* SO_RCVTIMEO, ms; 0 = wait indefinitely */
    int  reuse;         /* SO_REUSEADDR was set */
    int  rd_shut;       /* SHUT_RD: reads report EOF regardless of the wire */
    struct usock *un;   /* S_UNIX only: the state, which lives in unix.c */
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

    if (s->kind == S_UNIX)
        return unix_read(s->un, buf, len, (f->flags & O_NONBLOCK) != 0);
    if (s->kind == S_DGRAM || s->kind == S_RAW) {
        /* A datagram (or raw ICMP message) read with no sender address:
         * recv(), not recvfrom(). */
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
    if (s->kind == S_UNIX)
        return unix_write(s->un, buf, len, (f->flags & O_NONBLOCK) != 0);
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
    case S_UNIX:   unix_release(s->un);     break;
    case S_RAW:    raw_icmp_close(s->id);   break;
    default: break;
    }
    s->kind = S_FREE;
}

/* ---------------------------------------------------------- the call face */

/* Wrap a `struct usock` in an F_SOCK file. Shared by lsock_create's AF_UNIX
 * branch, by accept() and by socketpair(), because all three have to produce
 * the identical fd-shaped thing and a second copy of these six lines is how one
 * of them ends up without O_RDWR. */
static struct file *unix_file(struct usock *u, int pid, int *err)
{
    struct lsock *s = sk_alloc();
    if (!s) { unix_release(u); if (err) *err = LSK_E_FULL; return NULL; }
    struct file *f = file_alloc();
    if (!f) { s->kind = S_FREE; unix_release(u); if (err) *err = LSK_E_FULL; return NULL; }
    s->kind = S_UNIX;
    s->un = u;
    s->pid = pid;
    f->type = F_SOCK;
    f->amode = O_RDWR;
    f->backing = s;
    if (err) *err = 0;
    return f;
}

struct file *lsock_create(int domain, int type, int protocol, int pid, int *err)
{
    int e = LSK_E_ARG;
    if (domain == LOGIT_AF_UNIX) {
        /* DELIBERATELY BEFORE THE net_up() CHECK BELOW. An AF_UNIX socket
         * touches no NIC, no IP stack and no driver; refusing one because the
         * machine has no network would mean a machine with no NIC cannot run
         * its own daemons, which is the opposite of why this family exists.
         * `protocol` must still be 0 -- there is nothing under it to select,
         * and accepting a number that means nothing would be a value silently
         * ignored. */
        if (protocol != 0) goto fail;
        struct usock *u = unix_create(type, pid, &e);
        if (!u) goto fail;
        return unix_file(u, pid, err);
    }
    if (domain != LOGIT_AF_INET) goto fail;
    int raw = (type == LOGIT_SOCK_RAW);
    if (!raw && type != LOGIT_SOCK_STREAM && type != LOGIT_SOCK_DGRAM) goto fail;
    if (raw) {
        /* ONLY IPPROTO_ICMP -- see raw.h. A protocol number the raw layer
         * does not implement gets a real refusal here rather than a socket
         * that opens, accepts sendto(), and never delivers anything: this
         * tree returns an error instead of stubbing to success. */
        if (protocol != LOGIT_IPPROTO_ICMP) goto fail;
        /* WHO MAY OPEN ONE. A raw socket bypasses the port abstraction: it
         * reads ICMP traffic -- including the reply to a ping another
         * process on this machine started -- that no port number ever
         * scoped to it, and can source a message this machine's own ICMP
         * stack did not build. c/fs/vfs_cred.c already has process
         * credentials and c/fs/vfsctl.c already refuses an unprivileged
         * shell on top of them; reusing that uid rather than inventing a
         * second notion of "privileged" is the point. Said plainly, because
         * vfs_cred.h says it plainly about itself: this machine has no
         * setuid bit, so "root" here means "the login session is root, or
         * nobody has logged in yet" -- not a hardened boundary against a
         * process that was already running as another uid. It is still the
         * ONLY privilege boundary this machine has, and the alternative is
         * no check at all. */
        struct vcred me;
        vfs_cred_get(pid, &me);
        if (me.uid != 0) { e = LSK_E_PERM; goto fail; }
    } else {
        if (protocol != 0) goto fail;
    }
    if (!net_up()) { e = LSK_E_NET; goto fail; }

    struct lsock *s = sk_alloc();
    if (!s) { e = LSK_E_FULL; goto fail; }
    if (raw) {
        int id = raw_icmp_open(pid);
        if (id < 0) { s->kind = S_FREE; e = LSK_E_FULL; goto fail; }
        s->id = id;
        s->kind = S_RAW;          /* usable at once: no bind() needed, see lsock.h */
    }
    struct file *f = file_alloc();
    if (!f) {
        if (raw) raw_icmp_close(s->id);
        s->kind = S_FREE;
        e = LSK_E_FULL;
        goto fail;
    }
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

/* AF_UNIX bind. `canon` is ALREADY RESOLVED against the caller's cwd by
 * c/kernel/exec/syscall.c -- the resolution needs `struct proc`, which neither
 * this file nor unix.c has, and doing it at the syscall site keeps exactly one
 * copy of the rule every other path-taking call in this ABI follows.
 *
 * The DIRECTORY's permissions are the VFS's question, so vfs_may_create()
 * answers it; the NAME's own owner and mode are recorded from this process's
 * credential and umask, and are what connect() is checked against later. */
int lsock_bind_unix(struct file *f, const char *canon)
{
    struct lsock *s = sk_of(f);
    if (!s || !canon) return LSK_E_ARG;
    if (s->kind != S_UNIX) return LSK_E_ARG;
    if (canon[0] && vfs_may_create(canon) < 0) return LSK_E_PERM;
    struct vcred cr;
    vfs_cred_get(s->pid, &cr);
    return unix_bind(s->un, canon, &cr, vmeta_umask(s->pid, -1));
}

/* AF_UNIX connect. Same resolution note as bind above. */
int lsock_connect_unix(struct file *f, const char *canon)
{
    struct lsock *s = sk_of(f);
    if (!s || !canon) return LSK_E_ARG;
    if (s->kind != S_UNIX) return LSK_E_ARG;
    struct vcred cr;
    vfs_cred_get(s->pid, &cr);
    return unix_connect(s->un, canon, &cr);
}

/* AF_UNIX getsockname: a PATH, so it cannot go through the sockaddr form. */
int lsock_getsockname_unix(struct file *f, char *out, int max)
{
    struct lsock *s = sk_of(f);
    if (!s || s->kind != S_UNIX) return LSK_E_ARG;
    return unix_getsockname(s->un, out, max);
}

int lsock_socketpair(int domain, int type, int protocol, int pid,
                     struct file **fa, struct file **fb, int *err)
{
    if (domain != LOGIT_AF_UNIX || protocol != 0) { if (err) *err = LSK_E_ARG; return LSK_E_ARG; }
    struct usock *ua = 0, *ub = 0;
    int e = 0;
    if (unix_pair(type, pid, &ua, &ub, &e) != 0) { if (err) *err = e; return e; }
    struct file *a = unix_file(ua, pid, &e);
    if (!a) { unix_release(ub); if (err) *err = e; return e; }
    struct file *b = unix_file(ub, pid, &e);
    /* If the second descriptor cannot be built the first must go too, whole:
     * handing back one end of a pair is worse than handing back none, because
     * the caller's sv[] is then half-written and it has no way to tell. */
    if (!b) { file_close(a); if (err) *err = e; return e; }
    *fa = a; *fb = b;
    if (err) *err = 0;
    return 0;
}

int lsock_bind(struct file *f, const struct logit_sockaddr *a)
{
    struct lsock *s = sk_of(f);
    if (!s || !a) return LSK_E_ARG;
    /* An AF_INET address on an AF_UNIX socket. Refused here BY NAME rather
     * than falling through to the state check below, which would have said
     * LSK_E_STATE -- "the socket is in the wrong state" -- about a socket whose
     * state is fine and whose address is the wrong family. */
    if (s->kind == S_UNIX) return LSK_E_ARG;
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
    if (s->kind == S_UNIX) return unix_listen(s->un, backlog);
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
    if (s->kind == S_UNIX) {
        /* `timeout_ms` is IGNORED here, and that is a limit rather than an
         * oversight: unix.c's accept parks on a wait queue with no deadline
         * form, and inventing one would mean a timer this layer does not own.
         * A non-blocking descriptor still returns LSK_E_AGAIN at once, which is
         * the form every accept loop in this tree actually uses. */
        struct usock *cu = unix_accept(s->un, (f->flags & O_NONBLOCK) != 0, err);
        if (!cu) return NULL;
        /* An AF_UNIX peer address is a path, and an unnamed client has none --
         * the usual case, since a client rarely binds. `peer` is left UNTOUCHED
         * rather than filled with a fabricated address. */
        (void)peer;
        return unix_file(cu, s->pid, err);
    }
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
    /* An AF_UNIX socket has a path, not an address+port, and there is no honest
     * way to render one as the other. SYS_GETSOCKNAME on one is refused so the
     * caller uses the path form (lsock_getsockname_unix) rather than reading
     * back 0.0.0.0:0 and believing it. */
    if (s->kind == S_UNIX) return LSK_E_ARG;
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
    /* NOT A FALL-THROUGH. Every option below is an AF_INET one: SO_RCVTIMEO
     * would set a field the AF_UNIX path never reads (a value silently
     * ignored), and LOGIT_TCP_NODELAY would call tcp_set_nodelay() with
     * `s->id`, which on an S_UNIX socket is not a connection id at all.
     * Refused by name. */
    if (s->kind == S_UNIX) return LSK_E_ARG;
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
    if (s->kind == S_UNIX) return unix_shutdown(s->un, how);
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
    /* AF_UNIX HAS NO recvfrom WITH AN ADDRESS, and this is where that is said.
     * struct logit_dgram carries family/port/addr and has nowhere to put a
     * 108-byte path, so a sender's name could not be reported even if one
     * existed. A datagram AF_UNIX socket is read with read()/recv(), which is
     * what a syslog daemon does; a client that needs a reply connect()s and is
     * written to through its own descriptor. Widening struct logit_dgram is an
     * ABI change with an AetherScript generator (tools/gen_abi.py) downstream
     * of it, and nothing in this tree needs it yet. */
    if (s->kind == S_UNIX) return LSK_E_ARG;
    if (s->kind != S_DGRAM && s->kind != S_RAW) return LSK_E_STATE;
    uint32_t src = 0; uint16_t sport = 0;
    unsigned waited = 0;
    for (;;) {
        /* ICMP has no ports: a raw socket's `sport` stays 0, which is what
         * from->port reports below -- same convention SYS_NET_PING's
         * pingless ABI already uses (an IP with no port attached). */
        int n = (s->kind == S_RAW) ? raw_icmp_recv(s->id, buf, max, &src)
                                    : udp_recv(s->id, buf, max, &src, &sport);
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
        /* Neither udp.c nor raw.c has a per-socket wake queue, so this is the
         * one wait here that really is a sleep-and-look. Said plainly rather
         * than dressed up: giving either a wait queue is a change to a file
         * this line does not own, and a DNS/DHCP responder -- or a ping --
         * is not latency-critical at 10 ms. */
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
    if (s->kind == S_UNIX) return LSK_E_ARG;   /* see lsock_recvfrom */
    if (s->kind != S_DGRAM && s->kind != S_RAW) return LSK_E_STATE;
    if (to->family != LOGIT_AF_INET) return LSK_E_ARG;
    if (s->kind == S_RAW) {
        /* to->port is ignored: ICMP has none. The caller supplies the whole
         * ICMP message in `buf` (see raw_icmp_send()'s comment on why
         * IP_HDRINCL is refused rather than this call building one for
         * them). */
        int n = raw_icmp_send(s->id, to->addr, buf, len);
        return n < 0 ? LSK_E_NET : n;
    }
    if (udp_send_to(s->id, to->addr, to->port, buf, (uint16_t)len) != 0)
        return LSK_E_NET;
    return len;
}

long lsock_stat(int what)
{
    /* One selector space, split at UNIXSTAT_BASE -- see the comment beside it
     * in include/abi/logit_abi.h. */
    if (what >= UNIXSTAT_BASE) return unix_stat(what);
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
