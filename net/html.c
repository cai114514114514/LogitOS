#include <stdint.h>
#include <stddef.h>
#include "url.h"

/* Minimal HTML -> readable text renderer for the Browser app:
 *  - drop <script>/<style> element contents entirely
 *  - strip all other <...> tags; map block tags to newlines
 *  - decode common entities (&amp; &lt; &gt; &quot; &#NN;)
 *  - collapse runs of whitespace
 *  - extract <a href="..."> ... </a> into a link table; insert an inline
 *    "[n]" marker and keep the anchor text in the flow
 * Output: rendered text (returns its length); links queryable via the API below. */

#define MAX_LINKS 64
#define URL_ABS_MAX 600

struct link { char url[URL_ABS_MAX]; };
static struct link links[MAX_LINKS];
static int link_n;

int  http_link_count(void);
int  http_link_url(int i, char *buf, int max);
int  http_link_count(void) { return link_n; }
int  http_link_url(int i, char *buf, int max)
{
    if (i < 0 || i >= link_n) return -1;
    int o = 0; const char *s = links[i].url;
    for (; s[o] && o < max - 1; o++) buf[o] = s[o];
    buf[o] = 0;
    return 0;
}

static int ci_eq(const char *a, const char *b, int n)   /* case-insensitive, n chars */
{
    for (int i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

/* Decode one entity starting at body[i] ('&' already seen at i-1 by caller).
 * Writes the decoded char(s) via emit; returns the index just past the ';'. */
struct emit { char *out; int o, max; };
static void put(struct emit *e, char c) { if (e->o < e->max - 1) e->out[e->o++] = c; }

int html_render(const char *body, int blen, const struct url *base,
                char *out, int outmax)
{
    struct emit e = { out, 0, outmax };
    link_n = 0;
    int last_space = 1;          /* collapse leading/space runs; start "spaced" */
    int pending_nl = 0;

    for (int i = 0; i < blen; ) {
        char c = body[i];

        if (c == '<') {
            /* tag name */
            int j = i + 1;
            int close = 0;
            if (j < blen && body[j] == '/') { close = 1; j++; }
            int ns = j;
            while (j < blen && ((body[j] >= 'a' && body[j] <= 'z') ||
                                (body[j] >= 'A' && body[j] <= 'Z') ||
                                (body[j] >= '0' && body[j] <= '9'))) j++;
            int nlen = j - ns;
            const char *name = body + ns;

            /* skip <script>/<style> ... </script>/</style> bodies entirely */
            if (!close && ((nlen == 6 && ci_eq(name, "script", 6)) ||
                           (nlen == 5 && ci_eq(name, "style", 5)))) {
                const char *want = (nlen == 6) ? "/script" : "/style";
                int wl = (nlen == 6) ? 7 : 6;
                int k = j;
                while (k < blen) {
                    if (body[k] == '<' && k + 1 + wl <= blen && ci_eq(body + k + 1, want, wl)) break;
                    k++;
                }
                /* advance to the end of the closing tag */
                while (k < blen && body[k] != '>') k++;
                i = (k < blen) ? k + 1 : blen;
                continue;
            }

            /* block-level tags -> a line break */
            if ((nlen == 1 && ci_eq(name, "p", 1)) ||
                (nlen == 2 && ci_eq(name, "br", 2)) ||
                (nlen == 3 && ci_eq(name, "div", 3)) ||
                (nlen == 2 && (ci_eq(name, "h1", 2) || ci_eq(name, "h2", 2) ||
                               ci_eq(name, "h3", 2) || ci_eq(name, "li", 2))) ||
                (nlen == 2 && ci_eq(name, "tr", 2))) {
                pending_nl = 1;
            }

            /* <a href="..."> -> record a link + inline marker */
            if (!close && nlen == 1 && ci_eq(name, "a", 1)) {
                /* scan attributes for href within this tag */
                int k = j;
                int tagend = k; while (tagend < blen && body[tagend] != '>') tagend++;
                while (k < tagend) {
                    if ((body[k] == 'h' || body[k] == 'H') && k + 4 < tagend &&
                        ci_eq(body + k, "href", 4)) {
                        int m = k + 4;
                        while (m < tagend && (body[m] == ' ' || body[m] == '=')) m++;
                        char q = 0;
                        if (m < tagend && (body[m] == '"' || body[m] == '\'')) q = body[m++];
                        char ref[URL_ABS_MAX]; int r = 0;
                        while (m < tagend && r < URL_ABS_MAX - 1) {
                            char ch = body[m];
                            if (q ? (ch == q) : (ch == ' ' || ch == '>')) break;
                            ref[r++] = ch; m++;
                        }
                        ref[r] = 0;
                        if (r > 0 && link_n < MAX_LINKS) {
                            url_resolve(base, ref, links[link_n].url, URL_ABS_MAX);
                            /* inline "[n] " marker */
                            char mk[8]; int t = 0; mk[t++] = '[';
                            int v = link_n; char d[4]; int di = 0;
                            if (!v) d[di++] = '0'; while (v) { d[di++] = (char)('0'+v%10); v/=10; }
                            while (di) mk[t++] = d[--di];
                            mk[t++] = ']'; mk[t++] = ' ';
                            for (int z = 0; z < t; z++) put(&e, mk[z]);
                            last_space = 1;
                            link_n++;
                        }
                        break;
                    }
                    k++;
                }
            }

            /* skip to end of tag */
            while (j < blen && body[j] != '>') j++;
            i = (j < blen) ? j + 1 : blen;
            continue;
        }

        if (c == '&') {                          /* entity */
            int j = i + 1, semi = -1;
            for (int k = j; k < blen && k < j + 10; k++) if (body[k] == ';') { semi = k; break; }
            char dec = 0; int handled = 0;
            if (semi > 0) {
                int el = semi - j;
                const char *en = body + j;
                if (body[j] == '#') {            /* numeric &#NN; or &#xHH; -> UTF-8 */
                    int v = 0, k = j + 1, hex = 0;
                    if (k < semi && (body[k]=='x' || body[k]=='X')) { hex = 1; k++; }
                    for (; k < semi; k++) {
                        char d = body[k];
                        if (hex) { if (d>='0'&&d<='9') v=v*16+(d-'0');
                                   else if (d>='a'&&d<='f') v=v*16+(d-'a'+10);
                                   else if (d>='A'&&d<='F') v=v*16+(d-'A'+10); }
                        else if (d>='0'&&d<='9') v = v*10 + (d-'0');
                    }
                    /* encode v as UTF-8 */
                    if (v < 0x80) put(&e, (char)v);
                    else if (v < 0x800) { put(&e,(char)(0xC0|(v>>6))); put(&e,(char)(0x80|(v&0x3F))); }
                    else if (v < 0x10000) { put(&e,(char)(0xE0|(v>>12))); put(&e,(char)(0x80|((v>>6)&0x3F))); put(&e,(char)(0x80|(v&0x3F))); }
                    else { put(&e,(char)(0xF0|(v>>18))); put(&e,(char)(0x80|((v>>12)&0x3F))); put(&e,(char)(0x80|((v>>6)&0x3F))); put(&e,(char)(0x80|(v&0x3F))); }
                    last_space = 0; i = semi + 1; continue;
                } else if (el == 3 && ci_eq(en, "amp", 3)) { dec = '&'; handled = 1; }
                else if (el == 2 && ci_eq(en, "lt", 2))    { dec = '<'; handled = 1; }
                else if (el == 2 && ci_eq(en, "gt", 2))    { dec = '>'; handled = 1; }
                else if (el == 4 && ci_eq(en, "quot", 4))  { dec = '"'; handled = 1; }
                else if (el == 4 && ci_eq(en, "nbsp", 4))  { dec = ' '; handled = 1; }
                else if (el == 5 && ci_eq(en, "apos", 5))  { dec = '\''; handled = 1; }
            }
            if (handled) {
                if (dec == ' ') { if (!last_space) { put(&e, ' '); last_space = 1; } }
                else { put(&e, dec); last_space = 0; }
                i = semi + 1;
                continue;
            }
            /* not a known entity: emit '&' literally */
        }

        /* ordinary text: collapse whitespace, honor pending newline */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (pending_nl) { put(&e, '\n'); pending_nl = 0; last_space = 1; }
            else if (!last_space) { put(&e, ' '); last_space = 1; }
            i++;
            continue;
        }
        if (pending_nl) { put(&e, '\n'); pending_nl = 0; }
        put(&e, c);
        last_space = 0;
        i++;
    }
    out[e.o < outmax ? e.o : outmax - 1] = 0;
    return e.o;
}
