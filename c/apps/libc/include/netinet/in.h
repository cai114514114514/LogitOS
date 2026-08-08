#ifndef _NETINET_IN_H
#define _NETINET_IN_H
#include <stdint.h>
#include <sys/types.h>

/* Byte-order + address structs. Pure computation -- no kernel involvement --
 * so these are exactly as correct as any other libc's. What is NOT here is a
 * working socket(2)/bind()/listen()/accept(): the kernel's network syscalls
 * (SYS_SOCK_OPEN et al, include/abi/logit_abi.h) are a client-only,
 * hostname-based, non-sockaddr API with no server side at all, so a
 * sockaddr_in these structs describe cannot actually be handed to a working
 * bind()/listen(). See <sys/socket.h> (not provided) in the libc inventory
 * report for the precise gap. */

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in_addr { in_addr_t s_addr; };

struct in6_addr {
    union {
        uint8_t  __u6_addr8[16];
        uint16_t __u6_addr16[8];
        uint32_t __u6_addr32[4];
    } __in6_u;
#define s6_addr   __in6_u.__u6_addr8
#define s6_addr16 __in6_u.__u6_addr16
#define s6_addr32 __in6_u.__u6_addr32
};

extern const struct in6_addr in6addr_any;
extern const struct in6_addr in6addr_loopback;
#define IN6ADDR_ANY_INIT      {{{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}}}
#define IN6ADDR_LOOPBACK_INIT {{{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}}}

#define INADDR_ANY       0x00000000u
#define INADDR_BROADCAST 0xffffffffu
#define INADDR_LOOPBACK  0x7f000001u
#define INADDR_NONE      0xffffffffu

#define AF_UNSPEC 0
#define AF_INET   2
#define AF_INET6  10
#define AF_UNIX   1
#define PF_INET   AF_INET
#define PF_INET6  AF_INET6
#define PF_UNSPEC AF_UNSPEC

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_IPV6 41
#define IPPROTO_RAW  255

struct sockaddr_in {
    short          sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    unsigned char  sin_zero[8];
};

struct sockaddr_in6 {
    short           sin6_family;
    in_port_t       sin6_port;
    uint32_t        sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t        sin6_scope_id;
};

/* x86-64 is little-endian everywhere this tree targets (see the identical
 * statement in <endian.h>), so the network-order conversions are a genuine
 * byte swap, not a no-op wearing a function name. */
static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v)
{ return ((v & 0x000000ffu) << 24) | ((v & 0x0000ff00u) << 8) |
         ((v & 0x00ff0000u) >> 8) | ((v & 0xff000000u) >> 24); }
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

#endif /* _NETINET_IN_H */
