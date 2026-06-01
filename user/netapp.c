#include "aqua.h"

/* Network app: shows our IP/MAC/gateway, pings the gateway, and resolves a
 * hostname via DNS. A ring-3 process talking to the kernel net stack over
 * syscalls; results arrive asynchronously (the WM pumps net_poll), so we poll. */

#define WINW 460
#define WINH 300

static char host[64] = "example.com";
static int  hlen = 11;

static char line1[80], line2[80], pingmsg[80], dnsmsg[80];

/* append a decimal number to s at *pos */
static void putnum(char *s, int *pos, unsigned v)
{
    char t[12]; int i = 0;
    if (!v) { s[(*pos)++] = '0'; return; }
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) s[(*pos)++] = t[--i];
}
static void putstr(char *s, int *pos, const char *x) { while (*x) s[(*pos)++] = *x++; }
static void ip_str(char *s, int *pos, unsigned ip)
{
    putnum(s, pos, (ip >> 24) & 0xFF); s[(*pos)++] = '.';
    putnum(s, pos, (ip >> 16) & 0xFF); s[(*pos)++] = '.';
    putnum(s, pos, (ip >> 8) & 0xFF);  s[(*pos)++] = '.';
    putnum(s, pos, ip & 0xFF);
}
static void hex2(char *s, int *pos, unsigned v)
{
    const char *h = "0123456789abcdef";
    s[(*pos)++] = h[(v >> 4) & 0xF]; s[(*pos)++] = h[v & 0xF];
}

static void redraw(void)
{
    gui_clear(rgb(250, 250, 252));
    gui_text(12, 10, rgb(40, 40, 48), "Network");
    gui_text(12, 38, rgb(70, 70, 80), line1);
    gui_text(12, 58, rgb(70, 70, 80), line2);

    gui_text(12, 96, rgb(110, 110, 120), "Enter = ping gateway");
    gui_text(12, 116, rgb(40, 160, 80), pingmsg);

    gui_text(12, 150, rgb(110, 110, 120), "Host (type, then 'd' won't work -- press Tab to resolve):");
    gui_rect(12, 170, WINW - 24, 20, rgb(238, 238, 242));
    gui_text(16, 172, rgb(40, 40, 48), host);
    gui_text(12, 200, rgb(90, 90, 200), dnsmsg);
    gui_flush();
}

static void refresh_info(void)
{
    struct aqua_netinfo ni;
    if (net_info(&ni)) {
        int p = 0; putstr(line1, &p, "IP "); ip_str(line1, &p, ni.ip);
        putstr(line1, &p, "  GW "); ip_str(line1, &p, ni.gw); line1[p] = 0;
        p = 0; putstr(line2, &p, "MAC ");
        for (int i = 0; i < 6; i++) { if (i) line2[p++] = ':'; hex2(line2, &p, ni.mac[i]); }
        line2[p] = 0;
    } else {
        int p = 0; putstr(line1, &p, "no network"); line1[p] = 0; line2[0] = 0;
    }
}

void app_main(void)
{
    gui_create("Network", WINW, WINH);
    refresh_info();
    int p = 0; putstr(pingmsg, &p, "(press Enter)"); pingmsg[p] = 0;
    p = 0; putstr(dnsmsg, &p, "(press Tab)"); dnsmsg[p] = 0;
    redraw();

    /* The first send on a cold ARP cache fails (resolve pending), so re-issue
     * each loop while waiting and give up after a bounded number of tries. */
    int pinging = 0, resolving = 0, tries = 0;
    unsigned gw = 0;
    for (;;) {
        struct aqua_event e;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) app_exit(0);
            if (e.type == EV_KEY) {
                char c = (char)e.a;
                if (c == '\n') {                 /* ping the gateway */
                    struct aqua_netinfo ni;
                    if (net_info(&ni)) {
                        gw = ni.gw; net_ping(gw);
                        pinging = 1; tries = 0;
                        int q = 0; putstr(pingmsg, &q, "pinging..."); pingmsg[q] = 0;
                    }
                } else if (c == '\t') {          /* resolve the host */
                    net_dns(host);
                    resolving = 1; tries = 0;
                    int q = 0; putstr(dnsmsg, &q, "resolving..."); dnsmsg[q] = 0;
                } else if (c == '\b') {
                    if (hlen > 0) host[--hlen] = 0;
                } else if (hlen < (int)sizeof host - 1 && c >= ' ') {
                    host[hlen++] = c; host[hlen] = 0;
                }
                redraw();
            }
        }
        if (pinging) {
            int rtt = net_ping_rtt();
            if (rtt >= 0) {
                int q = 0; putstr(pingmsg, &q, "reply "); putnum(pingmsg, &q, (unsigned)rtt);
                putstr(pingmsg, &q, " ms"); pingmsg[q] = 0;
                pinging = 0; redraw();
            } else if (++tries < 200) {
                if ((tries & 31) == 0) net_ping(gw);     /* re-issue (warms ARP) */
            } else {
                int q = 0; putstr(pingmsg, &q, "no reply"); pingmsg[q] = 0;
                pinging = 0; redraw();
            }
        }
        if (resolving) {
            unsigned r = net_dns_result();
            if (r == 0xFFFFFFFFu) {
                int q = 0; putstr(dnsmsg, &q, "lookup failed"); dnsmsg[q] = 0;
                resolving = 0; redraw();
            } else if (r) {
                int q = 0; ip_str(dnsmsg, &q, r); dnsmsg[q] = 0;
                resolving = 0; redraw();
            } else if (++tries % 64 == 0) {
                net_dns(host);                           /* re-issue while pending */
            }
        }
        sys_yield();
    }
}
