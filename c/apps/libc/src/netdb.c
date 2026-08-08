/* <netdb.h>: name resolution over the real SYS_NET_DNS/SYS_NET_DNS_RESULT
 * pair. See the header for what this can and cannot back (a real IP for a
 * real name; no working socket to use it with yet). */
#include <netdb.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "logit_abi.h"

static long nsys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

/* Resolve `name` to a host-order IPv4 address. Returns 0 and fills *out on
 * success; a negative EAI_* code on failure. Numeric literals ("10.0.2.2")
 * are handled locally -- the DNS syscall documents that it also accepts
 * them, but going through inet_pton here means "is this numeric" never
 * depends on network state at all. */
static int resolve4(const char *name, unsigned *out)
{
    struct in_addr a;
    if (inet_pton(AF_INET, name, &a) == 1) { *out = (unsigned)ntohl(a.s_addr); return 0; }

    if (nsys(SYS_NET_DNS, (long)name, 0, 0) < 0) return EAI_FAIL;   /* no NIC */

    unsigned long long t0 = (unsigned long long)nsys(SYS_MONOTONIC_MS, 0, 0, 0);
    for (;;) {
        long r = nsys(SYS_NET_DNS_RESULT, 0, 0, 0);
        if (r != 0) {
            if ((unsigned long)r == 0xFFFFFFFFul) return EAI_NONAME;
            *out = (unsigned)r;
            return 0;
        }
        if ((unsigned long long)nsys(SYS_MONOTONIC_MS, 0, 0, 0) - t0 > 8000ull) return EAI_AGAIN;
        nsys(SYS_YIELD, 0, 0, 0);
    }
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    if (!res) return EAI_MEMORY;
    *res = NULL;
    if (!node) return EAI_NONAME;
    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET) return EAI_FAMILY;

    unsigned ip;
    int rc = resolve4(node, &ip);
    if (rc) return rc;

    unsigned short port = 0;
    if (service) {
        int allnum = *service != 0;
        for (const char *p = service; *p; p++) if (!isdigit((unsigned char)*p)) { allnum = 0; break; }
        if (allnum) port = (unsigned short)atoi(service);
        else return EAI_SERVICE;   /* no /etc/services to look a name up in */
    }

    struct sockaddr_in *sin = calloc(1, sizeof *sin);
    struct addrinfo *ai = calloc(1, sizeof *ai);
    if (!sin || !ai) { free(sin); free(ai); return EAI_MEMORY; }
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr.s_addr = htonl(ip);
    ai->ai_family = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_addrlen = sizeof *sin;
    ai->ai_addr = (struct sockaddr *)sin;
    if (hints && (hints->ai_flags & AI_CANONNAME)) ai->ai_canonname = strdup(node);
    *res = ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res->ai_canonname);
        free(res);
        res = next;
    }
}

const char *gai_strerror(int errcode)
{
    switch (errcode) {
    case 0:            return "Success";
    case EAI_AGAIN:    return "Temporary failure in name resolution";
    case EAI_BADFLAGS: return "Invalid flags";
    case EAI_FAIL:     return "Non-recoverable failure (no network interface)";
    case EAI_FAMILY:   return "Address family not supported (IPv4 A records only)";
    case EAI_MEMORY:   return "Out of memory";
    case EAI_NONAME:   return "Name does not resolve";
    case EAI_SERVICE:  return "Service not numeric (no /etc/services)";
    case EAI_SOCKTYPE: return "Socket type not supported";
    default:           return "Unknown resolver error";
    }
}

/* Legacy BSD interface, kept minimal: one A record, no aliases. */
struct hostent *gethostbyname(const char *name)
{
    static struct hostent he;
    static char namebuf[256];
    static struct in_addr addrbuf;
    static char *addrlist[2];
    static char *aliaslist[1];

    unsigned ip;
    if (!name || resolve4(name, &ip) != 0) return NULL;
    addrbuf.s_addr = htonl(ip);
    strlcpy(namebuf, name, sizeof namebuf);
    addrlist[0] = (char *)&addrbuf;
    addrlist[1] = NULL;
    aliaslist[0] = NULL;
    he.h_name = namebuf;
    he.h_aliases = aliaslist;
    he.h_addrtype = AF_INET;
    he.h_length = 4;
    he.h_addr_list = addrlist;
    return &he;
}
