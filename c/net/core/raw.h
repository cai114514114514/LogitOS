#ifndef LOGIT_RAW_H
#define LOGIT_RAW_H

#include <stdint.h>

/* RAW ICMP SOCKETS -- the fd-shaped face is c/net/core/lsock.c
 * (LOGIT_SOCK_RAW); this file is the queue underneath it, in the same shape
 * as c/net/transport/udp.c's bound ports. Kept separate from lsock.c for the
 * reason lsock.h gives for splitting from udp.c: lsock.c owns the fd/state
 * machine, this file owns the wire-protocol table.
 *
 * WHAT A RAW ICMP SOCKET IS, AND WHAT IT DELIBERATELY IS NOT.
 *
 * A real BSD raw ICMP socket receives a copy of EVERY inbound ICMP message
 * (not scoped by port -- ICMP has none) and can send a caller-built ICMP
 * message to any destination. Both of those are here. Two things a real one
 * can do are not:
 *
 *   - IP_HDRINCL (build your own IP header, including source address) is
 *     REFUSED, not merely unimplemented -- see raw_icmp_send()'s comment.
 *   - bind()/connect() to filter by source address are not wired to this
 *     table (lsock_bind() rejects a socket already in S_RAW state with
 *     LSK_E_STATE); a raw socket here sees every inbound ICMP message
 *     regardless of source and a caller filters in userland, same as it must
 *     for id/seq matching anyway (see ping.c). This machine has exactly one
 *     local address, so a source-address bind on send would buy nothing a
 *     real multi-homed BSD box needs it for.
 *
 * ONLY IPPROTO_ICMP. The table has no protocol field because it holds
 * nothing else; lsock_create() is what refuses any other protocol number
 * before a raw_icmp_open() ever happens. */

#define RAW_MAX     8      /* simultaneous raw ICMP sockets -- generous for a
                            * single-user machine; NLSOCK (lsock.c) is the
                            * real ceiling on total sockets of any kind */
#define RAW_QUEUES  4      /* queued inbound messages per socket, same depth
                            * as UDP_QUEUES (udp.c) and for the same reason:
                            * one outstanding ping in flight is the normal
                            * case, a handful covers a burst */
#define RAW_SLOT    1500   /* one full ICMP message, MTU-sized like UDP_SLOT */

/* Open a raw ICMP socket for `pid` (recorded but not currently used to scope
 * delivery -- see raw_icmp_deliver()). Returns an id in [0, RAW_MAX), or -1
 * if the table is full. Privilege is NOT checked here: the caller
 * (lsock_create) is the one place that knows the CREDENTIAL, and checking it
 * twice in two files is how the two checks drift apart. */
int raw_icmp_open(int pid);
void raw_icmp_close(int id);

/* Send a complete ICMP message (type/code/checksum/id/seq/payload, all
 * supplied by the caller -- see the file comment on IP_HDRINCL) to `dst`
 * (host order). Returns the byte count sent, or -1. */
int raw_icmp_send(int id, uint32_t dst, const void *buf, int len);

/* Take the oldest queued inbound ICMP message (whatever icmp_input() handed
 * to raw_icmp_deliver() -- the full ICMP header + payload, no IP header).
 * Returns the byte count (0 = none queued yet), or -1 for a bad id. */
int raw_icmp_recv(int id, void *buf, int max, uint32_t *src);

/* icmp.c's hook, called from icmp_input() once the ICMP checksum has already
 * been verified: deliver a copy of the message to EVERY open raw ICMP
 * socket. Not scoped by pid, id or sequence number -- a raw socket sees
 * everything of its protocol, exactly like a real one; matching a specific
 * echo reply to the request that produced it is userland's job (ping.c
 * checks id+seq+source itself, the same thing the standalone `ping` command
 * on every other OS does). */
void raw_icmp_deliver(uint32_t src, const uint8_t *data, uint16_t len);

#endif /* LOGIT_RAW_H */
