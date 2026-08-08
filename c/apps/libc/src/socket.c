/* <sys/socket.h>. See the header: none of this works. Every call sets errno
 * and fails, honestly, and precisely -- see the header for why a real
 * implementation needs kernel/fd-layer work this file does not own. */
#include <sys/socket.h>
#include <errno.h>

int socket(int domain, int type, int protocol)
{ (void)domain; (void)type; (void)protocol; errno = EAFNOSUPPORT; return -1; }
int bind(int fd, const struct sockaddr *addr, socklen_t len)
{ (void)fd; (void)addr; (void)len; errno = ENOSYS; return -1; }
int listen(int fd, int backlog)
{ (void)fd; (void)backlog; errno = ENOSYS; return -1; }
int accept(int fd, struct sockaddr *addr, socklen_t *len)
{ (void)fd; (void)addr; (void)len; errno = ENOSYS; return -1; }
int connect(int fd, const struct sockaddr *addr, socklen_t len)
{ (void)fd; (void)addr; (void)len; errno = ENOSYS; return -1; }
ssize_t send(int fd, const void *buf, size_t n, int flags)
{ (void)fd; (void)buf; (void)n; (void)flags; errno = ENOTSOCK; return -1; }
ssize_t recv(int fd, void *buf, size_t n, int flags)
{ (void)fd; (void)buf; (void)n; (void)flags; errno = ENOTSOCK; return -1; }
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *addr, socklen_t len)
{ (void)fd; (void)buf; (void)n; (void)flags; (void)addr; (void)len; errno = ENOTSOCK; return -1; }
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *addr, socklen_t *len)
{ (void)fd; (void)buf; (void)n; (void)flags; (void)addr; (void)len; errno = ENOTSOCK; return -1; }
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen)
{ (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; errno = ENOTSOCK; return -1; }
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{ (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; errno = ENOTSOCK; return -1; }
int getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{ (void)fd; (void)addr; (void)len; errno = ENOTSOCK; return -1; }
int getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{ (void)fd; (void)addr; (void)len; errno = ENOTSOCK; return -1; }
int shutdown(int fd, int how)
{ (void)fd; (void)how; errno = ENOTSOCK; return -1; }
int socketpair(int domain, int type, int protocol, int sv[2])
{ (void)domain; (void)type; (void)protocol; (void)sv; errno = EAFNOSUPPORT; return -1; }
