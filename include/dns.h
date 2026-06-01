#ifndef AQUA_DNS_H
#define AQUA_DNS_H

#include <stdint.h>

/* Resolve `name` to an IPv4 address (host order) via the SLIRP DNS server
 * (10.0.2.3) over UDP. Blocking-ish: pumps net_poll() with a timeout. Returns
 * the address, or 0 on failure/timeout. Must run with interrupts enabled. */
uint32_t dns_resolve(const char *name);

#endif /* AQUA_DNS_H */
