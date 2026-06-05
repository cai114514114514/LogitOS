#include "aui.h"

/* Network app: shows our IP/MAC/gateway, pings the gateway, and resolves a host
 * via DNS -- now a proper aui form (buttons + a text field). Results arrive
 * asynchronously (the WM pumps net_poll), so we poll and re-frame on updates. */

#define WINW 460
#define WINH 320

static char host[64] = "example.com";
static char line1[80], line2[80], pingmsg[80], dnsmsg[80];
static int  pinging, resolving, tries;
static unsigned gw;

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

static void refresh_info(void)
{
    struct aqua_netinfo ni;
    if (net_info(&ni)) {
        int p = 0; putstr(line1, &p, "IP "); ip_str(line1, &p, ni.ip);
        putstr(line1, &p, "   GW "); ip_str(line1, &p, ni.gw); line1[p] = 0;
        p = 0; putstr(line2, &p, "MAC ");
        for (int i = 0; i < 6; i++) { if (i) line2[p++] = ':'; hex2(line2, &p, ni.mac[i]); }
        line2[p] = 0;
    } else { int p = 0; putstr(line1, &p, "no network"); line1[p] = 0; line2[0] = 0; }
}

static void start_ping(void)
{
    struct aqua_netinfo ni;
    if (net_info(&ni)) { gw = ni.gw; net_ping(gw); pinging = 1; tries = 0;
        int q = 0; putstr(pingmsg, &q, "pinging..."); pingmsg[q] = 0; }
}
static void start_dns(void)
{
    net_dns(host); resolving = 1; tries = 0;
    int q = 0; putstr(dnsmsg, &q, "resolving..."); dnsmsg[q] = 0;
}

static void frame(void)
{
    aui_begin(AUI_BG);
    aui_label(20, 14, "Network", AUI_TEXT);
    aui_label(20, 46, line1, AUI_MUTED);
    aui_label(20, 68, line2, AUI_MUTED);

    if (aui_button(20, 102, 150, 30, "Ping gateway")) start_ping();
    aui_label(186, 110, pingmsg, AUI_ACCENT);

    aui_label(20, 158, "Host:", AUI_MUTED);
    if (aui_textfield(74, 152, 264, host, sizeof host)) start_dns();   /* Enter resolves */
    if (aui_button(20, 196, 150, 30, "Resolve DNS")) start_dns();
    aui_label(186, 204, dnsmsg, AUI_ACCENT);
    aui_end();
}

void app_main(void)
{
    gui_create("Network", WINW, WINH);
    refresh_info();
    { int q = 0; putstr(pingmsg, &q, "(click Ping)"); pingmsg[q] = 0; }
    { int q = 0; putstr(dnsmsg, &q, "(type a host, click Resolve)"); dnsmsg[q] = 0; }
    frame();

    for (;;) {
        struct aqua_event e;
        while (poll_event(&e)) {
            if (e.type == EV_CLOSE) app_exit(0);
            aui_feed(&e); frame(); aui_feed_done();      /* handle clicks/typing */
        }
        if (pinging) {
            int rtt = net_ping_rtt();
            if (rtt >= 0) { int q = 0; putstr(pingmsg, &q, "reply "); putnum(pingmsg, &q, (unsigned)rtt);
                putstr(pingmsg, &q, " ms"); pingmsg[q] = 0; pinging = 0; frame(); }
            else if (++tries < 200) { if ((tries & 31) == 0) net_ping(gw); }
            else { int q = 0; putstr(pingmsg, &q, "no reply"); pingmsg[q] = 0; pinging = 0; frame(); }
        }
        if (resolving) {
            unsigned r = net_dns_result();
            if (r == 0xFFFFFFFFu) { int q = 0; putstr(dnsmsg, &q, "lookup failed"); dnsmsg[q] = 0; resolving = 0; frame(); }
            else if (r) { int q = 0; ip_str(dnsmsg, &q, r); dnsmsg[q] = 0; resolving = 0; frame(); }
            else if (++tries % 64 == 0) net_dns(host);
        }
        sys_yield();
    }
}
