#include "clib.h"
#include <stdlib.h>
#include <string.h>
#include "net_digest.inc"
#include "net_fetch.inc"

/* `net` -- the networking CLI (replaces the old Network GUI app; runs in the
 * Terminal's shell). Subcommands:
 *   net [info]        show IP / mask / gateway / MAC
 *   net ping          ping the gateway, print the round-trip time
 *   net dns <host>    resolve a hostname to an IPv4 address
 *   net get <url> [algorithm hex]      fetch and optionally verify a digest
 *   net save <url> <path> algorithm hex  verify then save (overwrites path)
 *   net download <url> [algorithm hex]  save under /download, no clobber
 *   net checksum algorithm <file|->    hash a file or stdin incrementally
 *   net verify algorithm hex <file|->  compare with a published checksum
 * Algorithms: sha256, sha512, blake2b (512 bits), blake3 (256 bits).
 * The old global HTTP syscall / 128 KiB prefix was replaced by http1's
 * per-process parser; net_fetch.inc owns the socket, framing and completion.
 * IRQ/softirq advances the network while these CLI commands yield. */

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

/* Subtract uptime, not the adjustable RTC (or midnight looks like timeout). */
static unsigned long long now_secs(void) { return monotonic_ms() / 1000; }

static int do_ping(void)
{
    struct logit_netinfo ni;
    if (net_info(&ni) != 1) { errs("net: no network interface\n"); return 1; }
    outs("ping "); ip_print(ni.gw); outs(" ...\n");
    /* The first send returns -1 until ARP resolves, so (re)send once a second and
     * poll for the reply (net_poll runs on the WM thread while we yield). */
    unsigned long long t0 = now_secs(), last = t0 - 1;
    for (;;) {
        int rtt = net_ping_rtt();
        if (rtt >= 0) { outs("reply: "); outn(rtt); outs(" ms\n"); return 0; }
        unsigned long long now = now_secs();
        if (now != last) { net_ping(ni.gw); last = now; }
        if (now - t0 >= 5) { outs("no reply\n"); return 1; }
        sys_yield();
    }
}

static int do_dns(const char *host)
{
    outs("resolving "); outs(host); outs(" ...\n");
    net_dns(host);                          /* send first; polling before any query reads a stale state */
    unsigned long long t0 = now_secs(), last = t0;
    for (;;) {
        unsigned r = net_dns_result();
        if (r && r != 0xFFFFFFFFu) { ip_print(r); outc('\n'); return 0; }
        unsigned long long now = now_secs();
        /* re-send each second, or right away if the kernel reported a miss -- the
         * first query is dropped while the resolver's ARP entry is still cold. */
        if (now != last || r == 0xFFFFFFFFu) { net_dns(host); last = now; }
        if (now - t0 >= 8) { outs("lookup failed\n"); return 1; }
        sys_yield();
    }
}

int main(int argc, char **argv)
{
    if (argc < 2 || c_streq(argv[1], "info")) return do_info();
    if (c_streq(argv[1], "ping")) return do_ping();
    if (c_streq(argv[1], "dns")) {
        if (argc < 3) { errs("usage: net dns <host>\n"); return 1; }
        return do_dns(argv[2]);
    }
    if (c_streq(argv[1], "get") && argc == 3) return do_get(argv[2], ND_SHA256, NULL, NULL, 0);
    if(c_streq(argv[1],"download")&&(argc==3||argc==5)){
        int alg=argc==5?nd_algorithm(argv[3]):ND_SHA256;uint8_t expected[64];
        if(alg<0||(argc==5&&nd_expected(alg,argv[4],expected)<0)){errs("net: invalid algorithm or expected digest\n");return 1;}
        return do_get(argv[2],alg,argc==5?expected:NULL,NULL,1);
    }
    if (c_streq(argv[1], "checksum") && argc >= 4) {
        int alg = nd_algorithm(argv[2]), rc = 0;
        if (alg < 0) { errs("net: unsupported digest algorithm\n"); return 1; }
        for (int i = 3; i < argc; i++) if (do_checksum(alg, argv[i], NULL)) rc = 1;
        return rc;
    }
    int verify = c_streq(argv[1], "verify") && argc == 5;
    int get = c_streq(argv[1], "get") && argc == 5;
    int save = c_streq(argv[1], "save") && argc == 6;
    if (verify || get || save) {
        int pos = verify ? 2 : save ? 4 : 3;
        int alg = nd_algorithm(argv[pos]); uint8_t expected[64];
        if (alg < 0 || nd_expected(alg, argv[pos+1], expected) < 0) {
            errs("net: invalid algorithm or expected digest\n"); return 1;
        }
        if (verify) return do_checksum(alg, argv[4], expected);
        return do_get(argv[2], alg, expected, save ? argv[3] : NULL, 0);
    }
    errs("usage: net info | ping | dns HOST | get URL [ALG HEX]\n"
         "       net download URL [ALG HEX] (saves in /download)\n"
         "       net checksum ALG FILE... | verify ALG HEX FILE\n"
         "       net save URL PATH ALG HEX (overwrites PATH after verification)\n"
         "ALG: sha256, sha512, blake2b, blake3; '-' reads stdin\n");
    return 1;
}
