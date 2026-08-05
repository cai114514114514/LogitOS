#include "clib.h"

/* `net` -- the networking CLI (replaces the old Network GUI app; runs in the
 * Terminal's shell). Subcommands:
 *   net [info]        show IP / mask / gateway / MAC
 *   net ping          ping the gateway, print the round-trip time
 *   net dns <host>    resolve a hostname to an IPv4 address
 *   net get <url>     fetch HTTP(S), print body length + FNV-1a checksum
 * Ping/DNS are non-blocking in the kernel; we poll + sys_yield so the WM loop
 * pumps net_poll (the RX path) between polls. */

static void ip_print(unsigned ip)        /* host order: a.b.c.d */
{
    outn((ip >> 24) & 0xFF); outc('.');
    outn((ip >> 16) & 0xFF); outc('.');
    outn((ip >> 8)  & 0xFF); outc('.');
    outn( ip        & 0xFF);
}

static void mac_print(const unsigned char *m)
{
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (i) outc(':');
        outc(hex[m[i] >> 4]); outc(hex[m[i] & 0xF]);
    }
}

static int do_info(void)
{
    struct logit_netinfo ni;
    if (net_info(&ni) != 1) { errs("net: no network interface\n"); return 1; }
    outs("ip    "); ip_print(ni.ip);   outc('\n');
    outs("mask  "); ip_print(ni.mask); outc('\n');
    outs("gw    "); ip_print(ni.gw);   outc('\n');
    outs("mac   "); mac_print(ni.mac); outc('\n');
    return 0;
}

/* Seconds-of-day, for coarse wall-clock timeouts (RTC has 1 s resolution). */
static int now_secs(void)
{
    struct logit_time t; get_time(&t);
    return t.hour * 3600 + t.minute * 60 + t.second;
}

static int do_ping(void)
{
    struct logit_netinfo ni;
    if (net_info(&ni) != 1) { errs("net: no network interface\n"); return 1; }
    outs("ping "); ip_print(ni.gw); outs(" ...\n");
    /* The first send returns -1 until ARP resolves, so (re)send once a second and
     * poll for the reply (net_poll runs on the WM thread while we yield). */
    int t0 = now_secs(), last = -1;
    for (;;) {
        int rtt = net_ping_rtt();
        if (rtt >= 0) { outs("reply: "); outn(rtt); outs(" ms\n"); return 0; }
        int now = now_secs();
        if (now != last) { net_ping(ni.gw); last = now; }
        if (now - t0 < 0 || now - t0 >= 5) { outs("no reply\n"); return 1; }
        sys_yield();
    }
}

static int do_dns(const char *host)
{
    outs("resolving "); outs(host); outs(" ...\n");
    net_dns(host);                          /* send first; polling before any query reads a stale state */
    int t0 = now_secs(), last = t0;
    for (;;) {
        unsigned r = net_dns_result();
        if (r && r != 0xFFFFFFFFu) { ip_print(r); outc('\n'); return 0; }
        int now = now_secs();
        /* re-send each second, or right away if the kernel reported a miss -- the
         * first query is dropped while the resolver's ARP entry is still cold. */
        if (now != last || r == 0xFFFFFFFFu) { net_dns(host); last = now; }
        if (now - t0 < 0 || now - t0 >= 8) { outs("lookup failed\n"); return 1; }
        sys_yield();
    }
}

#define GET_MAX (128 * 1024)
static unsigned char get_buf[GET_MAX];

static int do_get(const char *url)
{
    int rc = http_get(url);
    if (rc < 0 || http_status() != 2) {
        errs("net: fetch failed (rc="); outn_fd(2, rc); errs(")\n");
        return 1;
    }
    int n = http_body((char *)get_buf, sizeof get_buf);
    if (n < 0) { errs("net: could not read response body\n"); return 1; }
    unsigned hash = 2166136261u;
    for (int i = 0; i < n; i++) { hash ^= get_buf[i]; hash *= 16777619u; }
    outs("http bytes "); outn(n); outs(" fnv1a "); outn(hash); outc('\n');
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || c_streq(argv[1], "info")) return do_info();
    if (c_streq(argv[1], "ping")) return do_ping();
    if (c_streq(argv[1], "dns")) {
        if (argc < 3) { errs("usage: net dns <host>\n"); return 1; }
        return do_dns(argv[2]);
    }
    if (c_streq(argv[1], "get")) {
        if (argc < 3) { errs("usage: net get <url>\n"); return 1; }
        return do_get(argv[2]);
    }
    errs("usage: net [info | ping | dns <host> | get <url>]\n");
    return 1;
}
