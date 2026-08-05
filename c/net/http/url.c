#include <stdint.h>
#include <stddef.h>
#include "url.h"

/* local string helpers (freestanding; no libc str*) */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int sncmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) { if (a[i] != b[i]) return 1; if (!a[i]) return 0; }
    return 0;
}
static void scopy(char *d, const char *s, int max)
{
    int i = 0; for (; i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0;
}

int url_parse(const char *s, struct url *out)
{
    int https = 0;
    if (sncmp(s, "http://", 7) == 0) s += 7;
    else if (sncmp(s, "https://", 8) == 0) { https = 1; s += 8; }
    else return -1;                         /* require an explicit scheme */

    out->https = https;
    out->port = https ? 443 : 80;

    /* host[:port] up to the first '/' */
    int h = 0;
    while (*s && *s != '/' && *s != ':' && h < URL_HOST_MAX - 1)
        out->host[h++] = *s++;
    out->host[h] = 0;
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
        out->port = (uint16_t)port;
    }

    if (*s == '/') scopy(out->path, s, URL_PATH_MAX);
    else { out->path[0] = '/'; out->path[1] = 0; }
    return 0;
}

/* Render an absolute URL for `u` (scheme://host[:port]path) into out. */
static void url_unparse(const struct url *u, char *out, int max)
{
    int o = 0;
    const char *sch = u->https ? "https://" : "http://";
    for (const char *p = sch; *p && o < max - 1; p++) out[o++] = *p;
    for (const char *p = u->host; *p && o < max - 1; p++) out[o++] = *p;
    if (!(u->https && u->port == 443) && !(!u->https && u->port == 80)) {
        if (o < max - 1) out[o++] = ':';
        char t[6]; int i = 0; uint16_t v = u->port;
        if (!v) t[i++] = '0';
        while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
        while (i && o < max - 1) out[o++] = t[--i];
    }
    for (const char *p = u->path; *p && o < max - 1; p++) out[o++] = *p;
    out[o] = 0;
}

int url_resolve(const struct url *base, const char *ref, char *out, int max)
{
    /* Absolute reference: copy through (must be a scheme we recognise). */
    if (sncmp(ref, "http://", 7) == 0 || sncmp(ref, "https://", 8) == 0) {
        scopy(out, ref, max);
        return 0;
    }

    /* Protocol-relative "//host/path": inherit the base scheme. */
    if (ref[0] == '/' && ref[1] == '/') {
        const char *sch = base->https ? "https:" : "http:";
        int o = 0;
        for (const char *p = sch; *p && o < max - 1; p++) out[o++] = *p;
        for (const char *p = ref; *p && o < max - 1; p++) out[o++] = *p;
        out[o] = 0;
        return 0;
    }

    struct url u = *base;
    if (ref[0] == '/') {                    /* root-relative: replace whole path */
        scopy(u.path, ref, URL_PATH_MAX);
    } else {                                /* relative: replace after last '/' */
        char dir[URL_PATH_MAX];
        int n = slen(base->path), cut = 0;
        for (int i = 0; i < n; i++) if (base->path[i] == '/') cut = i + 1;
        for (int i = 0; i < cut && i < URL_PATH_MAX - 1; i++) dir[i] = base->path[i];
        dir[cut < URL_PATH_MAX ? cut : URL_PATH_MAX - 1] = 0;
        int p = slen(dir);
        for (int i = 0; ref[i] && p < URL_PATH_MAX - 1; i++) dir[p++] = ref[i];
        dir[p] = 0;
        scopy(u.path, dir, URL_PATH_MAX);
    }
    url_unparse(&u, out, max);
    return 0;
}
