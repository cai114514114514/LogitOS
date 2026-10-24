#include <stdint.h>
#include <stddef.h>
#include "url.h"

/* Local helpers stay freestanding. This is the transport's bounded HTTP
 * splitter, not the JS WHATWG URL implementation (no new URL grammar here). */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int sncmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) { if (a[i] != b[i]) return 1; if (!a[i]) return 0; }
    return 0;
}
static int bounded_len(const char *s, int max)
{
    int n = 0;
    while (n < max && s[n]) n++;
    return n < max ? n : -1;
}

static int copy_text(char *d, const char *s, int max, int wire)
{
    int i = 0;
    while (i < max - 1 && s[i] && !(wire && s[i] == '#')) {
        d[i] = s[i]; i++;
    }
    d[i] = 0;
#ifndef HTTP_URL_TRUNCATE
    if (s[i] && !(wire && s[i] == '#')) return -1;
#endif
    return 0;
}

static int append_text(char *out, int *used, int max, const char *text)
{
    if (copy_text(out + *used, text, max - *used, 0) != 0) return -1;
    *used += slen(out + *used);
    return 0;
}

/* Original behaviour: every bounded copy reported success after clipping.
 * A GET with a ~600-byte query fit the browser's 700-byte temporary URL, then
 * became a different request in path[512]. HTTP_URL_TRUNCATE restores only
 * that false capacity verdict for the host and actual guest negative controls.
 * Parse publishes its record only when complete; callers must use the return
 * value rather than an old or partially parsed endpoint after a refusal. */
static int parse_endpoint(const char *s, int https, struct url *out)
{
    struct url value;
    value.https = https;
    value.port = https ? 443 : 80;

    /* A fragment is client navigation state even when there is no slash. It
     * must not become part of the hostname or the HTTP request target. */
    int h = 0;
    while (*s && *s != '/' && *s != ':' && *s != '#') {
        if (h >= URL_HOST_MAX - 1) {
#ifdef HTTP_URL_TRUNCATE
            break;
#else
            return -1;
#endif
        }
        value.host[h++] = *s++;
    }
    value.host[h] = 0;
    if (h == 0) return -1;

    if (*s == ':') {
        s++;
        int port = 0, any = 0;
        /* Bail out past 65535 before the accumulation can overflow int; the
         * range check below then rejects it. */
        while (*s >= '0' && *s <= '9') {
            port = port * 10 + (*s++ - '0');
            any = 1;
            if (port > 65535) break;
        }
        if (!any || port == 0 || port > 65535) return -1;
        value.port = (uint16_t)port;
    }

    if (*s == '/') {
        if (copy_text(value.path, s, URL_PATH_MAX, 1) != 0) return -1;
    } else {
        value.path[0] = '/'; value.path[1] = 0;
    }
    *out = value;
    return 0;
}

int url_parse(const char *s, struct url *out)
{
    if (!s || !out) return -1;
    if (sncmp(s, "http://", 7) == 0) return parse_endpoint(s + 7, 0, out);
    if (sncmp(s, "https://", 8) == 0) return parse_endpoint(s + 8, 1, out);
    return -1;                             /* require an explicit scheme */
}

/* Render an absolute URL for `u` (scheme://host[:port]path) into out. */
static int url_unparse(const struct url *u, char *out, int max)
{
    if (bounded_len(u->host, URL_HOST_MAX) < 0 ||
        bounded_len(u->path, URL_PATH_MAX) < 0) return -1;
    int o = 0;
    const char *sch = u->https ? "https://" : "http://";
    if (append_text(out, &o, max, sch) != 0 ||
        append_text(out, &o, max, u->host) != 0) return -1;
    if (!(u->https && u->port == 443) && !(!u->https && u->port == 80)) {
        char digits[5], port[7]; int i = 0, p = 0; uint16_t v = u->port;
        if (!v) digits[i++] = '0';
        while (v) { digits[i++] = (char)('0' + v % 10); v /= 10; }
        port[p++] = ':';
        while (i) port[p++] = digits[--i];
        port[p] = 0;
        if (append_text(out, &o, max, port) != 0) return -1;
    }
    return append_text(out, &o, max, u->path);
}

int url_resolve(const struct url *base, const char *ref, char *out, int max)
{
    if (!base || !ref || !out || max < 1) return -1;
    /* Validate transport capacity but copy the original absolute reference.
     * Rendering the parsed endpoint would lose #fragment, an explicit default
     * port, and other spelling the navigation consumer is entitled to retain. */
    if (sncmp(ref, "http://", 7) == 0 || sncmp(ref, "https://", 8) == 0) {
        struct url parsed;
        if (url_parse(ref, &parsed) != 0) return -1;
        return copy_text(out, ref, max, 0);
    }

    /* Protocol-relative "//host/path": inherit the base scheme. */
    if (ref[0] == '/' && ref[1] == '/') {
        struct url parsed;
        if (parse_endpoint(ref + 2, base->https, &parsed) != 0) return -1;
        const char *sch = base->https ? "https:" : "http:";
        int o = 0;
        if (append_text(out, &o, max, sch) != 0) return -1;
        return append_text(out, &o, max, ref);
    }

    struct url u = *base;
    if (ref[0] == '/') {                    /* root-relative: replace whole path */
        if (copy_text(u.path, ref, URL_PATH_MAX, 0) != 0) return -1;
    } else {                                /* relative: replace after last '/' */
        char dir[URL_PATH_MAX];
        int n = bounded_len(base->path, URL_PATH_MAX), cut = 0;
        if (n < 0) return -1;
        for (int i = 0; i < n; i++) if (base->path[i] == '/') cut = i + 1;
        for (int i = 0; i < cut; i++) dir[i] = base->path[i];
        dir[cut] = 0;
        if (append_text(dir, &cut, URL_PATH_MAX, ref) != 0) return -1;
        if (copy_text(u.path, dir, URL_PATH_MAX, 0) != 0) return -1;
    }
    /* Relative references keep their raw fragment until rendering; url_parse
     * strips it only when the resulting address is used for HTTP transport. */
    return url_unparse(&u, out, max);
}
