#ifndef LOGIT_DNS_H
#define LOGIT_DNS_H

#include <stdint.h>
#include "ip6.h"        /* ip6_addr: a lookup's answer set is mixed-family */

/* A stub resolver: one upstream server (net_cfg.dns), trusted, no local
 * recursion or DNSSEC. See the header comment in dns.c for exactly what that
 * means and what this file does beyond the stub minimum (owner-name
 * matching + CNAME chasing, EDNS0 + a TCP fallback on TC, real per-record
 * TTLs). */

/* Resolve `name` to an IPv4 address (host order) via the SLIRP DNS server
 * (10.0.2.3) over UDP. Blocking-ish: pumps net_poll() with a timeout. Returns
 * the address, or 0 on failure/timeout. Must run with interrupts enabled. */
uint32_t dns_resolve(const char *name);

/* Non-blocking variant for apps (the WM loop pumps net_poll). dns_start() sends
 * the query; dns_result() returns the resolved IP, 0 while pending, or
 * 0xFFFFFFFF on timeout. */
void     dns_start(const char *name);
uint32_t dns_result(void);

/* Longest name the resolver stores (DNS caps a presentation name at 253). */
#define DNS_NAME_MAX 256

/* Async pool: several lookups outstanding at once, none of them waited on.
 * dns_query_start() returns a query id, dns_query_result() reports 0 pending /
 * 0xFFFFFFFF failed / the address, dns_query_free() releases the slot, and
 * dns_poll() (pumped from net_poll) is what advances all of them. Answers are
 * kept in a small name cache, expiring per the record's own TTL (clamped --
 * see DNS_TTL_FLOOR/DNS_TTL_CEIL in dns.c), so a page's many sub-resource
 * hosts cost one lookup each rather than one per connection. */
int      dns_query_start(const char *name);
uint32_t dns_query_result(int id);
void     dns_query_free(int id);
void     dns_poll(void);

/* The dual-stack view of the same lookup. A name has an A set and a AAAA set;
 * this returns them MERGED and ORDERED by RFC 6724 destination selection, with
 * IPv4 addresses carried as ::ffff:a.b.c.d so the two families are comparable.
 * 0 = still pending, < 0 = the name did not resolve, else the count written.
 *
 * The caller is expected to try out[0], and fall back to out[1..] -- which is
 * how a host whose IPv6 path is broken still reaches a dual-stack server.
 * AAAA is only queried when ip6_up() says IPv6 is actually routable, so on a
 * v4-only network this returns exactly the A records, in resolver order. */
int      dns_query_addrs(int id, ip6_addr *out, int max);

#endif /* LOGIT_DNS_H */
