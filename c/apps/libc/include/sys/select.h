#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#include <sys/time.h>

/* select(), implemented over the same primitives and the same honest limits
 * as <poll.h> -- read that header's comment first. */

#define FD_SETSIZE 256

typedef struct { unsigned long bits[FD_SETSIZE / (8 * sizeof(unsigned long))]; } fd_set;

static inline void FD_ZERO(fd_set *s) { for (unsigned i = 0; i < FD_SETSIZE / (8 * sizeof(unsigned long)); i++) s->bits[i] = 0; }
static inline void FD_SET(int fd, fd_set *s) { if (fd >= 0 && fd < FD_SETSIZE) s->bits[fd / (8 * sizeof(unsigned long))] |= 1UL << (fd % (8 * sizeof(unsigned long))); }
static inline void FD_CLR(int fd, fd_set *s) { if (fd >= 0 && fd < FD_SETSIZE) s->bits[fd / (8 * sizeof(unsigned long))] &= ~(1UL << (fd % (8 * sizeof(unsigned long)))); }
static inline int  FD_ISSET(int fd, const fd_set *s) { return (fd >= 0 && fd < FD_SETSIZE) && (s->bits[fd / (8 * sizeof(unsigned long))] >> (fd % (8 * sizeof(unsigned long)))) & 1UL; }

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);

#endif /* _SYS_SELECT_H */
