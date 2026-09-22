/* Adapt the existing TLS record engine to ordinary nonblocking ring-3 fds.
 * These tcp IDs are process descriptors, never kernel connection-table IDs. */
#include "clib.h"
#include "crypto.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
int tcp_send_nb(int fd, const void *b, int n)
{ int r=sys_write(fd,b,n);return r==LSK_E_AGAIN||r==SIG_E_INTR?0:r; }
int tcp_recv(int fd, void *b, int n)
{ int r=sys_read(fd,b,n);return r==LSK_E_AGAIN||r==SIG_E_INTR?0:r==0?-1:r; }
uint64_t timer_ticks(void) { return monotonic_ns()/10000000ull; }
int rng_strong(void) { return getrandom_strong(); }
void kernel_random_bytes(void *b, int n)
{ if(getrandom_bytes(b,n)<0) { errs("httpsd: entropy unavailable\n"); app_exit(1); } }
/* No kernel logger in a userspace daemon. Preserve messages without exposing
 * varargs to a mismatched libc formatter; rich diagnostics stay in tlss APIs. */
void kprintf(const char *fmt, ...) { (void)fmt; }
struct aes_backend;
const struct aes_backend *aes_backend_ni(void) { return 0; }
