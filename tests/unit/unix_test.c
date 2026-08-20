/* AF_UNIX, host-side and white-box.
 *
 * WHAT THIS GATE IS AIMED AT. Not "does a socket move bytes" -- a pipe moves
 * bytes and c/kernel/exec/file.c already gates that. Every case below is a
 * property a PIPE DOES NOT HAVE, because those are the properties that would
 * otherwise be assumed rather than checked:
 *
 *   two independent peers      a pipe is one buffer in one direction
 *   a name in the filesystem   a pipe has no name and cannot be found
 *   connect-before-listen      a pipe has no listener to be absent
 *   a second bind refused      a pipe has nothing to collide on
 *   a blocked peer, and close  a pipe HAS this, and it is the one shared
 *                              property, so it is checked here too because
 *                              this is a different implementation of it
 *   message boundaries         a pipe cannot have them at all
 *   permissions on the name    a pipe is reachable only by inheritance
 *
 * WHITE BOX, by #including unix.c -- the same shape tests/unit/tcp_test.c uses
 * on tcp.c, and for the same reason: the states worth checking (a queued but
 * unaccepted connection, a half-closed direction, a recycled slot) are not
 * reachable from the syscall face without a kernel around it.
 *
 * THE STUB THAT MATTERS is tests/unit/unixstub/kernel/core/wait.h -- read its
 * header before adding a blocking case. It turns "this thread parks" into "the
 * other process acts", counts the parks so a test can prove the call really
 * blocked, and ABORTS on a wait nothing satisfies, which is what makes a
 * dropped waitq_wake_all() a failing test rather than a hanging one. */

#include <stdio.h>
#include <string.h>

/* --- the stub state the wait/heap headers declare ------------------------ */
long  ustub_parks = 0;
long  ustub_wakes = 0;
void (*ustub_on_park)(void *) = 0;
void *ustub_park_arg = 0;
const char *ustub_where = 0;
int   ustub_fail_alloc = 0;
long  ustub_live_allocs = 0;

#include "unix.c"           /* the unit under test, whole */

/* --- the harness --------------------------------------------------------- */
static int checks = 0, fails = 0;

#define CHECK(tag, cond) do {                                                  \
        checks++;                                                              \
        if (!(cond)) { fails++; printf("FAIL: %s (%s:%d)\n", tag, __FILE__, __LINE__); } \
    } while (0)

#define CHECK_EQ(tag, got, want) do {                                          \
        long _g = (long)(got), _w = (long)(want);                              \
        checks++;                                                              \
        if (_g != _w) { fails++;                                               \
            printf("FAIL: %s -- got %ld, want %ld (%s:%d)\n", tag, _g, _w,     \
                   __FILE__, __LINE__); }                                      \
    } while (0)

static struct vcred CR_ROOT = { 0, 0 };
static struct vcred CR_A    = { 1000, 1000 };
static struct vcred CR_B    = { 1001, 1001 };

/* Reset the whole layer between groups. socks[] is file-static in unix.c and
 * this file is inside it, which is the other reason the test is white-box. */
static void reset_all(void)
{
    for (int i = 0; i < NUSOCK; i++)
        if (socks[i].used) unix_release(&socks[i]);
    ustub_on_park = 0; ustub_park_arg = 0;
}

/* ===================================================================== 1 ===
 * TWO INDEPENDENT PEERS. The single property that separates a socketpair from
 * a pipe: both ends write, both ends read, and neither reads its own bytes. */
static void t_twoway(void)
{
    struct usock *a = 0, *b = 0; int err = 0;
    char buf[64];

    CHECK_EQ("twoway: pair created", unix_pair(LOGIT_SOCK_STREAM, 1, &a, &b, &err), 0);
    CHECK_EQ("twoway: pair err", err, 0);

    CHECK_EQ("twoway: a->b write", unix_write(a, "ping", 4, 1), 4);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("twoway: b reads a", unix_read(b, buf, sizeof buf, 1), 4);
    CHECK("twoway: b got ping", memcmp(buf, "ping", 4) == 0);

    /* THE HALF A PIPE CANNOT DO. */
    CHECK_EQ("twoway: b->a write", unix_write(b, "pong!", 5, 1), 5);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("twoway: a reads b", unix_read(a, buf, sizeof buf, 1), 5);
    CHECK("twoway: a got pong", memcmp(buf, "pong!", 5) == 0);

    /* And a writer never sees its own bytes come back. */
    CHECK_EQ("twoway: a is empty again", unix_read(a, buf, sizeof buf, 1), EAGAIN_RC);
    CHECK_EQ("twoway: b is empty again", unix_read(b, buf, sizeof buf, 1), EAGAIN_RC);

    unix_release(a); unix_release(b);
    reset_all();
}

/* ===================================================================== 2 ===
 * MESSAGE BOUNDARIES. A stream coalesces; a datagram and a seqpacket do not.
 * These are the checks the "alias SOCK_SEQPACKET to SOCK_STREAM" shortcut
 * would turn red, which is why they carry their own tag. */
static void t_bounds(void)
{
    struct usock *a = 0, *b = 0; int err = 0;
    char buf[64];

    unix_pair(LOGIT_SOCK_STREAM, 1, &a, &b, &err);
    unix_write(a, "abc", 3, 1);
    unix_write(a, "de", 2, 1);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("bounds: stream coalesces", unix_read(b, buf, sizeof buf, 1), 5);
    CHECK("bounds: stream bytes", memcmp(buf, "abcde", 5) == 0);
    unix_release(a); unix_release(b);

    unix_pair(LOGIT_SOCK_DGRAM, 1, &a, &b, &err);
    unix_write(a, "abc", 3, 1);
    unix_write(a, "de", 2, 1);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("bounds: dgram keeps rec 1", unix_read(b, buf, sizeof buf, 1), 3);
    CHECK("bounds: dgram rec 1 bytes", memcmp(buf, "abc", 3) == 0);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("bounds: dgram keeps rec 2", unix_read(b, buf, sizeof buf, 1), 2);
    CHECK("bounds: dgram rec 2 bytes", memcmp(buf, "de", 2) == 0);
    unix_release(a); unix_release(b);

    unix_pair(LOGIT_SOCK_SEQPACKET, 1, &a, &b, &err);
    unix_write(a, "hello", 5, 1);
    unix_write(a, "world!", 6, 1);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("bounds: seqpacket rec 1", unix_read(b, buf, sizeof buf, 1), 5);
    CHECK("bounds: seqpacket rec 1 bytes", memcmp(buf, "hello", 5) == 0);
    /* A short buffer TRUNCATES and the remainder is DISCARDED -- the next read
     * must see the NEXT record, never the tail of this one. */
    memset(buf, 0, sizeof buf);
    CHECK_EQ("bounds: seqpacket truncates", unix_read(b, buf, 3, 1), 3);
    CHECK("bounds: seqpacket truncated bytes", memcmp(buf, "wor", 3) == 0);
    CHECK_EQ("bounds: seqpacket tail discarded", unix_read(b, buf, sizeof buf, 1), EAGAIN_RC);
    unix_release(a); unix_release(b);

    reset_all();
}

/* ===================================================================== 3 ===
 * A NAME IN THE FILESYSTEM: bind, getsockname, connect by that name, accept. */
static void t_name(void)
{
    int err = 0;
    char nm[LOGIT_UNIX_PATH_MAX];

    struct usock *srv = unix_create(LOGIT_SOCK_STREAM, 1, &err);
    CHECK("name: server created", srv != 0);
    CHECK_EQ("name: bind", unix_bind(srv, "/var/run/log", &CR_ROOT, 022), 0);
    CHECK_EQ("name: getsockname ok", unix_getsockname(srv, nm, sizeof nm), 0);
    CHECK("name: getsockname value", strcmp(nm, "/var/run/log") == 0);
    CHECK_EQ("name: bound name counted", unix_stat(UNIXSTAT_NAMES), 1);
    CHECK_EQ("name: listen", unix_listen(srv, 4), 0);

    struct usock *cli = unix_create(LOGIT_SOCK_STREAM, 2, &err);
    CHECK_EQ("name: connect by path", unix_connect(cli, "/var/run/log", &CR_ROOT), 0);

    /* The client may write BEFORE the server accepts -- connect() returns as
     * soon as the connection is queued, which is what Linux does and what a
     * client that writes a request immediately depends on. */
    CHECK_EQ("name: client writes pre-accept", unix_write(cli, "GET", 3, 1), 3);

    struct usock *conn = unix_accept(srv, 1, &err);
    CHECK("name: accept produced a socket", conn != 0);
    CHECK("name: accepted != listener", conn != srv);
    char buf[16]; memset(buf, 0, sizeof buf);
    CHECK_EQ("name: server reads pre-accept bytes", unix_read(conn, buf, sizeof buf, 1), 3);
    CHECK("name: server got GET", memcmp(buf, "GET", 3) == 0);

    /* An accepted connection is a REAL socket, not a view of the listener: it
     * has no name of its own. */
    CHECK_EQ("name: accepted has no name", unix_getsockname(conn, nm, sizeof nm), 0);
    CHECK("name: accepted name empty", nm[0] == 0);

    unix_release(conn); unix_release(cli); unix_release(srv);
    CHECK_EQ("name: name released with socket", unix_stat(UNIXSTAT_NAMES), 0);
    reset_all();
}

/* ===================================================================== 4 ===
 * THE REFUSALS. Each is a DIFFERENT code, and each bumps its own counter --
 * which is what lets this test assert the refusal fired for the reason it
 * claims rather than for some other one. */
static void t_refusals(void)
{
    int err = 0;
    long n0;

    /* (a) connect() to a path nobody holds. */
    n0 = unix_stat(UNIXSTAT_REFUSED_NAME);
    struct usock *c1 = unix_create(LOGIT_SOCK_STREAM, 2, &err);
    CHECK_EQ("refuse: no such name", unix_connect(c1, "/nope", &CR_ROOT), LSK_E_CONNREFUSED);
    CHECK_EQ("refuse: no-name counted", unix_stat(UNIXSTAT_REFUSED_NAME) - n0, 1);

    /* (b) CONNECT BEFORE LISTEN: the path is bound and the socket is not
     * listening. A different failure from (a) and it must say so. */
    struct usock *srv = unix_create(LOGIT_SOCK_STREAM, 1, &err);
    CHECK_EQ("refuse: bind for no-listen", unix_bind(srv, "/tmp/s", &CR_ROOT, 0), 0);
    n0 = unix_stat(UNIXSTAT_REFUSED_LISTEN);
    CHECK_EQ("refuse: connect before listen", unix_connect(c1, "/tmp/s", &CR_ROOT),
             LSK_E_CONNREFUSED);
    CHECK_EQ("refuse: no-listen counted", unix_stat(UNIXSTAT_REFUSED_LISTEN) - n0, 1);
    CHECK_EQ("refuse: connect left socket unconnected", c1->state, U_OPEN);

    /* (c) A SECOND BIND TO A LIVE PATH. */
    struct usock *srv2 = unix_create(LOGIT_SOCK_STREAM, 3, &err);
    n0 = unix_stat(UNIXSTAT_REFUSED_INUSE);
    CHECK_EQ("refuse: second bind", unix_bind(srv2, "/tmp/s", &CR_ROOT, 0), LSK_E_INUSE);
    CHECK_EQ("refuse: inuse counted", unix_stat(UNIXSTAT_REFUSED_INUSE) - n0, 1);
    /* ...and the SAME path binds once the holder is gone. Without this the
     * check above would also pass on an implementation that never releases a
     * name, which would be a leak wearing a refusal's clothes. */
    unix_release(srv);
    CHECK_EQ("refuse: rebind after release", unix_bind(srv2, "/tmp/s", &CR_ROOT, 0), 0);

    /* (d) TYPE MISMATCH: a stream may not connect to a datagram name. */
    struct usock *dg = unix_create(LOGIT_SOCK_DGRAM, 4, &err);
    CHECK_EQ("refuse: dgram bind", unix_bind(dg, "/tmp/d", &CR_ROOT, 0), 0);
    CHECK_EQ("refuse: stream->dgram", unix_connect(c1, "/tmp/d", &CR_ROOT), LSK_E_ARG);

    /* (e) The abstract namespace is refused, not silently taken as "". */
    struct usock *ab = unix_create(LOGIT_SOCK_STREAM, 5, &err);
    CHECK_EQ("refuse: abstract namespace", unix_bind(ab, "", &CR_ROOT, 0), LSK_E_ARG);

    /* (f) bind() twice on one socket. */
    CHECK_EQ("refuse: rebind same socket", unix_bind(srv2, "/tmp/other", &CR_ROOT, 0),
             LSK_E_STATE);

    /* (g) listen() on a datagram socket has nothing to accept. */
    CHECK_EQ("refuse: listen on dgram", unix_listen(dg, 4), LSK_E_STATE);

    /* (h) accept() on a socket that is not listening. */
    CHECK("refuse: accept on non-listener", unix_accept(srv2, 1, &err) == 0);
    CHECK_EQ("refuse: accept state code", err, LSK_E_STATE);

    reset_all();
}

/* ===================================================================== 5 ===
 * PERMISSIONS ON THE NAME. The reason binding under a root-owned directory
 * means anything at all. */
static void t_perm(void)
{
    int err = 0;
    long n0;

    /* umask 077 -> mode 0700: only the owner may connect. */
    struct usock *srv = unix_create(LOGIT_SOCK_STREAM, 1, &err);
    CHECK_EQ("perm: bind 0700", unix_bind(srv, "/tmp/private", &CR_A, 077), 0);
    CHECK_EQ("perm: mode recorded", (long)srv->mode, 0700);
    unix_listen(srv, 4);

    n0 = unix_stat(UNIXSTAT_REFUSED_PERM);
    struct usock *other = unix_create(LOGIT_SOCK_STREAM, 2, &err);
    CHECK_EQ("perm: stranger refused", unix_connect(other, "/tmp/private", &CR_B),
             LSK_E_PERM);
    CHECK_EQ("perm: refusal counted", unix_stat(UNIXSTAT_REFUSED_PERM) - n0, 1);

    struct usock *owner = unix_create(LOGIT_SOCK_STREAM, 3, &err);
    CHECK_EQ("perm: owner allowed", unix_connect(owner, "/tmp/private", &CR_A), 0);

    struct usock *root = unix_create(LOGIT_SOCK_STREAM, 4, &err);
    CHECK_EQ("perm: root allowed", unix_connect(root, "/tmp/private", &CR_ROOT), 0);

    /* umask 0 -> 0777: the stranger gets in. The control for the check above,
     * because "refused" that is really "always refused" proves nothing. */
    struct usock *open_srv = unix_create(LOGIT_SOCK_STREAM, 5, &err);
    CHECK_EQ("perm: bind 0777", unix_bind(open_srv, "/tmp/open", &CR_A, 0), 0);
    unix_listen(open_srv, 4);
    struct usock *o2 = unix_create(LOGIT_SOCK_STREAM, 6, &err);
    CHECK_EQ("perm: stranger allowed on 0777", unix_connect(o2, "/tmp/open", &CR_B), 0);

    reset_all();
}

/* ===================================================================== 6 ===
 * A BLOCKED PEER, AND WHAT THE OTHER END'S CLOSE DOES TO IT.
 *
 * Every case here asserts BOTH the answer and that the call actually parked.
 * Without the park count, an implementation that returns EOF immediately --
 * i.e. never blocks at all -- passes every one of them. */
static struct usock *g_peer;
static void park_release_peer(void *p) { (void)p; unix_release(g_peer); }
static void park_write_peer(void *p)
{
    (void)p;
    static int once = 0;
    if (!once) { once = 1; unix_write(g_peer, "late", 4, 1); }
}
static void park_shutwr_peer(void *p) { (void)p; unix_shutdown(g_peer, LOGIT_SHUT_WR); }

static void t_block(void)
{
    struct usock *a = 0, *b = 0; int err = 0;
    char buf[64];
    long p0;

    /* (a) Reader parks on an empty stream; the peer CLOSES -> EOF. */
    unix_pair(LOGIT_SOCK_STREAM, 1, &a, &b, &err);
    g_peer = a; ustub_on_park = park_release_peer; ustub_where = "block: close wakes reader";
    p0 = ustub_parks;
    CHECK_EQ("block: close -> EOF", unix_read(b, buf, sizeof buf, 0), 0);
    CHECK("block: the reader really parked", ustub_parks > p0);
    ustub_on_park = 0;
    unix_release(b);

    /* (b) Reader parks; the peer WRITES -> the bytes arrive. */
    unix_pair(LOGIT_SOCK_STREAM, 1, &a, &b, &err);
    g_peer = a; ustub_on_park = park_write_peer; ustub_where = "block: write wakes reader";
    p0 = ustub_parks;
    memset(buf, 0, sizeof buf);
    CHECK_EQ("block: write -> data", unix_read(b, buf, sizeof buf, 0), 4);
    CHECK("block: reader parked before data", ustub_parks > p0);
    CHECK("block: data is right", memcmp(buf, "late", 4) == 0);
    ustub_on_park = 0;
    unix_release(a); unix_release(b);

    /* (c) HALF CLOSE. The peer shuts down its WRITE side: the reader gets EOF,
     * and the connection is still usable in the other direction. A close would
     * kill both, which is why shutdown is a separate flag. */
    unix_pair(LOGIT_SOCK_STREAM, 1, &a, &b, &err);
    g_peer = a; ustub_on_park = park_shutwr_peer; ustub_where = "block: shutdown wakes reader";
    p0 = ustub_parks;
    CHECK_EQ("block: SHUT_WR -> EOF", unix_read(b, buf, sizeof buf, 0), 0);
    CHECK("block: reader parked before shutdown", ustub_parks > p0);
    ustub_on_park = 0;
    CHECK_EQ("block: other direction still open", unix_write(b, "reply", 5, 1), 5);
    CHECK_EQ("block: half-closed peer still reads", unix_read(a, buf, sizeof buf, 1), 5);
    unix_release(a); unix_release(b);

    /* (d) DATA WRITTEN BEFORE THE CLOSE IS STILL DELIVERED. EOF is tested after
     * the buffer, not instead of it. */
    unix_pair(LOGIT_SOCK_STREAM, 1, &a, &b, &err);
    unix_write(a, "last words", 10, 1);
    unix_release(a);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("block: drain after close", unix_read(b, buf, sizeof buf, 0), 10);
    CHECK("block: drained bytes", memcmp(buf, "last words", 10) == 0);
    CHECK_EQ("block: EOF after drain", unix_read(b, buf, sizeof buf, 0), 0);
    unix_release(b);

    /* (e) WRITING TO A PEER THAT IS GONE is EPIPE, not a silent success into a
     * buffer nobody will read. */
    unix_pair(LOGIT_SOCK_STREAM, 1, &a, &b, &err);
    unix_release(a);
    CHECK_EQ("block: write to dead peer", unix_write(b, "x", 1, 1), -1);
    unix_release(b);

    /* (f) A writer parks on a FULL buffer and the reader drains it. */
    unix_pair(LOGIT_SOCK_STREAM, 1, &a, &b, &err);
    static char big[UNIX_BUF];
    memset(big, 'z', sizeof big);
    CHECK_EQ("block: fill the ring", unix_write(a, big, UNIX_BUF, 1), UNIX_BUF);
    CHECK_EQ("block: ring is full", unix_write(a, "x", 1, 1), EAGAIN_RC);
    unix_release(a); unix_release(b);

    reset_all();
}

/* ===================================================================== 7 ===
 * THE LISTENER: backlog, two independent clients, and what a listener's death
 * does to a connection it never accepted. */
static void t_listener(void)
{
    int err = 0;
    char buf[32];

    struct usock *srv = unix_create(LOGIT_SOCK_STREAM, 1, &err);
    unix_bind(srv, "/tmp/srv", &CR_ROOT, 0);
    CHECK_EQ("listen: backlog 2", unix_listen(srv, 2), 0);

    struct usock *c1 = unix_create(LOGIT_SOCK_STREAM, 2, &err);
    struct usock *c2 = unix_create(LOGIT_SOCK_STREAM, 3, &err);
    struct usock *c3 = unix_create(LOGIT_SOCK_STREAM, 4, &err);
    CHECK_EQ("listen: c1 queues", unix_connect(c1, "/tmp/srv", &CR_ROOT), 0);
    CHECK_EQ("listen: c2 queues", unix_connect(c2, "/tmp/srv", &CR_ROOT), 0);
    CHECK_EQ("listen: c3 over backlog", unix_connect(c3, "/tmp/srv", &CR_ROOT), LSK_E_AGAIN);

    /* TWO CLIENTS, TWO CONNECTIONS. The tcp server gate's own negative control
     * is an accept that reuses one connection block; the same mistake here
     * would cross these two streams. */
    unix_write(c1, "one", 3, 1);
    unix_write(c2, "two", 3, 1);
    struct usock *s1 = unix_accept(srv, 1, &err);
    struct usock *s2 = unix_accept(srv, 1, &err);
    CHECK("listen: two accepted sockets", s1 && s2 && s1 != s2);
    CHECK("listen: two distinct connections", s1->conn != s2->conn);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("listen: s1 reads 3", unix_read(s1, buf, sizeof buf, 1), 3);
    CHECK("listen: s1 got one", memcmp(buf, "one", 3) == 0);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("listen: s2 reads 3", unix_read(s2, buf, sizeof buf, 1), 3);
    CHECK("listen: s2 got two", memcmp(buf, "two", 3) == 0);
    CHECK_EQ("listen: queue drained", unix_accept(srv, 1, &err) == 0 ? err : 0, LSK_E_AGAIN);

    unix_release(s1); unix_release(s2); unix_release(c1); unix_release(c2);

    /* THE LISTENER DIES WITH A CONNECTION STILL QUEUED. The client is already
     * "connected" and must get an EOF, not a park forever and not a read out of
     * freed memory. */
    CHECK_EQ("listen: c3 queues now", unix_connect(c3, "/tmp/srv", &CR_ROOT), 0);
    unix_release(srv);
    CHECK_EQ("listen: orphaned client sees EOF", unix_read(c3, buf, sizeof buf, 0), 0);
    CHECK_EQ("listen: orphaned client write is EPIPE", unix_write(c3, "x", 1, 1), -1);
    unix_release(c3);

    reset_all();
}

/* ===================================================================== 8 ===
 * NAMED DATAGRAMS: many senders, one inbox, boundaries and order kept, and the
 * sender finding out when the daemon goes away. This is the syslog shape. */
static struct usock *g_drain;
static void park_drain_one(void *p)
{
    (void)p;
    char t[8];
    unix_read(g_drain, t, sizeof t, 1);
}

static void t_dgram(void)
{
    int err = 0;
    char buf[64];

    struct usock *d = unix_create(LOGIT_SOCK_DGRAM, 1, &err);
    CHECK_EQ("dgram: bind inbox", unix_bind(d, "/dev/log", &CR_ROOT, 0), 0);

    struct usock *p1 = unix_create(LOGIT_SOCK_DGRAM, 2, &err);
    struct usock *p2 = unix_create(LOGIT_SOCK_DGRAM, 3, &err);
    /* An unconnected datagram socket has no destination and says so. */
    CHECK_EQ("dgram: send with no peer", unix_write(p1, "x", 1, 1), LSK_E_STATE);
    CHECK_EQ("dgram: p1 connect", unix_connect(p1, "/dev/log", &CR_ROOT), 0);
    CHECK_EQ("dgram: p2 connect", unix_connect(p2, "/dev/log", &CR_ROOT), 0);

    CHECK_EQ("dgram: p1 sends", unix_write(p1, "from-1", 6, 1), 6);
    CHECK_EQ("dgram: p2 sends", unix_write(p2, "two", 3, 1), 3);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("dgram: rec 1 length", unix_read(d, buf, sizeof buf, 1), 6);
    CHECK("dgram: rec 1 bytes", memcmp(buf, "from-1", 6) == 0);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("dgram: rec 2 length", unix_read(d, buf, sizeof buf, 1), 3);
    CHECK("dgram: rec 2 bytes", memcmp(buf, "two", 3) == 0);
    CHECK_EQ("dgram: inbox empty", unix_read(d, buf, sizeof buf, 1), EAGAIN_RC);

    /* A record bigger than the ring can never fit and is refused rather than
     * truncated. */
    static char huge[UNIX_BUF + 16];
    memset(huge, 'h', sizeof huge);
    CHECK_EQ("dgram: oversize refused", unix_write(p1, huge, sizeof huge, 1), LSK_E_ARG);

    /* A FULL INBOX: the sender parks and the daemon draining it lets it through.
     * UNIX_RECS records of one byte fill the record ring before the byte ring. */
    for (int i = 0; i < UNIX_RECS; i++) unix_write(p1, "f", 1, 1);
    CHECK_EQ("dgram: inbox full", unix_write(p1, "f", 1, 1), EAGAIN_RC);
    g_drain = d; ustub_on_park = park_drain_one; ustub_where = "dgram: drain wakes sender";
    long p0 = ustub_parks;
    CHECK_EQ("dgram: blocked send completes", unix_write(p1, "g", 1, 0), 1);
    CHECK("dgram: sender really parked", ustub_parks > p0);
    ustub_on_park = 0;

    /* THE DAEMON DIES. The destination is re-resolved on every send, so this is
     * ECONNREFUSED and not a write into a slot that now belongs to somebody
     * else. Proved by rebinding the same path to a DIFFERENT socket after: a
     * cached peer pointer would have been left aimed at the dead one. */
    unix_release(d);
    CHECK_EQ("dgram: send to dead daemon", unix_write(p1, "x", 1, 1), LSK_E_CONNREFUSED);

    struct usock *d2 = unix_create(LOGIT_SOCK_DGRAM, 9, &err);
    CHECK_EQ("dgram: daemon restarts on same path", unix_bind(d2, "/dev/log", &CR_ROOT, 0), 0);
    CHECK_EQ("dgram: sender reaches the new one", unix_write(p1, "again", 5, 1), 5);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("dgram: new daemon reads it", unix_read(d2, buf, sizeof buf, 1), 5);
    CHECK("dgram: new daemon bytes", memcmp(buf, "again", 5) == 0);

    reset_all();
}

/* ===================================================================== 9 ===
 * SEQPACKET IS CONNECTION-ORIENTED. It is the stream path with records on, so
 * listen/accept must work on it -- an implementation that routed it through the
 * datagram path would fail here while passing every boundary check above. */
static void t_seqpacket_conn(void)
{
    int err = 0;
    char buf[64];

    struct usock *srv = unix_create(LOGIT_SOCK_SEQPACKET, 1, &err);
    CHECK_EQ("seq: bind", unix_bind(srv, "/tmp/seq", &CR_ROOT, 0), 0);
    CHECK_EQ("seq: listen", unix_listen(srv, 2), 0);
    struct usock *cli = unix_create(LOGIT_SOCK_SEQPACKET, 2, &err);
    CHECK_EQ("seq: connect", unix_connect(cli, "/tmp/seq", &CR_ROOT), 0);
    struct usock *conn = unix_accept(srv, 1, &err);
    CHECK("seq: accepted", conn != 0);
    unix_write(cli, "aa", 2, 1);
    unix_write(cli, "bbbb", 4, 1);
    memset(buf, 0, sizeof buf);
    CHECK_EQ("bounds: seq accepted rec 1", unix_read(conn, buf, sizeof buf, 1), 2);
    CHECK_EQ("bounds: seq accepted rec 2", unix_read(conn, buf, sizeof buf, 1), 4);
    /* And EOF still works on it -- a record socket that could not end would
     * leave a server parked forever. */
    unix_release(cli);
    CHECK_EQ("seq: EOF after peer close", unix_read(conn, buf, sizeof buf, 0), 0);
    reset_all();
}

/* ==================================================================== 10 ===
 * EXHAUSTION AND FAILED ALLOCATION. Both are paths a host with 16 GiB never
 * reaches by accident, and both must leave nothing half-built behind. */
static void t_exhaust(void)
{
    int err = 0;
    long n0 = unix_stat(UNIXSTAT_REFUSED_FULL);

    struct usock *held[NUSOCK];
    int n = 0;
    for (; n < NUSOCK; n++) {
        held[n] = unix_create(LOGIT_SOCK_STREAM, 1, &err);
        if (!held[n]) break;
    }
    CHECK_EQ("full: table holds NUSOCK", n, NUSOCK);
    err = 0;
    CHECK("full: one more refused", unix_create(LOGIT_SOCK_STREAM, 1, &err) == 0);
    CHECK_EQ("full: refusal code", err, LSK_E_FULL);
    CHECK_EQ("full: refusal counted", unix_stat(UNIXSTAT_REFUSED_FULL) - n0, 1);
    for (int i = 0; i < n; i++) unix_release(held[i]);
    CHECK_EQ("full: all released", unix_stat(UNIXSTAT_SOCKS), 0);

    /* A connection whose buffer pair cannot be allocated must NOT leave the
     * client marked connected -- it would then write into a NULL conn. */
    struct usock *srv = unix_create(LOGIT_SOCK_STREAM, 1, &err);
    unix_bind(srv, "/tmp/oom", &CR_ROOT, 0);
    unix_listen(srv, 2);
    struct usock *cli = unix_create(LOGIT_SOCK_STREAM, 2, &err);
    ustub_fail_alloc = 1;
    CHECK_EQ("full: connect with no memory", unix_connect(cli, "/tmp/oom", &CR_ROOT),
             LSK_E_FULL);
    ustub_fail_alloc = 0;
    CHECK_EQ("full: client not left connected", cli->state, U_OPEN);
    CHECK("full: client has no connection", cli->conn == 0);
    CHECK_EQ("full: nothing queued on the listener", srv->qn, 0);

    /* Same for a datagram inbox. */
    struct usock *dg = unix_create(LOGIT_SOCK_DGRAM, 3, &err);
    ustub_fail_alloc = 1;
    CHECK_EQ("full: dgram bind with no memory", unix_bind(dg, "/tmp/oom2", &CR_ROOT, 0),
             LSK_E_FULL);
    ustub_fail_alloc = 0;
    CHECK("full: dgram left unbound", dg->name[0] == 0);
    CHECK_EQ("full: no name leaked", unix_stat(UNIXSTAT_NAMES), 1);   /* only /tmp/oom */

    reset_all();
}

/* ==================================================================== 11 ===
 * NON-BLOCKING. The same code EAGAIN_RC that a non-blocking pipe read returns,
 * deliberately -- a second convention for "would block" is the kind of thing
 * that costs somebody an afternoon (logit_abi.h says so about LSK_E_AGAIN). */
static void t_nonblock(void)
{
    int err = 0;
    char buf[16];
    struct usock *a = 0, *b = 0;
    unix_pair(LOGIT_SOCK_STREAM, 1, &a, &b, &err);
    CHECK_EQ("nb: empty read", unix_read(a, buf, sizeof buf, 1), EAGAIN_RC);
    unix_release(a); unix_release(b);

    struct usock *srv = unix_create(LOGIT_SOCK_STREAM, 1, &err);
    unix_bind(srv, "/tmp/nb", &CR_ROOT, 0);
    unix_listen(srv, 2);
    err = 0;
    CHECK("nb: empty accept", unix_accept(srv, 1, &err) == 0);
    CHECK_EQ("nb: empty accept code", err, LSK_E_AGAIN);
    reset_all();
}

int main(void)
{
    t_twoway();
    t_bounds();
    t_name();
    t_refusals();
    t_perm();
    t_block();
    t_listener();
    t_dgram();
    t_seqpacket_conn();
    t_exhaust();
    t_nonblock();

    /* Every buffer this layer allocated must be back. A socket that leaks its
     * connection is invisible from every check above -- the bytes still move. */
    CHECK_EQ("leak: no live allocations", ustub_live_allocs, 0);

    printf("unix: %d/%d checks, %ld parks, %ld wakes\n",
           checks - fails, checks, ustub_parks, ustub_wakes);
    if (fails) { printf("UNIX-FAIL %d\n", fails); return 1; }
    printf("UNIX-OK\n");
    return 0;
}
