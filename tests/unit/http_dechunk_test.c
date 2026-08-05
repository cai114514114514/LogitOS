/* Host unit test for the chunked transfer-encoding decoder in c/net/http/http.c.
 * http.c is #included whole (dechunk/te_is_chunked/chunked_done are static);
 * the network layer below it is stubbed and never called on these paths. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* --- stubs for everything http.c pulls in but we never execute --- */
void *kmalloc(unsigned long n) { return malloc(n); }
#include "net.h"
struct net_config net_cfg;
#include "tcp.h"
#include "tls.h"
#include "dns.h"
#include "arp.h"
#include "pit.h"
#include "rtc.h"
uint32_t dns_resolve(const char *h) { (void)h; return 0; }
int arp_warm(uint32_t ip, int t) { (void)ip; (void)t; return 0; }
int tcp_connect(uint32_t ip, uint16_t port) { (void)ip; (void)port; return -1; }
int tcp_send(int s, const void *b, int n) { (void)s; (void)b; (void)n; return -1; }
int tcp_recv(int s, void *b, int n) { (void)s; (void)b; (void)n; return -1; }
void tcp_close(int s) { (void)s; }
int tls_connect(int s, const char *h, int64_t now) { (void)s; (void)h; (void)now; return -1; }
int tls_send(int s, const void *b, int n) { (void)s; (void)b; (void)n; return -1; }
int tls_recv(int s, void *b, int n) { (void)s; (void)b; (void)n; return -1; }
void tls_close(int s) { (void)s; }
void net_poll(void) { }
void net_idle(void) { }
uint64_t timer_ticks(void) { return 0; }
void rtc_now(struct rtc_time *t) { (void)t; }

#include "../c/net/http/http.c"

static int fails;
#define OK(cond) do { if (cond) printf("ok   %s\n", #cond); \
                      else { printf("FAIL %s\n", #cond); fails++; } } while (0)

/* Run dechunk over a mutable copy of `wire`; returns decoded length or -1. */
static int dc(const char *wire, char *out)
{
    int n = (int)strlen(wire);
    memcpy(out, wire, n);
    int r = dechunk(out, n);
    if (r >= 0) out[r] = 0;
    return r;
}

int main(void)
{
    static char b[4096];

    OK(dc("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", b) == 9 && !strcmp(b, "Wikipedia"));
    OK(dc("A\r\n0123456789\r\n0\r\n\r\n", b) == 10 && !strcmp(b, "0123456789"));   /* uppercase hex */
    OK(dc("4;ext=1\r\nWiki\r\n0\r\n\r\n", b) == 4 && !strcmp(b, "Wiki"));        /* chunk extensions */
    OK(dc("3\r\nabc\r\n3\r\ndef\r\n3\r\nghi\r\n0\r\n\r\n", b) == 9 && !strcmp(b, "abcdefghi"));
    OK(dc("0\r\n\r\n", b) == 0);                                                /* empty body */
    OK(dc("0\r\nX-Trailer: x\r\n\r\n", b) == 0);                                /* trailers tolerated */
    OK(dc("5\r\nabc\r\n0\r\n\r\n", b) < 0);                                     /* size past end */
    OK(dc("zz\r\nabc\r\n0\r\n\r\n", b) < 0);                                    /* garbage size */
    OK(dc("3\r\nabcXX0\r\n\r\n", b) < 0);                                       /* missing CRLF after data */

    /* a bigger multi-chunk payload lands byte-exact */
    {
        char wire[2048], expect[1024]; int wp = 0, ep = 0;
        for (int c = 0; c < 40; c++) {
            wp += sprintf(wire + wp, "10\r\n");
            for (int i = 0; i < 16; i++) { wire[wp++] = 'A' + (c % 26); expect[ep++] = 'A' + (c % 26); }
            wp += sprintf(wire + wp, "\r\n");
        }
        wp += sprintf(wire + wp, "0\r\n\r\n");
        int r = dc(wire, b);
        OK(r == 640 && !memcmp(b, expect, 640));
    }

    OK(te_is_chunked("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", 51) == 1);
    OK(te_is_chunked("HTTP/1.1 200 OK\r\ntransfer-encoding: Chunked\r\n\r\n", 51) == 1);
    OK(te_is_chunked("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", 40) == 0);

    OK(chunked_done("0\r\n\r\n", 5) == 1);
    OK(chunked_done("4\r\nWiki\r\n0\r\n\r\n", 14) == 1);
    OK(chunked_done("4\r\nWiki\r\n", 9) == 0);

    /* header parse + status helpers still sane after the refactor */
    OK(status_code("HTTP/1.1 200 OK\r\n", 16) == 200);
    OK(find_body("HTTP/1.1 200 OK\r\n\r\nbody", 23) == 19);

    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("ALL PASS\n");
    return 0;
}
