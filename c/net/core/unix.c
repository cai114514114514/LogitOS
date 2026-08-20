#include <stdint.h>
#include <stddef.h>
#include "unix.h"
#include "logit_abi.h"          /* LOGIT_SOCK_*, LSK_E_*, UNIXSTAT_*, O_*, SIG* */
#include "vfs_meta.h"           /* struct vcred, VM_IW* -- the mode a name carries */
#include "kheap.h"              /* kmalloc/kfree: the buffers, never the table */
#include "kernel/core/wait.h"   /* path-qualified for the reason file.c gives:
                                 * mini-libc ships a <wait.h> that sorts first */

void *memset(void *, int, size_t);

/* SIGPIPE on a write to a peer that is gone, and EINTR on a signalled wait --
 * declared WEAK for exactly the reason c/kernel/exec/file.c declares the lsock
 * hooks weak: this file is compiled into a host gate that has no signal
 * delivery and no process to deliver to, and a hard reference would make the
 * gate fail to link over machinery it never exercises. NULL means "this build
 * has no signals", which the call sites already handle. */
int  ksig_post_current(int signo) __attribute__((weak));
int  ksig_interrupted(void)       __attribute__((weak));
static int sig_interrupted(void) { return ksig_interrupted ? ksig_interrupted() : 0; }

/* ===========================================================================
 * THE NAMESPACE: WHERE A BOUND PATH LIVES, AND WHAT THE OTHER ANSWER COSTS
 *
 * A named AF_UNIX socket has a path. The two places that path could live are a
 * REAL VFS NODE with a socket type, or a namespace of this layer's own. This
 * is the second, and the choice is not obvious enough to leave unargued.
 *
 * WHAT A REAL NODE WOULD COST, priced against this tree rather than in the
 * abstract. `struct vattr` (c/fs/vfs_meta.h) has exactly three types --
 * VT_REG, VT_DIR, VT_LNK -- and no fourth; `struct filesystem` (c/fs/vfs.h)
 * has no mknod op at all, only write/mkdir/symlink; and LogitFS's on-disk
 * inode has no type field a socket could set. So a real node means editing
 * c/fs/vfs.c, c/fs/vfs_meta.{h,c}, c/fs/logitfs.c AND tools/mkfs.py, and
 * `test-fs-format` asserts every on-disk offset against a real image, so it
 * means a FORMAT BUMP -- for a name that never holds a byte. Two of those
 * files are ones CLAUDE.md names as actively rewritten by another line.
 *
 * AND THE CHEAP VERSION OF IT IS WORSE THAN NOTHING. The tempting shortcut is
 * to have bind() create an ordinary ZERO-LENGTH FILE at the path, so `ls`
 * shows it. Then `cat /var/run/log` succeeds and prints nothing, `open()`
 * hands back an empty regular file, and every probe of the path learns
 * something false; on a real Unix, open() on a socket is ENXIO. A node that
 * lies about what it is, is worse than a name that is only in one place --
 * the same rule this tree applies to a stub that returns 0.
 *
 * WHAT THIS CHOICE COSTS, said plainly rather than discovered later:
 *   - the name does NOT appear in `ls`, and stat() on it fails with ENOENT.
 *   - `unlink(path)` does NOT release the binding. A daemon's customary
 *     `unlink(path); bind(path)` therefore gets ENOENT from the unlink -- which
 *     every daemon ignores, because on a real system the file may not be there
 *     -- and then binds. The stale-socket case that idiom exists for cannot
 *     arise here anyway: a binding is owned by its socket, the socket dies with
 *     the last fd, and process teardown closes fds. There is no on-disk residue
 *     to clean up because there is nothing on disk.
 *   - two names that resolve to the same file through a hard link are two
 *     DIFFERENT names here. Nothing in the tree binds through a link.
 *
 * PERMISSIONS ARE NOT SKIPPED, which is the part that would otherwise make
 * this a toy. bind() asks the REAL VFS whether the caller may create the name
 * (vfs_may_create, at the syscall site -- so the containing directory's mode is
 * enforced by the code that owns that question), and the binding then CARRIES
 * the creator's uid/gid and 0777 & ~umask. connect() checks write permission
 * against that, which is what makes a socket under a root-owned /var/run mean
 * something.
 * =========================================================================== */

/* 48 sockets. A socket costs a table slot (~250 B) and, once connected, one
 * kmalloc'd buffer pair. The ceiling that matters is not this number but
 * NFILE/NFD -- every one of these is an fd -- so 48 is chosen to sit safely
 * above what the fd tables can usefully hold open at once, not to be a
 * resource limit of its own. UNIXSTAT_REFUSED_FULL is what would say
 * otherwise, which is why it is a counter and not a kprintf. */
#define NUSOCK       48
#define UNIX_BACKLOG 8

/* A RESOLVED path, which is NOT LOGIT_UNIX_PATH_MAX. The ABI field a program
 * fills is 108 bytes because that is what every ported program assumes; what
 * arrives here is that name run through proc_resolve() against the caller's
 * cwd, and c/kernel/exec/syscall.c resolves into a 128-byte buffer like every
 * other path-taking call. Storing the resolved form in 108 would TRUNCATE, and
 * a truncated path is not a shorter name -- it is a name that silently
 * collides with a different one. Anything that does not fit is refused. */
#define UNIX_NAME_MAX 128

/* One direction of one connection, or one bound datagram socket's inbox.
 *
 * 4 KiB of bytes and 32 record lengths. The byte ring is HALF a pipe's (8 KiB)
 * because a connection has TWO of them and a socketpair should not cost four
 * times what a pipe costs to do the same job. The record ring is what a pipe
 * cannot have and is the entire reason this is not `struct pipe`: a datagram
 * is a length as well as bytes, and a byte ring cannot say where one ends. */
#define UNIX_BUF   4096
#define UNIX_RECS  32

struct uchan {
    unsigned char  buf[UNIX_BUF];
    int            head, tail, count;
    unsigned short rec[UNIX_RECS];
    int            rhead, rtail, rcount;
};

/* Per-endpoint state of a connection. `wr_shut` is "this side will write no
 * more" (shutdown(SHUT_WR), or the side going away); `rd_shut` is "this side
 * will read no more" (shutdown(SHUT_RD)), which is what turns the PEER's write
 * into EPIPE; `gone` is the descriptor closed for good. Three flags and not
 * one, because a half-closed connection is the normal shape of a request/
 * response protocol and collapsing them makes shutdown(SHUT_WR) a close. */
struct uend { int gone, wr_shut, rd_shut; };

struct uconn {
    int          refs;          /* 2 while both ends live; the accept queue
                                 * holds the server side's reference until
                                 * accept() takes it */
    int          records;       /* SOCK_DGRAM / SOCK_SEQPACKET */
    struct uend  e[2];
    struct uchan ch[2];         /* ch[i] is WRITTEN by side i, read by side 1-i */
    struct waitq wq;            /* data, room, and either side going away */
};

enum { U_FREE = 0, U_OPEN, U_BOUND, U_LISTEN, U_CONN };

struct usock {
    int  used;
    int  type;                  /* LOGIT_SOCK_STREAM / _DGRAM / _SEQPACKET */
    int  state;
    int  pid;
    char name[UNIX_NAME_MAX];         /* resolved bound path, "" if unbound */
    unsigned mode, uid, gid;          /* of the bound name */

    struct uconn *conn;         /* stream/seqpacket, and a datagram socketpair */
    int           side;

    struct uconn *q[UNIX_BACKLOG];    /* listener: queued, not yet accepted */
    int           qn, qmax;

    struct uchan *rx;           /* bound datagram socket's inbox */
    char          peer[UNIX_NAME_MAX];        /* connect()ed datagram destination */
    int           has_peer;

    /* One queue for accept() and for a datagram inbox; the two never coexist
     * on one socket, and wait_event re-tests its predicate so sharing costs a
     * spurious wake at worst.
     *
     * `wq_ready` is NOT redundant. wait.h says in as many words that
     * waitq_init() assigns a WHOLE NEW LOCK, so calling it on a queue anything
     * might be using resets that lock's ticket and leaves a queued core
     * spinning on a number that will never be served. A slot here is recycled
     * the moment its socket is released, and a thread woken by that release
     * has not necessarily re-acquired the queue lock yet -- so the queue is
     * initialised ONCE PER SLOT, for the life of the boot, and reuse merely
     * inherits an empty one. */
    struct waitq  wq;
    int           wq_ready;
};

static struct usock socks[NUSOCK];

static long st_refused_name, st_refused_listen, st_refused_perm;
static long st_refused_inuse, st_refused_full, st_accepted, st_dgrams;

/* ------------------------------------------------------------ small helpers */

static int pstreq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void pstrcpy(char *d, const char *s, int max)
{
    int i = 0;
    for (; i < max - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

/* Would `s` fit in UNIX_NAME_MAX with its NUL? Checked rather than truncated --
 * see UNIX_NAME_MAX above for why a truncated path is a correctness bug and
 * not a cosmetic one. */
static int pfits(const char *s)
{
    int i = 0;
    while (s[i]) if (++i >= UNIX_NAME_MAX) return 0;
    return 1;
}

static struct usock *sk_alloc(int type, int pid)
{
    for (int i = 0; i < NUSOCK; i++) {
        if (socks[i].used) continue;
        struct usock *s = &socks[i];
        /* The wait queue survives the wipe -- see wq_ready above. Everything
         * else is cleared, so a recycled slot cannot inherit a name, a peer
         * path or a stale connection pointer from its previous owner. */
        int ready = s->wq_ready;
        struct waitq keep = s->wq;
        memset(s, 0, sizeof *s);
        s->wq = keep; s->wq_ready = ready;
        if (!s->wq_ready) { waitq_init(&s->wq); s->wq_ready = 1; }
        s->used = 1; s->type = type; s->pid = pid; s->state = U_OPEN;
        s->qmax = UNIX_BACKLOG;
        return s;
    }
    st_refused_full++;
    return NULL;
}

/* The one place a name is resolved to a socket. Linear over 48 slots and by
 * STRING, deliberately: a datagram send re-resolves its destination on every
 * call rather than caching the peer pointer, so a destination that closed and
 * whose slot was reused CANNOT be written into by a stale sender. The cache
 * would save a 48-entry compare on a path a syslog daemon walks a few times a
 * second; the dangling pointer it buys is the kind of bug that survives a
 * whole test suite. */
static struct usock *by_name(const char *canon)
{
    for (int i = 0; i < NUSOCK; i++)
        if (socks[i].used && socks[i].name[0] && pstreq(socks[i].name, canon))
            return &socks[i];
    return NULL;
}

/* Write permission on a bound name, POSIX's own three-way test. A NULL
 * credential is the KERNEL asking on its own behalf (the host gate, and any
 * in-kernel caller), not "unknown, so allow": every user path supplies one. */
static int may_connect(const struct vcred *cr, const struct usock *t)
{
    if (!cr) return 1;
    if (cr->uid == 0) return 1;
    if (cr->uid == t->uid) return (t->mode & VM_IWUSR) != 0;
    if (cr->gid == t->gid) return (t->mode & VM_IWGRP) != 0;
    return (t->mode & VM_IWOTH) != 0;
}

/* ------------------------------------------------------------- the channels */

static int chan_readable(const struct uchan *c) { return c->count > 0 || c->rcount > 0; }

/* Take at most one record (records) or as many bytes as fit (stream). A record
 * larger than the caller's buffer is TRUNCATED and the remainder DISCARDED,
 * which is what SOCK_DGRAM and SOCK_SEQPACKET both specify; returning the tail
 * on the next read would silently turn one datagram into two. */
static long chan_read(struct uchan *c, void *vbuf, long len, int records)
{
    unsigned char *out = (unsigned char *)vbuf;
    long n = 0;
    if (records) {
        if (c->rcount == 0) return 0;
        int rl = (int)c->rec[c->rtail];
        c->rtail = (c->rtail + 1) % UNIX_RECS;
        c->rcount--;
        for (int i = 0; i < rl; i++) {
            unsigned char b = c->buf[c->tail];
            c->tail = (c->tail + 1) % UNIX_BUF;
            c->count--;
            if (n < len) out[n++] = b;
        }
        return n;
    }
    while (n < len && c->count > 0) {
        out[n++] = c->buf[c->tail];
        c->tail = (c->tail + 1) % UNIX_BUF;
        c->count--;
    }
    return n;
}

/* Returns bytes taken; 0 means "no room right now", which the caller turns
 * into a park or an EAGAIN. A RECORD IS ALL OR NOTHING: half a datagram is not
 * a short write, it is a corrupt message, so the record path takes the whole
 * thing or nothing at all. A byte stream may legitimately take part. */
static long chan_write(struct uchan *c, const void *vbuf, long len, int records)
{
    const unsigned char *in = (const unsigned char *)vbuf;
    long n = 0;
    if (records) {
        if (c->rcount >= UNIX_RECS) return 0;
        if ((long)c->count + len > UNIX_BUF) return 0;
        c->rec[c->rhead] = (unsigned short)len;
        c->rhead = (c->rhead + 1) % UNIX_RECS;
        c->rcount++;
    }
    while (n < len && c->count < UNIX_BUF) {
        c->buf[c->head] = in[n++];
        c->head = (c->head + 1) % UNIX_BUF;
        c->count++;
    }
    return n;
}

static struct uconn *conn_new(int records)
{
    struct uconn *c = (struct uconn *)kmalloc(sizeof *c);
    if (!c) { st_refused_full++; return NULL; }
    memset(c, 0, sizeof *c);
    c->refs = 2;
#ifdef UNIX_NEGCTL_STREAMRECS
    /* NEGATIVE CONTROL 1: the shortcut logit_abi.h argues against, on a switch.
     * Every connection is a byte stream and SOCK_SEQPACKET is an alias for
     * SOCK_STREAM. Note what still works: every socket still connects, every
     * byte still arrives, in order, and the leak check is still clean. Only the
     * boundaries are gone -- which is exactly why "it works" is not evidence
     * here and why the `bounds:` checks are tagged separately.
     *
     * Measured: EXACTLY 8 checks redden and every one of them is tagged
     * `bounds:` -- the two dgram records, the four seqpacket ones (including
     * the truncation, which a stream cannot do at all) and the two read back
     * through a real accept()ed connection. 124 of 132 still pass. That the
     * failures are confined to the tag is the part worth checking: a control
     * that reddens the whole suite says only that the build changed. */
    (void)records;
    c->records = 0;
#else
    c->records = records;
#endif
    waitq_init(&c->wq);          /* fresh memory: the one place wait.h allows it */
    return c;
}

/* NEGATIVE CONTROL 2: the wake that makes a peer's departure VISIBLE, on a
 * switch. file.c's pipe says of the same line "Omitting this is a hang, not a
 * slowdown", and this is that sentence made checkable: with UNIX_NEGCTL_NOWAKE
 * the parked reader in t_block never re-tests successfully and the stub's
 * wait_event aborts by name instead of the whole gate hanging. It is the most
 * plausible omission in the file -- the state change is obviously right and the
 * announcement of it is easy to forget. */
#ifdef UNIX_NEGCTL_NOWAKE
#define unix_wake(q) ((void)(q))
#else
#define unix_wake(q) waitq_wake_all(q)
#endif

static void conn_put(struct uconn *c)
{
    if (!c) return;
    if (--c->refs > 0) { unix_wake(&c->wq); return; }
    kfree(c);
}

/* --------------------------------------------------------------- the calls */

struct usock *unix_create(int type, int pid, int *err)
{
    if (type != LOGIT_SOCK_STREAM && type != LOGIT_SOCK_DGRAM &&
        type != LOGIT_SOCK_SEQPACKET) { if (err) *err = LSK_E_ARG; return NULL; }
    struct usock *s = sk_alloc(type, pid);
    if (!s) { if (err) *err = LSK_E_FULL; return NULL; }
    if (err) *err = 0;
    return s;
}

int unix_bind(struct usock *s, const char *canon, const struct vcred *cr,
              unsigned umask)
{
    if (!s || !s->used || !canon) return LSK_E_ARG;
    /* An empty path is Linux's ABSTRACT NAMESPACE marker (a sun_path whose
     * first byte is NUL names a socket with no filesystem entry at all). It is
     * not implemented, and it is refused rather than accepted as "the empty
     * path": treating it as a name would let two unrelated programs that both
     * meant "abstract, private" collide on it, which is the opposite of what
     * either asked for. */
    if (!canon[0] || !pfits(canon)) return LSK_E_ARG;
    if (s->state != U_OPEN) return LSK_E_STATE;   /* already bound or connected */
    if (by_name(canon)) { st_refused_inuse++; return LSK_E_INUSE; }

    if (s->type == LOGIT_SOCK_DGRAM && !s->conn) {
        /* A bound datagram socket needs an inbox: unlike a stream, its senders
         * have no connection to put bytes in. Allocated here rather than at
         * create() so an unbound sender -- which is legal and common -- costs
         * nothing. */
        s->rx = (struct uchan *)kmalloc(sizeof *s->rx);
        if (!s->rx) { st_refused_full++; return LSK_E_FULL; }
        memset(s->rx, 0, sizeof *s->rx);
    }
    pstrcpy(s->name, canon, sizeof s->name);
    s->uid = cr ? cr->uid : 0;
    s->gid = cr ? cr->gid : 0;
    s->mode = 0777u & ~(umask & 0777u);
    s->state = U_BOUND;
    return 0;
}

int unix_listen(struct usock *s, int backlog)
{
    if (!s || !s->used) return LSK_E_ARG;
    if (s->type == LOGIT_SOCK_DGRAM) return LSK_E_STATE;   /* nothing to accept */
    if (s->state == U_LISTEN) return 0;                    /* idempotent, as POSIX */
    if (s->state != U_BOUND) return LSK_E_STATE;
    if (backlog < 1) backlog = 1;
    if (backlog > UNIX_BACKLOG) backlog = UNIX_BACKLOG;
    s->qmax = backlog;
    s->state = U_LISTEN;
    return 0;
}

struct usock *unix_accept(struct usock *s, int nonblock, int *err)
{
    if (!s || !s->used) { if (err) *err = LSK_E_ARG; return NULL; }
    if (s->state != U_LISTEN) { if (err) *err = LSK_E_STATE; return NULL; }

    while (s->qn == 0) {
        if (nonblock) { if (err) *err = LSK_E_AGAIN; return NULL; }
        wait_event(&s->wq, s->qn > 0 || s->state != U_LISTEN || sig_interrupted());
        if (s->state != U_LISTEN) { if (err) *err = LSK_E_STATE; return NULL; }
        if (s->qn == 0 && sig_interrupted()) { if (err) *err = LSK_E_AGAIN; return NULL; }
    }

    /* Allocate BEFORE popping. A failed allocation after the pop would drop a
     * connection the client has already been told is established, and the
     * client's next write would succeed into a buffer nobody will ever read. */
    struct usock *cs = sk_alloc(s->type, s->pid);
    if (!cs) { if (err) *err = LSK_E_FULL; return NULL; }

    struct uconn *c = s->q[0];
    for (int i = 1; i < s->qn; i++) s->q[i - 1] = s->q[i];
    s->qn--;

    cs->conn = c;
    cs->side = 1;
    cs->state = U_CONN;
    st_accepted++;
    /* Wake the connection's queue: a client parked in write() on a full buffer
     * has something to re-test now, because the accepted end is what will
     * drain it. */
    waitq_wake_all(&c->wq);
    if (err) *err = 0;
    return cs;
}

int unix_connect(struct usock *s, const char *canon, const struct vcred *cr)
{
    if (!s || !s->used || !canon || !canon[0] || !pfits(canon)) return LSK_E_ARG;
    if (s->state == U_CONN || s->state == U_LISTEN) return LSK_E_STATE;

    struct usock *t = by_name(canon);
    if (!t) { st_refused_name++; return LSK_E_CONNREFUSED; }
    if (t->type != s->type) return LSK_E_ARG;      /* EPROTOTYPE: a stream may
                                                    * not connect to a datagram */
    if (!may_connect(cr, t)) { st_refused_perm++; return LSK_E_PERM; }

    if (s->type == LOGIT_SOCK_DGRAM) {
        /* No handshake and no state on the far end: a connected datagram
         * socket is just one that remembers where to send. POSIX allows
         * re-connecting one, so this deliberately does not require U_OPEN. */
        pstrcpy(s->peer, canon, sizeof s->peer);
        s->has_peer = 1;
        return 0;
    }

    /* THE CHECK THIS WHOLE CALL EXISTS FOR. A path that is bound but not
     * listening is not a place to connect to, and it is a DIFFERENT failure
     * from a path nobody holds -- a client that cannot tell them apart cannot
     * decide between "start the daemon" and "wait for it to finish starting". */
    if (t->state != U_LISTEN) { st_refused_listen++; return LSK_E_CONNREFUSED; }
    if (t->qn >= t->qmax) return LSK_E_AGAIN;      /* the listener's backlog */

    struct uconn *c = conn_new(s->type == LOGIT_SOCK_SEQPACKET);
    if (!c) return LSK_E_FULL;
    s->conn = c;
    s->side = 0;
    s->state = U_CONN;
    t->q[t->qn++] = c;
    waitq_wake_all(&t->wq);
    return 0;
}

int unix_pair(int type, int pid, struct usock **pa, struct usock **pb, int *err)
{
    if (type != LOGIT_SOCK_STREAM && type != LOGIT_SOCK_DGRAM &&
        type != LOGIT_SOCK_SEQPACKET) { if (err) *err = LSK_E_ARG; return LSK_E_ARG; }
    struct usock *a = sk_alloc(type, pid);
    if (!a) { if (err) *err = LSK_E_FULL; return LSK_E_FULL; }
    struct usock *b = sk_alloc(type, pid);
    if (!b) { a->used = 0; if (err) *err = LSK_E_FULL; return LSK_E_FULL; }
    /* A datagram socketpair keeps message boundaries, so it takes the record
     * ring too -- which is why `records` is a property of the CONNECTION and
     * not of the accept path. */
    struct uconn *c = conn_new(type != LOGIT_SOCK_STREAM);
    if (!c) { a->used = 0; b->used = 0; if (err) *err = LSK_E_FULL; return LSK_E_FULL; }
    a->conn = c; a->side = 0; a->state = U_CONN;
    b->conn = c; b->side = 1; b->state = U_CONN;
    *pa = a; *pb = b;
    if (err) *err = 0;
    return 0;
}

/* ------------------------------------------------------------------- I/O */

long unix_read(struct usock *s, void *buf, long len, int nonblock)
{
    if (!s || !s->used || len < 0) return -1;
    if (len == 0) return 0;

    if (s->conn) {
        struct uconn *c = s->conn;
#ifdef UNIX_NEGCTL_ONEDIR
        /* NEGATIVE CONTROL 3: A SOCKET IS A PIPE. One buffer shared by both
         * ends instead of one per direction -- the mistake anybody reaching
         * for `struct pipe` as the substrate makes, and the reason unix.h
         * argues about the substrate at all. Each end reads its OWN bytes back
         * and the peer never sees them. Measured: 17 checks redden, headed by
         * the FIVE `twoway:` ones that NAME the property, and the run then
         * aborts inside `block: write wakes reader` -- a reader that is
         * watching the wrong channel is never satisfied by the peer's write,
         * which is a HANG on the real machine and is why the stub's
         * wait_event refuses to spin. So the log carries 18 `FAIL` lines: the
         * 17 checks plus the abort, which is LAST because the stub flushes
         * stdout before writing it. That flush is not cosmetic -- without it
         * this count alternated between 17 and 18 run to run, the abort having
         * been spliced into the middle of a buffered check line; see
         * tests/unit/unixstub/kernel/core/wait.h. Verified stable at 18 over 8
         * consecutive runs. */
        struct uchan *in = &c->ch[s->side];
#else
        struct uchan *in = &c->ch[1 - s->side];
#endif
        if (c->e[s->side].rd_shut) return 0;             /* our own SHUT_RD */
        for (;;) {
            if (chan_readable(in)) {
                long n = chan_read(in, buf, len, c->records);
                waitq_wake_all(&c->wq);                  /* there is room now */
                return n;
            }
            /* EOF is tested AFTER the data, so bytes a peer wrote before it
             * closed are still delivered -- the property a pipe has and the one
             * every request/response protocol depends on. */
            if (c->e[1 - s->side].gone || c->e[1 - s->side].wr_shut) return 0;
            if (nonblock) return EAGAIN_RC;
            wait_event(&c->wq, chan_readable(in) || c->e[1 - s->side].gone ||
                               c->e[1 - s->side].wr_shut || sig_interrupted());
            if (!chan_readable(in) && !c->e[1 - s->side].gone &&
                !c->e[1 - s->side].wr_shut && sig_interrupted())
                return SIG_E_INTR;
        }
    }

    if (s->rx) {                                          /* bound datagram */
        for (;;) {
            if (chan_readable(s->rx)) {
                long n = chan_read(s->rx, buf, len, 1);
                waitq_wake_all(&s->wq);                   /* there is room now */
                return n;
            }
            if (nonblock) return EAGAIN_RC;
            /* NO EOF EXISTS HERE, and that is not an omission. A bound
             * datagram socket has no peer whose departure could end the
             * stream: senders come and go and the inbox stays open, exactly as
             * on Linux. A daemon reading this loop blocks until a message
             * arrives or it is signalled. */
            wait_event(&s->wq, chan_readable(s->rx) || sig_interrupted());
            if (!chan_readable(s->rx) && sig_interrupted()) return SIG_E_INTR;
        }
    }
    return LSK_E_STATE;    /* unconnected, unbound: there is nowhere to read from */
}

long unix_write(struct usock *s, const void *buf, long len, int nonblock)
{
    if (!s || !s->used || len < 0) return -1;
    /* A zero-length write sends NOTHING and reports 0, on a record socket as
     * well as a stream. That is deliberate and it is the one place this layer
     * is narrower than Linux, which delivers an empty datagram: read() returns
     * 0 for "end of stream", so an empty record arriving through the read()
     * face is indistinguishable from the peer having closed. Refusing to
     * create that ambiguity costs a caller nothing real -- the length is
     * already short-circuited by file.c before it reaches here. */
    if (len == 0) return 0;

    if (s->conn) {
        struct uconn *c = s->conn;
        struct uchan *out = &c->ch[s->side];
        long sent = 0;
        /* A record longer than the ring can NEVER fit, so waiting for room
         * would be an unbounded park. Refused by name instead (EMSGSIZE's
         * shape) rather than truncated -- a truncated datagram is a corrupt
         * message the receiver has no way to detect. */
        if (c->records && len > UNIX_BUF) return LSK_E_ARG;
        for (;;) {
            if (c->e[s->side].wr_shut || c->e[1 - s->side].gone ||
                c->e[1 - s->side].rd_shut) {
                /* Same rule and the same reason as the pipe's in file.c: the
                 * default action of SIGPIPE is terminate, which is what makes
                 * a writer stop; a server that has ignored it still gets the
                 * -1 here. */
                if (ksig_post_current) ksig_post_current(LOGIT_SIGPIPE);
                return sent > 0 ? sent : -1;
            }
            long n = chan_write(out, (const char *)buf + sent, len - sent, c->records);
            if (n > 0) {
                sent += n;
                if (c->records) st_dgrams++;
                waitq_wake_all(&c->wq);
                if (c->records || sent >= len) return sent;
                continue;
            }
            if (nonblock) return sent > 0 ? sent : EAGAIN_RC;
            wait_event(&c->wq, c->ch[s->side].count < UNIX_BUF ||
                               c->e[1 - s->side].gone || c->e[1 - s->side].rd_shut ||
                               sig_interrupted());
            if (sig_interrupted() && c->ch[s->side].count >= UNIX_BUF)
                return sent > 0 ? sent : SIG_E_INTR;
        }
    }

    if (s->type == LOGIT_SOCK_DGRAM) {
        if (!s->has_peer) return LSK_E_STATE;      /* send() with no destination */
        if (len > UNIX_BUF) return LSK_E_ARG;
        for (;;) {
            /* RE-RESOLVED EVERY TIME: see by_name(). If the daemon died, this
             * is where the sender finds out, and it finds out as
             * ECONNREFUSED rather than by writing into a recycled slot. */
            struct usock *t = by_name(s->peer);
            if (!t || !t->rx) { st_refused_name++; return LSK_E_CONNREFUSED; }
            long n = chan_write(t->rx, buf, len, 1);
            if (n > 0) { st_dgrams++; waitq_wake_all(&t->wq); return n; }
            if (nonblock) return EAGAIN_RC;
            wait_event(&t->wq, t->rx->rcount < UNIX_RECS || sig_interrupted());
            if (sig_interrupted() && t->rx->rcount >= UNIX_RECS) return SIG_E_INTR;
        }
    }
    return LSK_E_STATE;
}

int unix_shutdown(struct usock *s, int how)
{
    if (!s || !s->used) return LSK_E_ARG;
    if (how != LOGIT_SHUT_RD && how != LOGIT_SHUT_WR && how != LOGIT_SHUT_RDWR)
        return LSK_E_ARG;
    if (!s->conn) return LSK_E_STATE;
    if (how == LOGIT_SHUT_RD || how == LOGIT_SHUT_RDWR)
        s->conn->e[s->side].rd_shut = 1;
    if (how == LOGIT_SHUT_WR || how == LOGIT_SHUT_RDWR)
        s->conn->e[s->side].wr_shut = 1;
    /* The wake is the whole point: a peer parked in read() is waiting for
     * exactly this, and without it shutdown(SHUT_WR) is a hang rather than an
     * end-of-stream. Same sentence file.c writes over the pipe's close. */
    unix_wake(&s->conn->wq);
    return 0;
}

int unix_getsockname(struct usock *s, char *out, int max)
{
    if (!s || !s->used || !out || max <= 0) return LSK_E_ARG;
    pstrcpy(out, s->name, max);
    return 0;
}

void unix_release(struct usock *s)
{
    if (!s || !s->used) return;

    if (s->conn) {
        s->conn->e[s->side].gone = 1;
        s->conn->e[s->side].wr_shut = 1;
        conn_put(s->conn);              /* wakes the peer, or frees the pair */
        s->conn = NULL;
    }
    /* A listener that goes away takes its unaccepted connections with it. Each
     * one already has a live client on the other side, so the queue's
     * reference is dropped AND the server direction is marked gone -- which is
     * what turns that client's next read into EOF instead of a permanent park.
     * Silently freeing the uconn would leave the client reading memory that had
     * been handed back to the heap. */
    for (int i = 0; i < s->qn; i++) {
        s->q[i]->e[1].gone = 1;
        s->q[i]->e[1].wr_shut = 1;
        conn_put(s->q[i]);
    }
    s->qn = 0;
    if (s->rx) { kfree(s->rx); s->rx = NULL; }

    s->name[0] = 0;
    s->has_peer = 0;
    s->state = U_FREE;
    s->used = 0;
    /* Wake anything parked on THIS socket's queue (an acceptor, or a sender
     * waiting for room in this inbox) before the slot can be reused. The queue
     * itself is never re-initialised -- see wq_ready. */
    unix_wake(&s->wq);
}

long unix_stat(int what)
{
    int nsock = 0, nname = 0, nconn = 0;
    for (int i = 0; i < NUSOCK; i++) {
        if (!socks[i].used) continue;
        nsock++;
        if (socks[i].name[0]) nname++;
        if (socks[i].conn && socks[i].side == 0) nconn++;
    }
    switch (what) {
    case UNIXSTAT_SOCKS:           return nsock;
    case UNIXSTAT_NAMES:           return nname;
    case UNIXSTAT_CONNS:           return nconn;
    case UNIXSTAT_REFUSED_NAME:    return st_refused_name;
    case UNIXSTAT_REFUSED_LISTEN:  return st_refused_listen;
    case UNIXSTAT_REFUSED_PERM:    return st_refused_perm;
    case UNIXSTAT_REFUSED_INUSE:   return st_refused_inuse;
    case UNIXSTAT_REFUSED_FULL:    return st_refused_full;
    case UNIXSTAT_ACCEPTED:        return st_accepted;
    case UNIXSTAT_DGRAMS:          return st_dgrams;
    default:                       return -1;
    }
}
