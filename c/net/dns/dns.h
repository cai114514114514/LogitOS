#ifndef LOGIT_DNS_H
#define LOGIT_DNS_H

#include <stdint.h>

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
 * kept in a small fixed-TTL name cache, so a page's many sub-resource hosts cost
 * one lookup each rather than one per connection. */
int      dns_query_start(const char *name);
uint32_t dns_query_result(int id);
void     dns_query_free(int id);
void     dns_poll(void);

#endif /* LOGIT_DNS_H */
