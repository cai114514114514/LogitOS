#ifndef LOGIT_H_HOSTSTUB
#define LOGIT_H_HOSTSTUB

/* A host stand-in for c/apps/logit.h, so browser_rt.c's bxfer layer can be
 * compiled and driven on the host.
 *
 * WHY A STUB DIRECTORY RATHER THAN A FAKE bxfer.  The thing under test IS
 * browser_rt.c: the protocol choice, the re-encoding of a serialized HTTP/1.1
 * request as HPACK, the refcounted connection that many streams share, and the
 * materialisation of an HTTP/2 stream into a `struct h1_response`. A test that
 * reimplemented any of that would be asserting against itself. So the real
 * file is compiled, and the only things replaced are the six socket syscalls
 * underneath it -- which is the same trick tests/unit/tcpstub and fsstub use,
 * and the same reason: the boundary worth faking is the one the kernel owns.
 *
 * The stub forwards to hstub_* which the test file defines; the test is where
 * the peer lives (an in-memory HTTP/2 server and an HTTP/1.1 one), so a single
 * run can assert what a live server cannot be made to do on demand -- an
 * origin that answers `h2` for one test and `http/1.1` for the next. */

#include <stddef.h>
#include "logit_abi.h"          /* the real SOCK_F_* / SOCK_P_* */

int  hstub_open(const char *host, int port, int flags);
int  hstub_poll(int fd);
int  hstub_send(int fd, const void *buf, int len);
int  hstub_recv(int fd, void *buf, int max);
int  hstub_close(int fd);
int  hstub_alpn(int fd, char *buf, int max);
unsigned long long hstub_now(void);

static inline unsigned long long monotonic_ms(void) { return hstub_now(); }
static inline void sys_yield(void) { }

static inline int sock_open(const char *host, int port, int flags)
{ return hstub_open(host, port, flags); }
static inline int sock_poll(int fd) { return hstub_poll(fd); }
static inline int sock_send(int fd, const void *buf, int len) { return hstub_send(fd, buf, len); }
static inline int sock_recv(int fd, void *buf, int max) { return hstub_recv(fd, buf, max); }
static inline int sock_alpn(int fd, char *buf, int max) { return hstub_alpn(fd, buf, max); }
static inline int sock_close(int fd) { return hstub_close(fd); }

/* Layout's font metric. bxfer never calls it; browser_rt.c defines a wrapper
 * over it, so it only has to exist. */
static inline int text_measure_px(const char *s, int len, int px, int mono)
{ (void)s; (void)mono; return (len < 0 ? 0 : len) * (px ? px : 16) / 2; }

#endif /* LOGIT_H_HOSTSTUB */
