/* arpa/inet.h case list -- same two-build diff strategy as libc_fnmatch_test.c
 * (tests/libc.mk compiles this once against our own headers/inet.c, once as
 * a normal host program against glibc, and diffs the output). Covers IPv4
 * dotted-quad, IPv6 canonical compression (RFC 5952: longest run, first on a
 * tie, a lone zero group never compressed), IPv4-mapped/compatible forms,
 * and pton rejecting malformed input. */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static void t4(const char *s)
{
    struct in_addr a;
    int ok = inet_pton(AF_INET, s, &a);
    if (ok != 1) { printf("%s -> BAD\n", s); return; }
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    printf("%s -> %s\n", s, buf);
}
static void t4bad(const char *s)
{
    struct in_addr a;
    printf("%s -> %s\n", s, inet_pton(AF_INET, s, &a) == 1 ? "OK" : "BAD");
}

static void t6(const char *s)
{
    unsigned char a[16];
    int ok = inet_pton(AF_INET6, s, a);
    if (ok != 1) { printf("%s -> BAD\n", s); return; }
    char buf[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, a, buf, sizeof buf);
    printf("%s -> %s\n", s, buf);
}
static void t6bad(const char *s)
{
    unsigned char a[16];
    printf("%s -> %s\n", s, inet_pton(AF_INET6, s, a) == 1 ? "OK" : "BAD");
}

int main(void)
{
    t4("0.0.0.0"); t4("255.255.255.255"); t4("127.0.0.1"); t4("10.0.2.15");
    t4("192.168.1.1"); t4("1.2.3.4");
    t4bad("256.1.1.1"); t4bad("1.2.3"); t4bad("1.2.3.4.5"); t4bad("a.b.c.d");
    t4bad(""); t4bad("1.2.3.04"); t4bad("01.2.3.4");

    t6("::"); t6("::1"); t6("1::"); t6("1::1");
    t6("2001:db8::1"); t6("2001:db8:0:0:1:0:0:1"); t6("fe80::1234:5678:9abc");
    t6("::ffff:192.168.1.1"); t6("::192.168.1.1");
    t6("2001:0db8:0000:0000:0000:0000:0000:0001");
    t6("1:0:0:2:0:0:0:3");     /* two runs of zeros: longest (the second) wins */
    t6("1:0:2:0:0:3:0:0");     /* tie in length: FIRST run wins */
    t6("0:0:0:0:0:0:0:1");
    t6bad(":::"); t6bad("1:2:3:4:5:6:7:8:9"); t6bad("12345::"); t6bad("::g");
    t6bad("1::2::3");

    return 0;
}
