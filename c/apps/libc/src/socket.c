/* <sys/socket.h> and <sys/un.h>.
 *
 * WHAT CHANGED, AND WHAT DID NOT. This file used to be twelve one-line
 * refusals, and the header above it explained at length that a BSD sockets API
 * was impossible here because the kernel's network surface was "DELIBERATELY
 * not a BSD sockets API" and because "there is no bind()/listen()/accept() on
 * ANY path -- the kernel is a client only". Both halves of that are now false:
 * the fd layer grew F_SOCK (c/kernel/exec/file.c), the server-socket family
 * landed (SYS_SOCKET..SYS_SOCKSTAT), and AF_UNIX landed on top of it
 * (c/net/core/unix.c). So the AF_UNIX half of this header is REAL.
 *
 * AF_INET IS STILL REFUSED HERE, and that is a decision rather than an
 * oversight. Two things stand in the way and neither is a line of glue:
 *
 *   - `struct sockaddr_in` is NETWORK byte order by definition, and this
 *     kernel's ABI is HOST order everywhere (see the note on struct
 *     logit_sockaddr in include/abi/logit_abi.h, which calls the break with
 *     BSD deliberate). Converting is easy; getting it wrong is a byte-swap bug
 *     that shows up as a connection to a random address, and there is no
 *     caller in this tree to check it against -- /bin/httpd uses the raw
 *     syscalls, not this file.
 *   - connect() has no AF_INET implementation to call. SYS_CONNECT refuses
 *     that family in the kernel (see its comment in logit_abi.h).
 *
 * So socket(AF_INET, ...) returns EAFNOSUPPORT exactly as it did, rather than
 * succeeding and then failing somewhere less obvious. A caller that gets a
 * descriptor believes it has one.
 *
 * ERRNO MAPPING. The kernel returns distinct LSK_E_* codes precisely so that
 * this layer does not have to collapse them (logit_abi.h argues that at the
 * definition). Each maps to the errno a C program tests for by name, which is
 * why "no daemon at that path" comes out as ECONNREFUSED and not as EINVAL --
 * a client retry loop is written around exactly that distinction. */
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include "logit_abi.h"

static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

/* One mapping, used by every call below, so the same kernel code cannot become
 * two different errnos depending on which wrapper the program happened to
 * call. Returns the errno to set.
 *
 * THE LSK_E_* AND SIG_E_* SPACES OVERLAP NUMERICALLY, and this function decodes
 * exactly one of them. `SIG_E_INTR` is -4 and so is `LSK_E_FULL`; the compiler
 * found it as a duplicate case label, which is the only reason it was noticed
 * at all. They do not collide in practice because they never travel on the same
 * syscall -- an interrupted wait comes back through SYS_READ/SYS_WRITE, where
 * read() and write() in io.c already map it to EINTR, and none of the calls in
 * this file can return it. Adding it "for completeness" would have turned every
 * genuine LSK_E_FULL into a spurious EINTR, and a retry loop written around
 * EINTR would then spin forever on a full descriptor table.
 *
 * The one overlap that IS deliberate is LSK_E_AGAIN == EAGAIN_RC (-2), which
 * include/abi/logit_abi.h states outright at the definition: "Same value
 * SYS_READ uses on a non-blocking pipe." */
static int sock_errno(long rc)
{
    switch (rc) {
    case LSK_E_ARG:         return EINVAL;
    case LSK_E_AGAIN:       return EAGAIN;
    case LSK_E_INUSE:       return EADDRINUSE;
    case LSK_E_FULL:        return EMFILE;
    case LSK_E_STATE:       return ENOTCONN;
    case LSK_E_NET:         return ENETDOWN;
    case LSK_E_PERM:        return EACCES;
    case LSK_E_CONNREFUSED: return ECONNREFUSED;
    default:                return EIO;
    }
}

static long sfail(long rc)
{
    if (rc < 0) { errno = sock_errno(rc); return -1; }
    return rc;
}

/* AF_UNIX / AF_LOCAL only -- see the header comment for why AF_INET is not
 * quietly accepted here. */
static int is_unix(int domain) { return domain == AF_UNIX || domain == AF_LOCAL; }

/* `struct sockaddr_un` and `struct logit_sockaddr_un` are laid out identically
 * (unsigned short, then 108 chars) on purpose, so this is a copy with a
 * terminator forced on -- a caller is allowed to hand over a sun_path that
 * fills all 108 bytes with no NUL, and everything below the syscall treats it
 * as a C string. */
static int to_kaddr(const struct sockaddr *sa, socklen_t len,
                    struct logit_sockaddr_un *out)
{
    if (!sa || len < (socklen_t)sizeof(sa_family_t)) { errno = EINVAL; return -1; }
    const struct sockaddr_un *un = (const struct sockaddr_un *)sa;
    if (!is_unix(un->sun_family)) { errno = EAFNOSUPPORT; return -1; }
    out->family = LOGIT_AF_UNIX;
    size_t max = sizeof out->path - 1;
    size_t room = (size_t)len > sizeof(sa_family_t) ? (size_t)len - sizeof(sa_family_t) : 0;
    if (room > sizeof un->sun_path) room = sizeof un->sun_path;
    if (room > max) room = max;
    memcpy(out->path, un->sun_path, room);
    out->path[room] = 0;
    /* The abstract namespace is not implemented. Refused HERE as well as in the
     * kernel, so the errno is the POSIX one for "this address form is not
     * supported" rather than whatever the kernel's generic bad-argument code
     * maps to. */
    if (!out->path[0]) { errno = EAFNOSUPPORT; return -1; }
    return 0;
}

int socket(int domain, int type, int protocol)
{
    if (!is_unix(domain)) { errno = EAFNOSUPPORT; return -1; }
    if (protocol != 0) { errno = EPROTONOSUPPORT; return -1; }
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET)
        { errno = EPROTOTYPE; return -1; }
    return (int)sfail(sys(SYS_SOCKET, LOGIT_AF_UNIX, type, 0));
}

int socketpair(int domain, int type, int protocol, int sv[2])
{
    if (!is_unix(domain)) { errno = EAFNOSUPPORT; return -1; }
    if (protocol != 0) { errno = EPROTONOSUPPORT; return -1; }
    if (!sv) { errno = EINVAL; return -1; }
    return (int)sfail(sys(SYS_SOCKETPAIR, LOGIT_AF_UNIX, type, (long)sv));
}

int bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    struct logit_sockaddr_un ka;
    if (to_kaddr(addr, len, &ka) < 0) return -1;
    return (int)sfail(sys(SYS_BIND, fd, (long)&ka, sizeof ka));
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    struct logit_sockaddr_un ka;
    if (to_kaddr(addr, len, &ka) < 0) return -1;
    return (int)sfail(sys(SYS_CONNECT, fd, (long)&ka, sizeof ka));
}

int listen(int fd, int backlog)
{
    return (int)sfail(sys(SYS_LISTEN, fd, backlog, 0));
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    /* `timeout_ms` 0 = wait indefinitely, which is what accept(2) means.
     *
     * An AF_UNIX client is almost never bound, so it HAS no address to report.
     * POSIX allows *len = 0 for exactly that case, and that is what happens
     * here: the kernel leaves the caller's buffer untouched and this sets the
     * length to zero rather than fabricating a name. A caller that prints the
     * peer gets an empty string, not a wrong one. */
    long rc = sys(SYS_ACCEPT, fd, 0, 0);
    if (rc < 0) { errno = sock_errno(rc); return -1; }
    if (addr && len) {
        if (*len >= (socklen_t)sizeof(sa_family_t))
            ((struct sockaddr_un *)addr)->sun_family = AF_UNIX;
        *len = 0;
    }
    return (int)rc;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    if (!addr || !len || *len < (socklen_t)sizeof(struct sockaddr_un))
        { errno = EINVAL; return -1; }
    struct logit_sockaddr_un ka;
    long rc = sys(SYS_GETSOCKNAME, fd, (long)&ka, sizeof ka);
    if (rc < 0) { errno = sock_errno(rc); return -1; }
    struct sockaddr_un *un = (struct sockaddr_un *)addr;
    un->sun_family = AF_UNIX;
    memcpy(un->sun_path, ka.path, sizeof un->sun_path);
    un->sun_path[sizeof un->sun_path - 1] = 0;
    *len = (socklen_t)sizeof *un;
    return 0;
}

/* getpeername has no kernel call for AF_UNIX and is NOT faked. An unnamed
 * client -- which is nearly every client -- genuinely has no address, so the
 * only honest answers are "no address" or an error, and ENOTCONN is the one
 * POSIX already defines for a socket with no peer name to report. Returning a
 * zeroed sockaddr_un with a 0 length would be indistinguishable from a real
 * answer about a real unnamed peer, which is worse. */
int getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{ (void)fd; (void)addr; (void)len; errno = ENOTCONN; return -1; }

int shutdown(int fd, int how)
{
    return (int)sfail(sys(SYS_SHUTDOWN, fd, how, 0));
}

/* --- send/recv ------------------------------------------------------------
 *
 * With flags == 0 these ARE read() and write() on the descriptor, which is what
 * they are on any Unix. The flags are where the honesty is:
 *
 *   MSG_NOSIGNAL is HONOURED, by ignoring SIGPIPE across the write and putting
 *   the previous disposition back. Two extra syscalls per send, and it is not
 *   safe against another thread changing the same disposition concurrently --
 *   both stated rather than hidden. The alternative was to refuse the flag,
 *   which turns "this program does not want to die on a broken pipe" into
 *   "this program cannot send at all", and the alternative to THAT was to
 *   ignore the flag, which kills the process the flag exists to keep alive.
 *
 *   MSG_DONTWAIT is REFUSED. Honouring it means toggling O_NONBLOCK around the
 *   call, and there is no syscall here that reads the flag back -- so
 *   restoring it afterwards would silently CLEAR a flag the caller had set
 *   itself. Set O_NONBLOCK on the descriptor instead; that path works.
 *
 *   MSG_PEEK, MSG_OOB and MSG_WAITALL are REFUSED. Each needs support inside
 *   c/net/core/unix.c that is not there: a peek must not consume, out-of-band
 *   data has no channel, and waitall must loop across record boundaries. A
 *   silently-ignored MSG_PEEK consumes the data it promised to leave. */
#define MSG_KNOWN (MSG_NOSIGNAL)

ssize_t send(int fd, const void *buf, size_t n, int flags)
{
    if (flags & ~MSG_KNOWN) { errno = ENOTSUP; return -1; }
    if (!(flags & MSG_NOSIGNAL)) return write(fd, buf, n);
    __sighandler_t prev = signal(SIGPIPE, SIG_IGN);
    ssize_t r = write(fd, buf, n);
    int saved = errno;
    if (prev != SIG_ERR) signal(SIGPIPE, prev);
    errno = saved;
    return r;
}

ssize_t recv(int fd, void *buf, size_t n, int flags)
{
    if (flags & ~MSG_KNOWN) { errno = ENOTSUP; return -1; }
    return read(fd, buf, n);
}

/* sendto/recvfrom WITH an address are not available on AF_UNIX here: the
 * kernel's datagram address struct carries family/port/addr and has nowhere to
 * put a 108-byte path (see lsock_recvfrom in c/net/core/lsock.c). With a NULL
 * address they are exactly send/recv, so those forms work and the addressed
 * forms say EOPNOTSUPP instead of dropping the address on the floor. A
 * datagram client connect()s and then send()s, which is what glibc's syslog()
 * does and what this tree's own /bin/syslogd expects. */
ssize_t sendto(int fd, const void *buf, size_t n, int flags,
               const struct sockaddr *addr, socklen_t len)
{
    if (addr) { (void)len; errno = EOPNOTSUPP; return -1; }
    return send(fd, buf, n, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t n, int flags,
                 struct sockaddr *addr, socklen_t *len)
{
    if (addr) {
        /* The sender of an AF_UNIX datagram is not reported. Say so with a
         * zero length -- POSIX's "the address is unavailable" -- rather than
         * failing the whole receive, because the DATA is available and a
         * caller that ignores the address (every one in practice) should get
         * it. The family is stamped so a caller that inspects it does not read
         * uninitialised memory. */
        if (*len >= (socklen_t)sizeof(sa_family_t))
            ((struct sockaddr_un *)addr)->sun_family = AF_UNIX;
        *len = 0;
    }
    return recv(fd, buf, n, flags);
}

/* getsockopt/setsockopt: the kernel accepts no option on an AF_UNIX socket
 * (lsock_setsockopt refuses the family by name -- every option it has is an
 * AF_INET one, and LOGIT_TCP_NODELAY on one of these would index a connection
 * table with a handle that is not a connection id). Refused rather than
 * accepted-and-ignored: SO_RCVTIMEO accepted and ignored is a program that
 * believes it has a deadline and blocks forever. */
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{ (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; errno = ENOPROTOOPT; return -1; }
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{ (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; errno = ENOPROTOOPT; return -1; }
