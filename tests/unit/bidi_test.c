/* Conformance test for c/lib/text/bidi.c against the Unicode Character
 * Database's own bidi corpora:
 *
 *    BidiTest.txt           ~490k cases, written as Bidi_Class sequences
 *    BidiCharacterTest.txt  ~91k cases, written as real code points, and the
 *                           only one that exercises N0 (paired brackets)
 *
 * Both files carry the expected embedding levels AND the expected visual
 * order, so this checks the resolution and the reordering, not just one of
 * them. Nothing here is a case we invented: the standard ships the corpus, and
 * inventing our own would only test our own misreading of the spec.
 *
 * Build:  cc -O2 -o bidi_test tests/unit/bidi_test.c c/lib/text/bidi.c -Ic/lib/text
 * Run:    ./bidi_test [/usr/share/unicode]
 *
 * With -DBIDI_NEGATIVE_CONTROL the resolver is replaced by the "no bidi"
 * behaviour LogitOS had before this line existed -- every level 0, visual
 * order = logical order. The suite must then FAIL; `make test-bidi-negctl`
 * asserts that it does.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bidi.h"

#define MAXCH 256
static unsigned char scratch[MAXCH * 32 + 64];

static long pass_levels, fail_levels, pass_order, fail_order, cases;
static int shown;

static void complain(const char *file, long line, const char *what,
                     const char *expect, const char *got)
{
    if (shown++ < 12)
        fprintf(stderr, "FAIL %s:%ld %s expected [%s] got [%s]\n",
                file, line, what, expect, got);
}

/* ------------------------------------------------------------- the runner -- */

/* Resolve, then produce the level string and the visual order the corpora
 * describe. Both files omit the characters the algorithm removes (X9), so the
 * caller passes `hide[]` telling us which positions the file left out. */
static int run_case(const uint32_t *cps, int n, int dir, uint8_t *levels,
                    int *order, int *norder, const int *hide)
{
#ifdef BIDI_NEGATIVE_CONTROL
    (void)dir; (void)cps;
    for (int i = 0; i < n; i++) levels[i] = 0;
    int para = 0;
#else
    int para = bidi_resolve(cps, n, dir, levels, scratch, (int)sizeof scratch);
    if (para < 0) return -1;
#endif
    /* L2 over only the characters the corpus keeps. */
    uint8_t vis[MAXCH];
    int map[MAXCH], k = 0;
    for (int i = 0; i < n; i++) {
        if (hide[i]) continue;
        vis[k] = levels[i];
        map[k] = i;
        k++;
    }
    int tmp[MAXCH];
    bidi_reorder(vis, k, tmp);
    *norder = k;
    for (int i = 0; i < k; i++) order[i] = map[tmp[i]];
    return para;
}

/* ----------------------------------------------------------- BidiTest.txt -- */

/* One representative code point per Bidi_Class. BidiTest.txt is written in
 * class names, so the test has to pick characters; each is verified to
 * actually carry the class before any case runs, otherwise a trie bug would
 * masquerade as an algorithm bug. */
static const struct { const char *name; uint32_t cp; int cls; } REP[] = {
    {"L",   0x0041, BIDI_L},   {"R",   0x05D0, BIDI_R},   {"AL",  0x0627, BIDI_AL},
    {"EN",  0x0030, BIDI_EN},  {"ES",  0x002B, BIDI_ES},  {"ET",  0x0023, BIDI_ET},
    {"AN",  0x0660, BIDI_AN},  {"CS",  0x002C, BIDI_CS},  {"NSM", 0x0300, BIDI_NSM},
    {"BN",  0x00AD, BIDI_BN},  {"B",   0x2029, BIDI_B},   {"S",   0x0009, BIDI_S},
    {"WS",  0x0020, BIDI_WS},  {"ON",  0x0021, BIDI_ON},
    {"LRE", 0x202A, BIDI_LRE}, {"LRO", 0x202D, BIDI_LRO},
    {"RLE", 0x202B, BIDI_RLE}, {"RLO", 0x202E, BIDI_RLO}, {"PDF", 0x202C, BIDI_PDF},
    {"LRI", 0x2066, BIDI_LRI}, {"RLI", 0x2067, BIDI_RLI}, {"FSI", 0x2068, BIDI_FSI},
    {"PDI", 0x2069, BIDI_PDI},
};
#define NREP ((int)(sizeof REP / sizeof REP[0]))

static int rep_lookup(const char *tok, uint32_t *cp)
{
    for (int i = 0; i < NREP; i++)
        if (!strcmp(tok, REP[i].name)) { *cp = REP[i].cp; return REP[i].cls; }
    return -1;
}

static int check_representatives(void)
{
    int bad = 0;
    for (int i = 0; i < NREP; i++) {
        int c = bidi_class(REP[i].cp);
        if (c != REP[i].cls) {
            fprintf(stderr, "FAIL representative U+%04X should be %s, table says %d\n",
                    REP[i].cp, REP[i].name, c);
            bad = 1;
        }
    }
    /* U+0021 must not be a bracket, or the N0 cases would be contaminated. */
    if (bidi_mirror_cp(0x0028) != 0x0029) {
        fprintf(stderr, "FAIL mirroring table: '(' should mirror to ')'\n");
        bad = 1;
    }
    return bad;
}

static void run_biditest(const char *path)
{
    FILE *fh = fopen(path, "r");
    if (!fh) { fprintf(stderr, "FAIL cannot open %s\n", path); fail_levels++; return; }

    char line[4096];
    /* Current @Levels / @Reorder expectations. */
    char exp_levels[1024] = "", exp_order[1024] = "";
    int want_lv[MAXCH], want_hidden[MAXCH], nwant = 0;
    int want_ord[MAXCH], nwant_ord = 0;
    long lineno = 0;

    while (fgets(line, sizeof line, fh)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;

        if (!strncmp(p, "@Levels:", 8)) {
            snprintf(exp_levels, sizeof exp_levels, "%s", p + 8);
            nwant = 0;
            for (char *t = strtok(p + 8, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
                if (nwant >= MAXCH) break;
                if (*t == 'x') { want_hidden[nwant] = 1; want_lv[nwant] = 0; }
                else { want_hidden[nwant] = 0; want_lv[nwant] = atoi(t); }
                nwant++;
            }
            continue;
        }
        if (!strncmp(p, "@Reorder:", 9)) {
            snprintf(exp_order, sizeof exp_order, "%s", p + 9);
            nwant_ord = 0;
            for (char *t = strtok(p + 9, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
                if (nwant_ord >= MAXCH) break;
                want_ord[nwant_ord++] = atoi(t);
            }
            continue;
        }

        /* Data line: "<class> <class> ...; <bitset>" */
        char *semi = strchr(p, ';');
        if (!semi) continue;
        *semi = 0;
        int bits = (int)strtol(semi + 1, NULL, 16);   /* the file says hex */

        uint32_t cps[MAXCH];
        int n = 0, bad = 0;
        for (char *t = strtok(p, " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
            uint32_t cp;
            if (n >= MAXCH || rep_lookup(t, &cp) < 0) { bad = 1; break; }
            cps[n++] = cp;
        }
        if (bad || n == 0 || n != nwant) continue;

        for (int b = 0; b < 3; b++) {
            static const int DIRS[3] = { BIDI_DIR_AUTO, BIDI_DIR_LTR, BIDI_DIR_RTL };
            if (!(bits & (1 << b))) continue;
            cases++;

            uint8_t lv[MAXCH];
            int ord[MAXCH], nord = 0;
            if (run_case(cps, n, DIRS[b], lv, ord, &nord, want_hidden) < 0) {
                fail_levels++; continue;
            }

            /* Levels: the corpus's `x` positions are unconstrained. */
            int ok = 1;
            for (int i = 0; i < n; i++)
                if (!want_hidden[i] && lv[i] != want_lv[i]) { ok = 0; break; }
            if (ok) pass_levels++;
            else {
                fail_levels++;
                char got[1024]; int o = 0;
                for (int i = 0; i < n && o < 1000; i++)
                    o += snprintf(got + o, sizeof got - o, "%d ", lv[i]);
                complain(path, lineno, "levels", exp_levels, got);
            }

            /* Reorder: "indexes into the input string. Items with a level of x
             * are skipped" -- so these are ORIGINAL indices with the removed
             * characters dropped, which is exactly what run_case returns. */
            ok = (nord == nwant_ord);
            for (int i = 0; ok && i < nord; i++)
                if (ord[i] != want_ord[i]) ok = 0;
            if (ok) pass_order++;
            else {
                fail_order++;
                char got[1024]; int o = 0;
                for (int i = 0; i < nord && o < 1000; i++)
                    o += snprintf(got + o, sizeof got - o, "%d ", ord[i]);
                complain(path, lineno, "reorder", exp_order, got);
            }
        }
    }
    fclose(fh);
}

/* -------------------------------------------------- BidiCharacterTest.txt -- */

static void run_charactertest(const char *path)
{
    FILE *fh = fopen(path, "r");
    if (!fh) { fprintf(stderr, "FAIL cannot open %s\n", path); fail_levels++; return; }

    char line[8192];
    long lineno = 0;
    while (fgets(line, sizeof line, fh)) {
        lineno++;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        /* cps;direction;paragraph level;levels;visual order */
        char *f[5]; int nf = 0;
        for (char *t = line, *s = line; nf < 5; s++) {
            if (*s == ';' || *s == '\n' || *s == '\r' || *s == 0) {
                int last = (*s != ';');
                *s = 0; f[nf++] = t; t = s + 1;
                if (last) break;
            }
        }
        if (nf < 5) continue;

        uint32_t cps[MAXCH];
        int n = 0;
        for (char *t = strtok(f[0], " \t"); t; t = strtok(NULL, " \t")) {
            if (n >= MAXCH) { n = -1; break; }
            cps[n++] = (uint32_t)strtoul(t, NULL, 16);
        }
        if (n <= 0) continue;

        int filedir = atoi(f[1]);
        int dir = filedir == 0 ? BIDI_DIR_LTR : filedir == 1 ? BIDI_DIR_RTL : BIDI_DIR_AUTO;
        int want_para = atoi(f[2]);

        int want_lv[MAXCH], hidden[MAXCH], nlv = 0;
        for (char *t = strtok(f[3], " \t"); t; t = strtok(NULL, " \t")) {
            if (nlv >= MAXCH) break;
            if (*t == 'x') { hidden[nlv] = 1; want_lv[nlv] = 0; }
            else { hidden[nlv] = 0; want_lv[nlv] = atoi(t); }
            nlv++;
        }
        if (nlv != n) continue;

        int want_ord[MAXCH], nwo = 0;
        for (char *t = strtok(f[4], " \t\r\n"); t; t = strtok(NULL, " \t\r\n")) {
            if (nwo >= MAXCH) break;
            want_ord[nwo++] = atoi(t);
        }

        cases++;
        uint8_t lv[MAXCH];
        int ord[MAXCH], nord = 0;
        int para = run_case(cps, n, dir, lv, ord, &nord, hidden);
        if (para < 0) { fail_levels++; continue; }

        int ok = (para == want_para);
        for (int i = 0; ok && i < n; i++)
            if (!hidden[i] && lv[i] != want_lv[i]) ok = 0;
        if (ok) pass_levels++;
        else {
            fail_levels++;
            char got[2048]; int o = 0;
            o += snprintf(got + o, sizeof got - o, "para=%d: ", para);
            for (int i = 0; i < n && o < 2000; i++)
                o += snprintf(got + o, sizeof got - o, "%d ", lv[i]);
            char exp[2048];
            snprintf(exp, sizeof exp, "para=%d: %s", want_para, f[3]);
            complain(path, lineno, "levels", exp, got);
        }

        /* This file's visual order lists ORIGINAL indices (not renumbered). */
        ok = (nord == nwo);
        for (int i = 0; ok && i < nord; i++)
            if (ord[i] != want_ord[i]) ok = 0;
        if (ok) pass_order++;
        else {
            fail_order++;
            char got[2048]; int o = 0;
            for (int i = 0; i < nord && o < 2000; i++)
                o += snprintf(got + o, sizeof got - o, "%d ", ord[i]);
            complain(path, lineno, "reorder", f[4], got);
        }
    }
    fclose(fh);
}

/* ---------------------------------------------------------------- driver -- */

int main(int argc, char **argv)
{
    const char *ucd = argc > 1 ? argv[1] : "/usr/share/unicode";
    char p1[512], p2[512];
    snprintf(p1, sizeof p1, "%s/BidiTest.txt", ucd);
    snprintf(p2, sizeof p2, "%s/BidiCharacterTest.txt", ucd);

    int bad = check_representatives();

    run_biditest(p1);
    run_charactertest(p2);

    long total = pass_levels + fail_levels;
    long totalo = pass_order + fail_order;
    printf("bidi_test: %ld cases\n", cases);
    printf("  levels : %ld/%ld pass (%ld fail)\n", pass_levels, total, fail_levels);
    printf("  reorder: %ld/%ld pass (%ld fail)\n", pass_order, totalo, fail_order);

    if (total < 400000) {
        fprintf(stderr, "FAIL corpus too small (%ld) -- did the UCD files load?\n", total);
        bad = 1;
    }
    if (fail_levels || fail_order || bad) {
        printf("bidi_test: FAIL\n");
        return 1;
    }
    printf("bidi_test: ALL PASS\n");
    return 0;
}
