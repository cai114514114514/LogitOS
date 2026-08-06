/* css_extra.c -- capture properties our vendored LibCSS doesn't know about.
 * Currently: border-radius (px + %), and the "visually hidden" pattern
 * (clip-path:inset(50%) / clip:rect(0,0,0,0)) which real browsers lift out of
 * flow via position:absolute -- we force display:none instead. The author
 * sheet is scanned for simple selectors (tag, .class, #id, tag.class, comma
 * lists; descendant selectors match on their last compound) and inline style=
 * attributes, and matching nodes' cstyle is patched after css_apply. */
#include <string.h>
#include "css.h"
#include "dom.h"

static int spc(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/* Parse a border-radius value list: first length wins. "6px" -> 6, "50%" -> pct. */
static int parse_radius(const char *v, int len, int *px, int *pct)
{
    int i = 0, n = 0, is_pct = 0;
    while (i < len && spc(v[i])) i++;
    for (; i < len && v[i] >= '0' && v[i] <= '9'; i++) {
        if (n > 100000) break;
        n = n * 10 + (v[i] - '0');
    }
    if (i < len && v[i] == '%') is_pct = 1;
    if (n <= 0) return -1;
    if (n > 512) n = 512;
    if (is_pct) { if (n > 50) n = 50; *pct = n; }
    else *px = n;
    return 0;
}

/* 1 if the declarations block [d,dlen) sets border-radius; fills px/pct. */
static int decls_radius(const char *d, int dlen, int *px, int *pct)
{
    const char *key = "border-radius";
    int kl = 13, found = 0;
    for (int i = 0; i + kl < dlen; i++) {
        if (i > 0 && !spc(d[i-1]) && d[i-1] != ';' && d[i-1] != '{') continue;
        if (memcmp(d + i, key, kl)) continue;
        int j = i + kl;
        while (j < dlen && spc(d[j])) j++;
        if (j >= dlen || d[j] != ':') continue;
        j++;
        int vs = j;
        while (j < dlen && d[j] != ';' && d[j] != '}') j++;
        if (parse_radius(d + vs, j - vs, px, pct) == 0) found = 1;
        i = j;
    }
    return found;
}

/* 1 if the declarations block uses the classic "visually hidden" pattern
 * (clip-path:inset(50%) or clip:rect(0...)). Real browsers pull those out of
 * flow with position:absolute; we treat them as display:none. */
static int decls_vish(const char *d, int dlen)
{
    static const char *pats[] = { "clip-path:inset(50%", "clip:rect(0,0,0,0)",
                                  "clip:rect(0 0 0 0)", "clip:rect(1px" };
    for (int p = 0; p < 4; p++) {
        const char *pat = pats[p];
        int pl = (int)strlen(pat);
        for (int i = 0; i + pl <= dlen; i++)
            if (!memcmp(d + i, pat, pl)) return 1;
    }
    return 0;
}

/* Match ONE compound selector (no combinators): [tag][#id][.cls][.cls]... */
static int match_compound(struct node *n, const char *s, int len)
{
    int i = 0, classes = 0, idok = 1, tagok = 1;
    char cls[2][64]; int ncls = 0;
    char id[64]; int has_id = 0;
    char tag[32]; int has_tag = 0;
    while (i < len && ncls < 2) {
        if (s[i] == '.') {
            i++; int o = 0;
            while (i < len && !spc(s[i]) && s[i] != '.' && s[i] != '#' && o < 63) cls[ncls][o++] = s[i++];
            cls[ncls][o] = 0; if (!o) return 0; ncls++;
        } else if (s[i] == '#') {
            i++; int o = 0;
            while (i < len && !spc(s[i]) && s[i] != '.' && s[i] != '#' && o < 63) id[o++] = s[i++];
            id[o] = 0; if (!o) return 0; has_id = 1;
        } else if (!spc(s[i])) {
            int o = 0;
            while (i < len && !spc(s[i]) && s[i] != '.' && s[i] != '#' && o < 31) tag[o++] = s[i++];
            tag[o] = 0; if (!o) return 0; has_tag = 1;
        } else break;
    }
    (void)classes;
    if (has_tag) {
        int k = 0; for (; tag[k]; k++) if (n->tag[k] != tag[k]) { tagok = 0; break; }
        if (tagok && n->tag[k]) tagok = 0;
        if (!tagok) return 0;
    }
    if (has_id) {
        const char *nid = dom_attr(n, "id");
        if (!nid || strcmp(nid, id)) idok = 0;
        if (!idok) return 0;
    }
    for (int c = 0; c < ncls; c++) {
        const char *nc = dom_attr(n, "class");
        if (!nc) return 0;
        /* class attr is a space-separated list; require whole-token match */
        int ok = 0, cl = (int)strlen(cls[c]);
        for (const char *p = nc; *p; p++) {
            if ((p == nc || spc(p[-1])) && !strncmp(p, cls[c], cl) && (!p[cl] || spc(p[cl]))) { ok = 1; break; }
        }
        if (!ok) return 0;
    }
    return 1;
}

/* 1 if any comma-separated selector matches; for descendant selectors only the
 * last compound is matched (documented simplification). */
static int match_selector(struct node *n, const char *s, int len)
{
    int i = 0;
    while (i < len) {
        while (i < len && (spc(s[i]) || s[i] == ',')) i++;
        int start = i;
        while (i < len && s[i] != ',') i++;
        int end = i; while (end > start && spc(s[end-1])) end--;
        /* last compound of a descendant chain */
        int c = end - 1;
        while (c > start && !spc(s[c])) c--;
        int cs = spc(s[c]) ? c + 1 : start;
        /* strip pseudo-classes/elements (:hover etc.) -- we match the base */
        int ce = end;
        for (int k = cs; k < ce; k++) if (s[k] == ':') { ce = k; break; }
        if (ce > cs && match_compound(n, s + cs, ce - cs)) return 1;
    }
    return 0;
}

/* mode 0: patch border-radius (px/pct); mode 1: force display:none */
static void apply_patch(struct node *n, int px, int pct, int mode)
{
    if (!n->style) return;
    struct cstyle *st = n->style;
    if (mode == 1) { st->display = DISP_NONE; return; }
    if (pct > 0) { st->radius_pct = pct; st->radius = 0; }
    else { st->radius = px; st->radius_pct = 0; }
}

static void walk(struct node *n, const char *sel, int slen, int px, int pct, int mode)
{
    if (n->type == N_ELEM && match_selector(n, sel, slen)) apply_patch(n, px, pct, mode);
    for (struct node *c = n->first_child; c; c = c->next) walk(c, sel, slen, px, pct, mode);
}

/* inline style="border-radius:..." / visually-hidden clip on each element */
static void walk_inline(struct node *n)
{
    if (n->type == N_ELEM && n->style) {
        const char *st = dom_attr(n, "style");
        if (st) {
            int slen = (int)strlen(st);
            if (decls_vish(st, slen)) apply_patch(n, 0, 0, 1);
            else {
                int px = 0, pct = 0;
                if (decls_radius(st, slen, &px, &pct)) apply_patch(n, px, pct, 0);
            }
        }
    }
    for (struct node *c = n->first_child; c; c = c->next) walk_inline(c);
}

void css_extra_apply(struct node *root, const char *css, int len)
{
    if (!root || !css || len <= 0) { if (root) walk_inline(root); return; }
    int i = 0;
    while (i < len) {
        /* selector up to '{' (skip @-blocks naively: their inner rules still
         * get matched, which for our simple media usage is fine). Stray '}'
         * from closed @-blocks must be skipped too, else it poisons the next
         * selector as a bogus tag name. */
        while (i < len && (spc(css[i]) || css[i] == '}')) i++;
        if (i >= len) break;
        if (css[i] == '@') {                       /* @media ... { -> scan inside */
            while (i < len && css[i] != '{') i++;
            if (i < len) i++;
            continue;
        }
        int s = i;
        while (i < len && css[i] != '{') i++;
        if (i >= len) break;
        int slen = i - s;
        i++;
        int d = i, depth = 1;
        while (i < len && depth) { if (css[i] == '{') depth++; else if (css[i] == '}') depth--; i++; }
        int dlen = i - 1 - d;
        int px = 0, pct = 0;
        if (dlen > 0 && decls_vish(css + d, dlen))
            walk(root, css + s, slen, 0, 0, 1);
        else if (dlen > 0 && decls_radius(css + d, dlen, &px, &pct))
            walk(root, css + s, slen, px, pct, 0);
    }
    walk_inline(root);
}
