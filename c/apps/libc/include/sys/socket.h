#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#include <sys/types.h>
#include <stdint.h>

/* NOT A WORKING SOCKET LAYER -- READ THIS BEFORE RELYING ON ANYTHING HERE.
 *
 * This header exists so a program that unconditionally #includes
 * <sys/socket.h> at the top of a file (nearly everything that ever touches
 * the network does) COMPILES, the same way <signal.h> lets a program that
 * mentions SIGINT link. Every function below FAILS at runtime (ENOSYS or
 * EAFNOSUPPORT), honestly, rather than pretending.
 *
 * WHY IT CANNOT BE MORE THAN THAT TODAY. The kernel's real network surface
 * (SYS_SOCK_OPEN/POLL/SEND/RECV/CLOSE, include/abi/logit_abi.h, "M27") is
 * DELIBERATELY not a BSD sockets API: a socket "handle" is opened with a
 * HOSTNAME AND PORT IN ONE CALL (it does DNS internally) and lives in its
 * own per-process table -- it is NOT a file descriptor, and it cannot be one
 * without either (a) teaching the fd layer (c/kernel/exec/file.c) a new file
 * kind, or (b) building a ring-3 shim that intercepts read()/write()/close()
 * ahead of every real syscall to multiplex a second, disjoint number space
 * over the same int -- which means editing io.c's read/write/close, the
 * single most depended-on function in this tree (every program, including
 * the ones this change must not break, calls through it). That is real
 * engineering, not a header, and it is exactly the gap the libc inventory
 * report calls out as unimplemented rather than attempted half-safely.
 * There is no bind()/listen()/accept() on ANY path -- the kernel is a
 * client only -- so even a completed shim would still refuse server sockets.
 *
 * A ported program that wants to actually talk to the network on LogitOS
 * today uses the native ABI directly (see c/apps/net's ring-3 HTTP client
 * for the pattern), not this header. */

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_storage { sa_family_t ss_family; char __pad[128 - sizeof(sa_family_t)]; };

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

#define SOL_SOCKET  1
#define SO_REUSEADDR 2
#define SO_KEEPALIVE 9
#define SO_ERROR     4
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define MSG_PEEK     0x01
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t recv(int fd, void *buf, size_t n, int flags);
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *addr, socklen_t len);
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *addr, socklen_t *len);
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int shutdown(int fd, int how);
int socketpair(int domain, int type, int protocol, int sv[2]);

#endif /* _SYS_SOCKET_H */
