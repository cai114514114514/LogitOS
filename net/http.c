#include <stdint.h>
#include <stddef.h>
#include "http.h"
#include "url.h"
#include "tcp.h"
#include "dns.h"
#include "net.h"
#include "pit.h"

void *memcpy(void *, const void *, size_t);

/* net/html.c (L2); weak so L1 builds before it exists. */
int html_render(const char *body, int blen, const struct url *base,
                char *out, int outmax) __attribute__((weak));

#define RAW_MAX  65536
#define TXT_MAX  32768

static char raw[RAW_MAX];
static char text[TXT_MAX];
static int  raw_len, text_len;
static int  status = HTTP_IDLE;
static struct url cur;                      /* the page currently loaded (link base) */

int  http_status(void)     { return status; }
/* http_link_count / http_link_url are provided by net/html.c (the renderer). */

int http_read(int off, char *buf, int max)
{
    if (status != HTTP_DONE || off >= text_len) return 0;
    int n = text_len - off;
    if (n > max) n = max;
    memcpy(buf, text + off, (size_t)n);
    return n;
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
    const char *tail = "\r\nConnection: close\r\nUser-Agent: Aqua/1.0\r\n\r\n";
    for (const char *p = tail; *p && o < max; p++) req[o++] = *p;
    return o;
}

/* Find the body (after the first CRLFCRLF), return offset or -1. */
static int find_body(const char *buf, int len)
{
    for (int i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return i + 4;
    return -1;
}

int http_get(const char *url)
{
    status = HTTP_BUSY;
    raw_len = text_len = 0;

    if (url_parse(url, &cur) != 0) { status = HTTP_ERR_URL; return HTTP_ERR_URL; }
    if (cur.https) { status = HTTP_ERR_URL; return HTTP_ERR_URL; }   /* TLS is M12 */

    uint32_t ip = dns_resolve(cur.host);
    if (!ip) { status = HTTP_ERR_DNS; return HTTP_ERR_DNS; }

    int id = tcp_connect(ip, cur.port);
    if (id < 0) { status = HTTP_ERR_CONN; return HTTP_ERR_CONN; }

    char req[URL_PATH_MAX + URL_HOST_MAX + 64];
    int rl = build_request(&cur, req, (int)sizeof req);
    tcp_send(id, req, rl);

    /* Receive until the peer closes (Connection: close) or we time out. */
    uint64_t start = timer_ticks();
    while (timer_ticks() - start < 800) {       /* ~8 s */
        net_poll();
        int n = tcp_recv(id, raw + raw_len, RAW_MAX - raw_len);
        if (n > 0) {
            raw_len += n;
            start = timer_ticks();              /* reset idle timer on progress */
            if (raw_len >= RAW_MAX) break;
        } else if (n < 0) {
            break;                              /* closed + drained */
        }
        for (volatile int d = 0; d < 150000; d++) ;
    }
    tcp_close(id);

    if (raw_len == 0) { status = HTTP_ERR_CONN; return HTTP_ERR_CONN; }

    int body = find_body(raw, raw_len);
    if (body < 0) body = 0;
    int blen = raw_len - body;

    if (html_render)
        text_len = html_render(raw + body, blen, &cur, text, TXT_MAX);
    else {                                       /* L1: no renderer yet -- raw body */
        text_len = blen > TXT_MAX ? TXT_MAX : blen;
        memcpy(text, raw + body, (size_t)text_len);
    }
    status = HTTP_DONE;
    return 0;
}

void http_poll(void) { }                         /* fetch is synchronous in http_get */
