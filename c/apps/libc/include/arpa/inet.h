#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <netinet/in.h>
#include <stddef.h>

/* Text <-> binary IPv4/IPv6 address conversion. Pure computation, real and
 * complete, byte-for-byte checked against glibc (tests/unit/libc_inet_test.c
 * / test-libc-diff-style host diff). */

#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46

in_addr_t     inet_addr(const char *cp);
int           inet_aton(const char *cp, struct in_addr *addr);
char         *inet_ntoa(struct in_addr in);
const char   *inet_ntop(int af, const void *src, char *dst, size_t size);
int           inet_pton(int af, const char *src, void *dst);

#endif /* _ARPA_INET_H */
