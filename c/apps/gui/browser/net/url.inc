/* Shared transport URL resolution. The loader fake previously only joined
 * paths, so import-map tests fetched /./lib.js while the guest fetched /lib.js.
 * Keep the SAME production normalizer in both consumers; a second toy parser
 * would let the apparatus invent missing modules and hide package escapes.
 * This remains the HTTP transport parser, not a replacement for js_url.c. */
#include "url.h"
/* RFC 3986 5.2.4 remove_dot_segments, applied to the path of an already
 * absolute URL. url.c's url_resolve concatenates instead, so "./chunk.js"
 * against ".../assets/index.js" comes out as ".../assets/./chunk.js" -- which
 * some servers normalise and some 404 on. A module graph cannot depend on which
 * kind it is talking to. */
static void norm_dots(char *url)
{
    /* find the start of the path: after "scheme://host[:port]" */
    char *p = url;
    while (*p && *p != ':') p++;
    if (p[0] != ':' || p[1] != '/' || p[2] != '/') return;
    p += 3;
    while (*p && *p != '/') p++;
    if (!*p) return;
    char *path = p;

    /* Dots inside a query or fragment are data, not path segments. */
    char *q = path;
    while (*q && *q != '?' && *q != '#') q++;
    int plen = (int)(q - path);
    if (plen <= 0 || plen >= URL_PATH_MAX) return;

    char in[URL_PATH_MAX], out[URL_PATH_MAX];
    memcpy(in, path, (size_t)plen); in[plen] = 0;

    int o = 0, i = 0;
    while (in[i]) {
        if (in[i] == '/' && in[i + 1] == '.' && (in[i + 2] == '/' || in[i + 2] == 0)) {
            i += 2;
            if (!in[i] && o < URL_PATH_MAX - 1) out[o++] = '/';
            continue;
        }
        if (in[i] == '/' && in[i + 1] == '.' && in[i + 2] == '.' &&
            (in[i + 3] == '/' || in[i + 3] == 0)) {
            i += 3;
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;                       /* drop the separator too */
            if (!in[i] && o < URL_PATH_MAX - 1) out[o++] = '/';
            continue;
        }
        if (o < URL_PATH_MAX - 1) out[o++] = in[i];
        i++;
        while (in[i] && in[i] != '/') { if (o < URL_PATH_MAX - 1) out[o++] = in[i]; i++; }
    }
    if (o == 0) out[o++] = '/';

    /* Normalisation only ever shortens, so the result fits where the old path
     * was; move the query/fragment down first, then lay the path over it. */
    int tl = 0; while (q[tl]) tl++;
    memmove(path + o, q, (size_t)tl + 1);
    memcpy(path, out, (size_t)o);
}

static int browser_url_resolve(const char *base, const char *ref, char *out, int max)
{
    if (!ref || !out || max < 2) return -1;
    const char *b = base && base[0] ? base : "";
    struct url bu;
    if (url_parse(b, &bu) != 0) {
        /* base is not an absolute http(s) URL (about:blank, or an inline
         * script's synthetic name): the reference has to stand alone. */
        struct url t;
        if (url_parse(ref, &t) != 0) return -1;
        int i = 0; while (ref[i] && i < max - 1) { out[i] = ref[i]; i++; }
        out[i] = 0;
        return 0;
    }
    if (url_resolve(&bu, ref, out, max) != 0) return -1;
    struct url chk;
    if (url_parse(out, &chk) != 0) return -1;
    norm_dots(out);
    return 0;
}

