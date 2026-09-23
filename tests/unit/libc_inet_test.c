/* arpa/inet.h case list -- same two-build diff strategy as libc_fnmatch_test.c
 * (tests/libc.mk compiles this once against our own headers/inet.c, once as
 * a normal host program against its libc, and diffs the output). Covers IPv4
 * dotted-quad, IPv6 canonical compression (RFC 5952: longest run, first on a
 * tie, a lone zero group never compressed), IPv4-mapped/compatible forms,
 * and pton rejecting malformed input.
 * Correction (2026-09-22): the old description called Darwin "glibc" too.
 * Darwin accepts leading decimal zeroes in pton; the stricter project policy
 * and BSD aton forms now have a separate libc_inet_contract_test.c gate. */
#define _DEFAULT_SOURCE 1
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

static void t_aton(const char *s)
{
    struct in_addr a;
    if (inet_aton(s, &a) != 1) {
        printf("aton %s -> BAD\n", s);
        return;
    }
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    printf("aton %s -> %s%s\n", s, buf,
           inet_addr(s) == a.s_addr ? "" : " (inet_addr differs)");
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
    t4bad("");

    /* Both BSD and glibc accept a final ASCII space and stop parsing there.
     * The text after that space is not another address component. */
    t_aton("1.2.3.4 ");
    t_aton("127.1 trailing");
    t_aton("0x7f.0.0.1\tcomment");

    t6("::"); t6("::1"); t6("1::"); t6("1::1");
    t6("2001:db8::1"); t6("2001:db8:0:0:1:0:0:1"); t6("fe80::1234:5678:9abc");
    t6("::ffff:192.168.1.1"); t6("::192.168.1.1");
    t6("2001:0db8:0000:0000:0000:0000:0000:0001");
    t6("1:0:0:2:0:0:0:3");     /* two runs of zeros: longest (the second) wins */
    t6("1:0:2:0:0:3:0:0");     /* tie in length: FIRST run wins */
    t6("0:0:0:0:0:0:0:1");
    t6bad(":::"); t6bad("1:2:3:4:5:6:7:8:9"); t6bad("12345::"); t6bad("::g");
    t6bad("1::2::3");
    t6bad("::1:"); t6bad("1:2:3:4:5:6:7:8:");
    t6bad("1:2:3:4:5:6:7:8::"); t6bad("::1:2:3:4:5:6:7:8");
    t6bad("1:2:3:4::5:6:7:8"); t6bad("1:2:3:4:5::6:192.0.2.1");
    t6("1:2:3:4:5::192.0.2.1"); t6("1:2:3:4:5:6:192.0.2.1");

    return 0;
}
