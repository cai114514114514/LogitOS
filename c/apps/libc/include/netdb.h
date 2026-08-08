#ifndef _NETDB_H
#define _NETDB_H
#include <netinet/in.h>
#include <sys/socket.h>

/* Name resolution is REAL (SYS_NET_DNS / SYS_NET_DNS_RESULT -- the same
 * primitive c/net/dns.c's client uses, and the same one http_get() polls),
 * even though the socket layer above it is not (see <sys/socket.h>).
 * getaddrinfo() therefore gives a caller a genuine struct sockaddr_in it
 * could connect() with on a system where connect() worked; on THIS system it
 * is still useful on its own (a program that just wants "what IP does this
 * name have" -- ping, a resolver test, a URL validator) and it fails
 * honestly (EAI_NONAME/EAI_AGAIN) rather than fabricating an address.
 * IPv6 (AF_INET6) is refused with EAI_FAMILY: the kernel's DNS client
 * resolves A records only. */

struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]

#define AI_PASSIVE     0x01
#define AI_CANONNAME   0x02
#define AI_NUMERICHOST 0x04
#define AI_NUMERICSERV 0x08

#define EAI_AGAIN    -3
#define EAI_BADFLAGS -1
#define EAI_FAIL     -4
#define EAI_FAMILY   -6
#define EAI_MEMORY   -10
#define EAI_NONAME   -2
#define EAI_SERVICE  -8
#define EAI_SOCKTYPE -7
#define EAI_SYSTEM   -11

int  getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int errcode);

struct hostent *gethostbyname(const char *name);

#endif /* _NETDB_H */
