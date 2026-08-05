#include <stdint.h>
#include <stddef.h>
#include "http.h"
#include "url.h"
#include "tcp.h"
#include "tls.h"
#include "dns.h"
#include "net.h"
#include "arp.h"
#include "pit.h"
#include "rtc.h"

void *memcpy(void *, const void *, size_t);
void *kmalloc(unsigned long);

#define RAW_MAX  (512*1024)   /* big enough for large pages + stylesheets (github's
                              * primer CSS is hundreds of KB); the TCP 64 KiB window
                              * slides as fetch_once drains raw[] continuously. */

static char raw[RAW_MAX];
static int  raw_len;
static int  body_off;                       /* offset of the body within raw[] */
static int  status = HTTP_IDLE;
static int  http_busy = 0;
static struct url cur;                      /* the page currently loaded (link base) */

int  http_status(void)     { return status; }

/* After HTTP_DONE: the response body (HTML source) and its length. */
const char *http_body(int *len)
{
    if (len) *len = (status == HTTP_DONE && body_off >= 0) ? raw_len - body_off : 0;
    return raw + (body_off >= 0 ? body_off : 0);
}

/* Build "GET <path> HTTP/1.0\r\nHost: <host>\r\nConnection: close\r\n\r\n". */
static int build_request(const struct url *u, char *req, int max)
{
    int o = 0;
    const char *g = "GET ";
    for (const char *p = g; *p && o < max; p++) req[o++] = *p;
    for (const char *p = u->path; *p && o < max; p++) req[o++] = *p;
    const char *v = " HTTP/1.0\r\nHost: ";
    for (const char *p = v; *p && o < max; p++) req[o++] = *p;
    for (const char *p = u->host; *p && o < max; p++) req[o++] = *p;
    const char *tail = "\r\nConnection: close\r\nUser-Agent: Aether/1.0\r\n\r\n";
    for (const char *p = tail; *p && o < max; p++) req[o++] = *p;
    return o;
}

/* Current wall-clock time as unix-ish seconds, on the same proleptic-Gregorian,
 * no-leap-second scale that x509.c's parse_time uses, so cert validity windows
 * compare correctly. Sourced from the CMOS RTC (QEMU tracks the host clock). */
static int64_t now_unix(void)
{
    struct rtc_time t; rtc_now(&t);
    int64_t days = 0;
    for (int y = 1970; y < t.year; y++)
        days += (y%4==0 && (y%100!=0 || y%400==0)) ? 366 : 365;
    static const int md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    /* Clamp a bogus RTC month so md[m-1] can't index past md[12]. */
    for (int m = 1; m < t.month && m <= 12; m++) {
        days += md[m-1];
        if (m==2 && (t.year%4==0 && (t.year%100!=0 || t.year%400==0))) days++;
    }
    days += t.day - 1;
    return ((days*24 + t.hour)*60 + t.minute)*60 + t.second;
}

/* Find the body (after the first CRLFCRLF), return offset or -1. */
static int find_body(const char *buf, int len)
{
    for (int i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return i + 4;
    return -1;
}

/* Status code from the "HTTP/1.x NNN ..." response line, or 0. */
static int status_code(const char *buf, int len)
{
    int i = 0;
    while (i < len && buf[i] != ' ') i++;        /* skip "HTTP/1.x" */
    i++;
    if (i + 3 > len) return 0;
    if (buf[i] < '0' || buf[i] > '9') return 0;
    return (buf[i]-'0')*100 + (buf[i+1]-'0')*10 + (buf[i+2]-'0');
}

/* Extract a header value (case-insensitive name) into out. 0 ok, -1 not found. */
static int header_value(const char *buf, int len, const char *name, char *out, int max)
{
    int nl = 0; while (name[nl]) nl++;
    for (int i = 0; i + nl + 1 < len; i++) {
        if (i != 0 && buf[i-1] != '\n') continue;            /* at line start */
        int j = 0;
        for (; j < nl; j++) {
            char a = buf[i+j], b = name[j];
            if (a>='A'&&a<='Z') a+=32; if (b>='A'&&b<='Z') b+=32;
            if (a != b) break;
        }
        if (j != nl || buf[i+nl] != ':') continue;
        int p = i + nl + 1;
        while (p < len && (buf[p]==' '||buf[p]=='\t')) p++;
        int o = 0;
        while (p < len && buf[p] != '\r' && buf[p] != '\n' && o < max-1) out[o++] = buf[p++];
        out[o] = 0;
        return 0;
    }
    return -1;
}

/* One request/response into raw[]/raw_len for URL `u`. 0 ok, else HTTP_ERR_*. */
static int fetch_once(const struct url *u)
{
    raw_len = 0;
    uint32_t ip = dns_resolve(u->host);
    if (!ip) return HTTP_ERR_DNS;
    /* Warm the next-hop's ARP (gateway for off-subnet, else the host itself) so
     * the SYN isn't dropped on a cold cache -> no 0.5 s retransmit stall. */
    uint32_t nexthop = ((ip & net_cfg.mask) == (net_cfg.ip & net_cfg.mask)) ? ip : net_cfg.gw;
    arp_warm(nexthop, 30);
    int tcp = tcp_connect(ip, u->port);
    if (tcp < 0) return HTTP_ERR_CONN;

    int tls = -1;
    if (u->https) {
        tls = tls_connect(tcp, u->host, now_unix());
        if (tls < 0) { tcp_close(tcp); return HTTP_ERR_TLS; }
    }

    char req[URL_PATH_MAX + URL_HOST_MAX + 64];
    int rl = build_request(u, req, (int)sizeof req);
    if (tls >= 0) tls_send(tls, req, rl); else tcp_send(tcp, req, rl);

    int hdr_end = -1;        /* offset past the response headers, once seen */
    int clen = -1;           /* Content-Length, if the server sent one */
    uint64_t start = timer_ticks();
    while (timer_ticks() - start < 800) {       /* ~8 s idle budget */
        net_poll();
        int n = (tls >= 0) ? tls_recv(tls, raw + raw_len, RAW_MAX - raw_len)
                           : tcp_recv(tcp, raw + raw_len, RAW_MAX - raw_len);
        if (n > 0) {
            raw_len += n;
            start = timer_ticks();
            /* Once we have the full body per Content-Length, stop -- don't sit idle
             * waiting for the server's FIN (often ~1 s later), which made every
             * page feel sluggish even though the bytes had all arrived. */
            if (hdr_end < 0 && (hdr_end = find_body(raw, raw_len)) >= 0) {
                char cl[16];
                if (header_value(raw, hdr_end, "content-length", cl, sizeof cl) == 0) {
                    clen = 0;
                    /* Cap the accumulation: raw_len can never exceed RAW_MAX, so
                     * saturating at RAW_MAX keeps the 'have the whole body'
                     * short-circuit exact without risking signed overflow. */
                    for (const char *p = cl; *p >= '0' && *p <= '9'; p++) {
                        if (clen > (RAW_MAX - 9) / 10) { clen = RAW_MAX; break; }
                        clen = clen*10 + (*p-'0');
                    }
                }
            }
            if (clen >= 0 && hdr_end >= 0 && raw_len - hdr_end >= clen) break;
            if (raw_len >= RAW_MAX) break;
            continue;                            /* data flowing: drain tightly, no delay */
        }
        if (n < 0) break;
        net_idle();                                  /* no data yet: sleep until the RX IRQ/tick, don't peg host CPU */
    }
    if (tls >= 0) tls_close(tls);
    tcp_close(tcp);
    return raw_len == 0 ? HTTP_ERR_CONN : 0;
}

int http_get(const char *url)
{
    if (http_busy) return HTTP_ERR_CONN;
    http_busy = 1;
    status = HTTP_BUSY;
    raw_len = 0; body_off = -1;

    if (url_parse(url, &cur) != 0) { status = HTTP_ERR_URL; http_busy = 0; return HTTP_ERR_URL; }

    /* Follow up to 5 redirects (301/302/303/307/308 with a Location header), so
     * e.g. https://google.com lands on https://www.google.com like a real browser. */
    for (int hop = 0; hop < 5; hop++) {
        int rc = fetch_once(&cur);
        if (rc != 0) { status = rc; http_busy = 0; return rc; }

        int code = status_code(raw, raw_len);
        if (code >= 300 && code < 400) {
            int hdr = find_body(raw, raw_len);
            int hlen = hdr < 0 ? raw_len : hdr;
            char loc[URL_HOST_MAX + URL_PATH_MAX + 16];
            char abs[URL_HOST_MAX + URL_PATH_MAX + 16];
            struct url nu;
            if (header_value(raw, hlen, "location", loc, sizeof loc) == 0 &&
                url_resolve(&cur, loc, abs, sizeof abs) == 0 &&
                url_parse(abs, &nu) == 0) {
                cur = nu;                            /* hop to the new location */
                continue;
            }
        }
        break;                                       /* final (non-redirect) response */
    }

    int body = find_body(raw, raw_len);
    body_off = body < 0 ? 0 : body;
    status = HTTP_DONE;
    http_busy = 0;
    return 0;
}

void http_poll(void) { }                         /* fetch is synchronous in http_get */

/* The base URL of the page currently loaded (for resolving sub-resources). */
const struct url *http_page_url(void) { return &cur; }

/* ---- sub-resource fetch (images via the layout engine) ---- */
static int b64v(int c)
{
    if (c>='A'&&c<='Z') return c-'A';
    if (c>='a'&&c<='z') return c-'a'+26;
    if (c>='0'&&c<='9') return c-'0'+52;
    if (c=='+') return 62; if (c=='/') return 63;
    return -1;
}

/* Decode a "data:[<mime>][;base64],<data>" URI into a fresh kmalloc buffer. */
static int decode_data_uri(const char *u, uint8_t **buf, int *len)
{
    const char *comma = u; while (*comma && *comma != ',') comma++;
    if (*comma != ',') return -1;
    int is_b64 = 0;
    for (const char *p = u + 5; p < comma; p++)
        if ((p[0]=='b'||p[0]=='B') && p+6<=comma &&
            p[1]=='a'&&p[2]=='s'&&p[3]=='e'&&p[4]=='6'&&p[5]=='4') { is_b64 = 1; break; }
    const char *d = comma + 1;
    int dn = 0; while (d[dn]) dn++;
    if (!is_b64) {                               /* treat as raw bytes (URL-ish) */
        uint8_t *b = kmalloc(dn ? dn : 1); if (!b) return -1;
        for (int i = 0; i < dn; i++) b[i] = (uint8_t)d[i];
        *buf = b; *len = dn; return 0;
    }
    uint8_t *b = kmalloc((dn/4)*3 + 4); if (!b) return -1;
    int o = 0, acc = 0, nb = 0;
    for (int i = 0; i < dn; i++) {
        int v = b64v(d[i]); if (v < 0) continue;
        acc = (acc << 6) | v; nb += 6;
        if (nb >= 8) { nb -= 8; b[o++] = (uint8_t)((acc >> nb) & 0xFF); }
    }
    *buf = b; *len = o; return 0;
}

/* Fetch a sub-resource (image) relative to the current page. Returns 0 with a
 * kmalloc'd body the caller frees, or -1. Reuses raw[] (the DOM was already
 * built from the page body, which copied its strings). */
int res_fetch(const char *src, uint8_t **buf, int *len)
{
    if (!src || !*src) return -1;
    if (src[0]=='d'&&src[1]=='a'&&src[2]=='t'&&src[3]=='a'&&src[4]==':')
        return decode_data_uri(src, buf, len);

    char abs[URL_HOST_MAX + URL_PATH_MAX + 16];
    struct url u;
    if (url_resolve(&cur, src, abs, sizeof abs) != 0) return -1;
    if (url_parse(abs, &u) != 0) return -1;

    for (int hop = 0; hop < 4; hop++) {
        if (fetch_once(&u) != 0) return -1;
        int code = status_code(raw, raw_len);
        if (code >= 300 && code < 400) {
            int hdr = find_body(raw, raw_len); int hlen = hdr < 0 ? raw_len : hdr;
            char loc[URL_HOST_MAX + URL_PATH_MAX + 16], a2[sizeof loc]; struct url nu;
            if (header_value(raw, hlen, "location", loc, sizeof loc) == 0 &&
                url_resolve(&u, loc, a2, sizeof a2) == 0 && url_parse(a2, &nu) == 0) {
                u = nu; continue;
            }
            return -1;
        }
        if (code != 200) return -1;     /* code 0 = unparseable status line: not HTTP */
        break;
    }
    int body = find_body(raw, raw_len); if (body < 0) return -1;
    int blen = raw_len - body;
    if (blen <= 0) return -1;
    uint8_t *b = kmalloc(blen); if (!b) return -1;
    memcpy(b, raw + body, (size_t)blen);
    *buf = b; *len = blen; return 0;
}
