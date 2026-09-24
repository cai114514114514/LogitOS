/* sed [-nE] [-i[SUFFIX]] [-e SCRIPT]... [-f FILE]... [SCRIPT] [FILE...]
 *
 * A stream editor over lre (lre.h). What it implements, and nothing it
 * pretends to:
 *
 *   addresses   N  $  /RE/  \cREc  A,B  A!   (GNU `first~step` and `A,+N`
 *               are refused by name)
 *   commands    { } s y d D p P n N q = a i c h H g G x b t :label #
 *   s flags     g p N i/I, and Ng (the Nth and every later one)
 *   options     -n -e -f -E/-r -i[SUFFIX] (GNU form: the suffix is attached)
 *
 * a/i/c take POSIX `a\` + newline + text or GNU's one-line `a text`. The
 * replacement understands & \1..\9 \n and an escaped delimiter. Regular
 * expressions are BRE unless -E; `\n` in one matches a newline in the pattern
 * space (after N). Exit status: 0, 1 for a bad script, 2 for a missing file,
 * or q's argument. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "lre.h"

enum { A_NONE, A_LINE, A_LAST, A_RE };
struct addr { int kind; long line; struct lre *re; };

struct cmd {
    char c;
    struct addr a1, a2;
    int naddr, neg;
    int in_range;                       /* state for a two-address command */
    long range_end;                     /* A_LINE a2: the line number */
    /* s */
    struct lre *re; char *repl; int global, nth, print;
    /* y */
    unsigned char ymap[256];
    /* a i c b t : q */
    char *text; char *label; int target; int qcode;
    int block_end;                      /* '{': index of its '}' */
};

static struct cmd *cmds;
static int ncmds, capcmds;
static int o_n, o_E, o_i;
static const char *o_suffix = "";
static char *script;
static size_t scriptlen;

static void die(const char *m) { fprintf(stderr, "sed: -e expression: %s\n", m); exit(1); }

static void addscript(const char *s)
{
    size_t n = strlen(s);
    char *p = realloc(script, scriptlen + n + 2);
    if (!p) die("out of memory");
    script = p;
    memcpy(script + scriptlen, s, n);
    scriptlen += n;
    script[scriptlen++] = '\n';
    script[scriptlen] = 0;
}

/* ---------------------------------------------------------------- parse -- */

static const char *sp;

static void skipws(void) { while (*sp == ' ' || *sp == '\t') sp++; }

/* Read up to an unescaped `delim`; handles \delim and (for REs) \n. Returns a
 * malloc'd string with \delim turned into delim. */
static char *delimited(char delim, int is_re)
{
    size_t cap = 64, n = 0;
    char *b = malloc(cap);
    if (!b) die("out of memory");
    for (;;) {
        char c = *sp;
        if (!c || (c == '\n' && !is_re)) die("unterminated `s' command");
        if (c == '\n') die("unterminated address regex");
        if (c == delim) { sp++; break; }
        if (n + 3 >= cap) { cap *= 2; b = realloc(b, cap); if (!b) die("out of memory"); }
        if (c == '\\' && sp[1]) {
            if (sp[1] == delim) { b[n++] = delim; sp += 2; continue; }
            if (sp[1] == 'n' && is_re) { b[n++] = '\\'; b[n++] = 'n'; sp += 2; continue; }
            b[n++] = c; b[n++] = sp[1]; sp += 2; continue;
        }
        /* A bracket expression may contain the delimiter unescaped. */
        if (is_re && c == '[') {
            b[n++] = *sp++;
            if (*sp == '^') b[n++] = *sp++;
            if (*sp == ']') b[n++] = *sp++;
            while (*sp && *sp != ']') {
                if (n + 3 >= cap) { cap *= 2; b = realloc(b, cap); if (!b) die("out of memory"); }
                if (sp[0] == '[' && (sp[1] == ':' || sp[1] == '.' || sp[1] == '=')) {
                    char k = sp[1];
                    b[n++] = *sp++; b[n++] = *sp++;
                    while (*sp && !(sp[0] == k && sp[1] == ']')) {
                        if (n + 3 >= cap) { cap *= 2; b = realloc(b, cap); if (!b) die("out of memory"); }
                        b[n++] = *sp++;
                    }
                    if (*sp) { b[n++] = *sp++; b[n++] = *sp++; }
                    continue;
                }
                b[n++] = *sp++;
            }
            continue;
        }
        b[n++] = c; sp++;
    }
    b[n] = 0;
    return b;
}

static struct lre *compile_re(const char *pat, int icase)
{
    static struct lre *last;
    if (!*pat) {                     /* the empty RE is the last one used */
        if (!last) die("no previous regular expression");
        return last;
    }
    struct lre *re;
    char err[128];
    if (lre_compile(&re, pat, (o_E ? LRE_EXTENDED : 0) | (icase ? LRE_ICASE : 0), err, sizeof err)) {
        fprintf(stderr, "sed: -e expression: %s\n", err);
        exit(1);
    }
    last = re;
    return re;
}

static int parse_addr(struct addr *a)
{
    if (*sp >= '0' && *sp <= '9') {
        a->kind = A_LINE; a->line = strtol(sp, (char **)&sp, 10);
        if (*sp == '~') die("`first~step' addresses are not supported");
        return 1;
    }
    if (*sp == '$') { sp++; a->kind = A_LAST; return 1; }
    if (*sp == '/' || *sp == '\\') {
        char d = '/';
        if (*sp == '\\') { sp++; d = *sp; if (!d || d == '\n' || d == '\\') die("unexpected `,'"); }
        sp++;
        char *pat = delimited(d, 1);
        int icase = 0;
        while (*sp == 'I') { icase = 1; sp++; }
        a->kind = A_RE; a->re = compile_re(pat, icase);
        free(pat);
        return 1;
    }
    return 0;
}

/* Text for a/i/c: `a\` NEWLINE text (continuation with trailing \), or GNU
 * one-line `a text`. */
static char *read_text(void)
{
    skipws();
    size_t cap = 64, n = 0;
    char *b = malloc(cap);
    if (!b) die("out of memory");
    if (*sp == '\\') { sp++; skipws(); if (*sp == '\n') sp++; }
    for (;;) {
        char c = *sp;
        if (!c) break;
        if (n + 2 >= cap) { cap *= 2; b = realloc(b, cap); if (!b) die("out of memory"); }
        if (c == '\\') {
            if (sp[1] == '\n') { b[n++] = '\n'; sp += 2; continue; }
            if (sp[1]) { b[n++] = sp[1]; sp += 2; continue; }
            sp++; continue;
        }
        if (c == '\n') { sp++; break; }
        b[n++] = c; sp++;
    }
    b[n] = 0;
    return b;
}

static char *read_label(void)
{
    skipws();
    const char *s = sp;
    while (*sp && *sp != '\n' && *sp != ';') sp++;
    const char *e = sp;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    char *l = malloc((size_t)(e - s) + 1);
    if (!l) die("out of memory");
    memcpy(l, s, (size_t)(e - s)); l[e - s] = 0;
    return l;
}

static struct cmd *newcmd(void)
{
    if (ncmds == capcmds) {
        capcmds = capcmds ? capcmds * 2 : 32;
        cmds = realloc(cmds, sizeof *cmds * (size_t)capcmds);
        if (!cmds) die("out of memory");
    }
    memset(&cmds[ncmds], 0, sizeof cmds[ncmds]);
    return &cmds[ncmds++];
}

static void parse(void)
{
    int stack[64], depth = 0;
    sp = script;
    for (;;) {
        while (*sp == ' ' || *sp == '\t' || *sp == '\n' || *sp == ';') sp++;
        if (!*sp) break;
        if (*sp == '#') { while (*sp && *sp != '\n') sp++; continue; }
        struct cmd *c = newcmd();
        int ci = ncmds - 1;
        if (parse_addr(&c->a1)) {
            c->naddr = 1;
            if (*sp == ',') {
                sp++;
                if (*sp == '+' || *sp == '~') die("`addr,+N' and `addr,~N' are not supported");
                if (!parse_addr(&c->a2)) die("unexpected `,'");
                c->naddr = 2;
            }
        }
        skipws();
        while (*sp == '!') { c->neg = 1; sp++; skipws(); }
        c->c = *sp++;
        switch (c->c) {
        case '{':
            if (depth == 64) die("too many nested blocks");
            stack[depth++] = ci;
            continue;                   /* a command may follow `{` directly */
        case '}':
            if (!depth) die("unexpected `}'");
            if (c->naddr) die("} doesn't want any addresses");
            cmds[stack[--depth]].block_end = ci;
            break;
        case 's': {
            char d = *sp++;
            if (!d || d == '\n' || d == '\\') die("unterminated `s' command");
            char *pat = delimited(d, 1);
            c->repl = delimited(d, 0);
            int icase = 0;
            for (;;) {
                if (*sp == 'g') { c->global = 1; sp++; }
                else if (*sp == 'p') { c->print = 1; sp++; }
                else if (*sp == 'i' || *sp == 'I') { icase = 1; sp++; }
                else if (*sp >= '0' && *sp <= '9') c->nth = (int)strtol(sp, (char **)&sp, 10);
                else if (*sp == 'w') die("the `w' flag is not supported");
                else break;
            }
            c->re = compile_re(pat, icase);
            free(pat);
            break;
        }
        case 'y': {
            char d = *sp++;
            char *a = delimited(d, 0), *b = delimited(d, 0);
            for (int k = 0; k < 256; k++) c->ymap[k] = (unsigned char)k;
            /* \\ and \n in y's strings */
            char *src[2] = { a, b }, *out[2];
            size_t len[2];
            for (int z = 0; z < 2; z++) {
                out[z] = malloc(strlen(src[z]) + 1); if (!out[z]) die("out of memory");
                size_t m = 0;
                for (const char *q = src[z]; *q; q++) {
                    if (*q == '\\' && q[1] == 'n') { out[z][m++] = '\n'; q++; }
                    else if (*q == '\\' && q[1] == '\\') { out[z][m++] = '\\'; q++; }
                    else out[z][m++] = *q;
                }
                len[z] = m;
            }
            if (len[0] != len[1]) die("strings for `y' command are different lengths");
            for (size_t k = 0; k < len[0]; k++) c->ymap[(unsigned char)out[0][k]] = (unsigned char)out[1][k];
            free(a); free(b); free(out[0]); free(out[1]);
            break;
        }
        case 'a': case 'i': case 'c':
            c->text = read_text();
            continue;                   /* read_text consumed the line end */
        case ':':
            if (c->naddr) die(": doesn't want any addresses");
            c->label = read_label();
            if (!*c->label) die("\":\" lacks a label");
            break;
        case 'b': case 't': c->label = read_label(); break;
        case 'q':
            skipws();
            if (*sp >= '0' && *sp <= '9') c->qcode = (int)strtol(sp, (char **)&sp, 10);
            break;
        case 'd': case 'D': case 'p': case 'P': case 'n': case 'N': case '=':
        case 'h': case 'H': case 'g': case 'G': case 'x':
            break;
        default:
            fprintf(stderr, "sed: -e expression: unknown command: `%c'\n", c->c);
            exit(1);
        }
        skipws();
        if (*sp == '}') continue;
        if (*sp == '#') continue;
        if (*sp && *sp != '\n' && *sp != ';') {
            fprintf(stderr, "sed: -e expression: extra characters after command\n");
            exit(1);
        }
    }
    if (depth) die("unmatched `{'");
    for (int k = 0; k < ncmds; k++) {
        if (cmds[k].c != 'b' && cmds[k].c != 't') continue;
        cmds[k].target = ncmds;                          /* empty label: end of script */
        if (!*cmds[k].label) continue;
        int found = 0;
        for (int j = 0; j < ncmds; j++)
            if (cmds[j].c == ':' && !strcmp(cmds[j].label, cmds[k].label)) { cmds[k].target = j; found = 1; }
        if (!found) { fprintf(stderr, "sed: -e expression: can't find label for jump to `%s'\n", cmds[k].label); exit(1); }
    }
}

/* ---------------------------------------------------------------- input -- */

static char **files;
static int nfiles, fileidx;
static FILE *in;
static char *look; static size_t lookcap; static ssize_t looklen = -1;
static long lineno;
static int quit_now = 0, exitcode = 0;
static FILE *out;

static int open_next(void)
{
    while (fileidx < nfiles) {
        const char *f = files[fileidx++];
        if (!strcmp(f, "-")) { in = stdin; return 1; }
        in = fopen(f, "r");
        if (in) return 1;
        fprintf(stderr, "sed: can't read %s: No such file or directory\n", f);
        exitcode = 2;
    }
    return 0;
}

/* One line ahead is always read, so `$` is known before the last line runs. */
static void prime(void)
{
    looklen = -1;
    for (;;) {
        if (!in && !open_next()) return;
        looklen = getline(&look, &lookcap, in);
        if (looklen >= 0) return;
        if (in != stdin) fclose(in);
        in = 0;
        if (o_i) return;          /* -i: each file is its own stream */
    }
}

static int next_line(char **buf, size_t *cap, size_t *len)
{
    if (looklen < 0) return 0;
    size_t n = (size_t)looklen;
    if (n && look[n - 1] == '\n') n--;
    if (n + 1 > *cap) { *cap = n + 64; *buf = realloc(*buf, *cap); if (!*buf) die("out of memory"); }
    memcpy(*buf, look, n); (*buf)[n] = 0; *len = n;
    lineno++;
    prime();
    return 1;
}
static int is_last(void) { return looklen < 0; }

/* ----------------------------------------------------------------- run -- */

struct buf { char *p; size_t n, cap; };
static void bput(struct buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); if (!b->p) die("out of memory"); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}

static struct buf ps, hs, appendq;

static int match_addr(const struct addr *a)
{
    int sub[2 * LRE_NSUB];
    switch (a->kind) {
    case A_LINE: return lineno == a->line;
    case A_LAST: return is_last();
    case A_RE:   return lre_exec(a->re, ps.p ? ps.p : "", (int)ps.n, 0, 0, sub);
    }
    return 0;
}

static int selected(struct cmd *c)
{
    int r;
    if (!c->naddr) r = 1;
    else if (c->naddr == 1) r = match_addr(&c->a1);
    else if (c->in_range) {
        r = 1;
        if (c->a2.kind == A_LINE) { if (lineno >= c->a2.line) c->in_range = 0; }
        else if (match_addr(&c->a2)) c->in_range = 0;
    } else if (match_addr(&c->a1)) {
        r = 1;
        c->in_range = 1;
        /* A line-number end at or before where the range starts: one line. */
        if (c->a2.kind == A_LINE && c->a2.line <= lineno) c->in_range = 0;
        if (c->a2.kind == A_LAST && is_last()) c->in_range = 0;
    } else r = 0;
    return c->neg ? !r : r;
}

static int substitute(struct cmd *c)
{
    struct buf o = {0};
    int sub[2 * LRE_NSUB], from = 0, count = 0, did = 0, prev_end = -1;
    const char *s = ps.p ? ps.p : "";
    int n = (int)ps.n;
    while (from <= n && lre_exec(c->re, s, n, from, 0, sub)) {
        /* An EMPTY match right where the previous match ended is not a new
         * match (s/b*\/x/g on "abc" is "xaxcx", not "xaxxcx"): step past it. */
        if (sub[1] == sub[0] && sub[0] == prev_end) {
            if (sub[0] < n) bput(&o, s + from, (size_t)(sub[0] + 1 - from));
            from = sub[0] + 1;
            prev_end = -1;
            continue;
        }
        prev_end = sub[1];
        count++;
        int want = c->nth ? count >= c->nth && (c->global || count == c->nth) : 1;
        bput(&o, s + from, (size_t)(sub[0] - from));
        if (want) {
            did = 1;
            for (const char *r = c->repl; *r; r++) {
                if (*r == '\\' && r[1]) {
                    r++;
                    if (*r >= '0' && *r <= '9') {
                        int k = *r - '0';
                        if (sub[2 * k] >= 0) bput(&o, s + sub[2 * k], (size_t)(sub[2 * k + 1] - sub[2 * k]));
                    } else if (*r == 'n') bput(&o, "\n", 1);
                    else if (*r == 't') bput(&o, "\t", 1);
                    else bput(&o, r, 1);
                } else if (*r == '&') bput(&o, s + sub[0], (size_t)(sub[1] - sub[0]));
                else bput(&o, r, 1);
            }
        } else bput(&o, s + sub[0], (size_t)(sub[1] - sub[0]));
        if (sub[1] == sub[0]) {                 /* empty match: step one byte */
            if (sub[1] < n) bput(&o, s + sub[1], 1);
            from = sub[1] + 1;
        } else from = sub[1];
        if (!c->global && want) break;
        if (!c->global && c->nth && count >= c->nth) break;
    }
    if (from < n) bput(&o, s + from, (size_t)(n - from));
    if (!did) { free(o.p); return 0; }
    free(ps.p);
    ps = o;
    if (!ps.p) bput(&ps, "", 0);
    return 1;
}

static void emit_ps(void) { fwrite(ps.p ? ps.p : "", 1, ps.n, out); fputc('\n', out); }
static void flush_append(void) { if (appendq.n) { fwrite(appendq.p, 1, appendq.n, out); appendq.n = 0; } }

static void run(void)
{
    char *line = 0; size_t cap = 0, len;
    int tflag = 0;
    while (!quit_now && next_line(&line, &cap, &len)) {
        ps.n = 0; bput(&ps, line, len);
        tflag = 0;
        int restart = 0;
    again:;
        int deleted = 0;
        for (int pc = 0; pc < ncmds && !deleted; pc++) {
            struct cmd *c = &cmds[pc];
            if (c->c == '}' || c->c == ':') continue;
            if (!selected(c)) { if (c->c == '{') pc = c->block_end; continue; }
            switch (c->c) {
            case '{': break;
            case 's': if (substitute(c)) { tflag = 1; if (c->print) emit_ps(); } break;
            case 'y': for (size_t k = 0; k < ps.n; k++) ps.p[k] = (char)c->ymap[(unsigned char)ps.p[k]]; break;
            case 'd': deleted = 1; break;
            case 'D': {
                char *nl = ps.p ? memchr(ps.p, '\n', ps.n) : 0;
                if (!nl) { deleted = 1; break; }
                size_t k = (size_t)(nl - ps.p) + 1;
                memmove(ps.p, ps.p + k, ps.n - k); ps.n -= k; ps.p[ps.n] = 0;
                flush_append();
                restart = 1; deleted = 1;
                break;
            }
            case 'p': emit_ps(); break;
            case 'P': { char *nl = ps.p ? memchr(ps.p, '\n', ps.n) : 0;
                        fwrite(ps.p ? ps.p : "", 1, nl ? (size_t)(nl - ps.p) : ps.n, out); fputc('\n', out); break; }
            case 'n':
                if (is_last()) { if (!o_n) emit_ps(); flush_append(); quit_now = 1; deleted = 1; break; }
                if (!o_n) emit_ps();
                flush_append();
                next_line(&line, &cap, &len);
                ps.n = 0; bput(&ps, line, len);
                break;
            case 'N':
                if (is_last()) {
                    /* GNU prints the pattern space when N runs out of input
                     * (POSIX says delete); GNU is what scripts expect. */
                    if (!o_n) emit_ps();
                    flush_append(); quit_now = 1; deleted = 1; break;
                }
                flush_append();
                next_line(&line, &cap, &len);
                bput(&ps, "\n", 1); bput(&ps, line, len);
                break;
            case 'q': if (!o_n) emit_ps(); flush_append(); quit_now = 1; exitcode = c->qcode; deleted = 2; break;
            case '=': fprintf(out, "%ld\n", lineno); break;
            case 'a': bput(&appendq, c->text, strlen(c->text)); bput(&appendq, "\n", 1); break;
            case 'i': fputs(c->text, out); fputc('\n', out); break;
            case 'c':
                /* In a range, the text replaces the whole range: printed once,
                 * at its end. */
                if (c->naddr < 2 || !c->in_range) { fputs(c->text, out); fputc('\n', out); }
                deleted = 1;
                break;
            case 'h': hs.n = 0; bput(&hs, ps.p ? ps.p : "", ps.n); break;
            case 'H': bput(&hs, "\n", 1); bput(&hs, ps.p ? ps.p : "", ps.n); break;
            case 'g': ps.n = 0; bput(&ps, hs.p ? hs.p : "", hs.n); break;
            case 'G': bput(&ps, "\n", 1); bput(&ps, hs.p ? hs.p : "", hs.n); break;
            case 'x': { struct buf t = ps; ps = hs; hs = t; if (!ps.p) bput(&ps, "", 0); break; }
            case 'b': pc = c->target - 1; break;
            case 't': if (tflag) { tflag = 0; pc = c->target - 1; } break;
            }
        }
        if (!deleted && !o_n) emit_ps();
        if (deleted != 2) flush_append();
        if (restart && !quit_now) { restart = 0; goto again; }
    }
    free(line);
}

static void usage(void)
{
    fputs("usage: sed [-nE] [-i[SUFFIX]] [-e script]... [-f file]... [script] [file...]\n", stderr);
    exit(1);
}

int main(int argc, char **argv)
{
    int i = 1, have = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (argv[i][1] == 'i') { o_i = 1; o_suffix = argv[i] + 2; continue; }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'n') o_n = 1;
            else if (*f == 'E' || *f == 'r') o_E = 1;
            else if (*f == 's') {}
            else if (*f == 'e') {
                const char *e = f[1] ? f + 1 : (++i < argc ? argv[i] : 0);
                if (!e) usage();
                addscript(e); have = 1; goto next;
            } else if (*f == 'f') {
                const char *fn = f[1] ? f + 1 : (++i < argc ? argv[i] : 0);
                if (!fn) usage();
                FILE *sf = fopen(fn, "r");
                if (!sf) { fprintf(stderr, "sed: couldn't open file %s\n", fn); return 1; }
                char *l = 0; size_t c = 0; ssize_t n;
                while ((n = getline(&l, &c, sf)) >= 0) { if (n && l[n - 1] == '\n') l[n - 1] = 0; addscript(l); }
                free(l); fclose(sf); have = 1; goto next;
            } else { fprintf(stderr, "sed: invalid option -- '%c'\n", *f); usage(); }
        }
    next:;
    }
    if (!have) { if (i >= argc) usage(); addscript(argv[i++]); }
    parse();
    out = stdout;
    static char *dash[] = { "-" };
    if (i < argc) { files = argv + i; nfiles = argc - i; } else { files = dash; nfiles = 1; }
    if (!o_i) {
        prime();
        run();
    } else {
        for (int k = 0; k < nfiles && !quit_now; k++) {
            const char *f = files[k];
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s.sedtmp", f);
            FILE *src = fopen(f, "r");
            if (!src) { fprintf(stderr, "sed: can't read %s: No such file or directory\n", f); exitcode = 2; continue; }
            out = fopen(tmp, "w");
            if (!out) { fprintf(stderr, "sed: couldn't open temporary file %s\n", tmp); fclose(src); return 4; }
            in = src; lineno = 0;
            for (int c = 0; c < ncmds; c++) cmds[c].in_range = 0;
            prime();
            run();
            if (in) { fclose(in); in = 0; }
            fclose(out);
            if (*o_suffix) {
                char bak[1024];
                snprintf(bak, sizeof bak, "%s%s", f, o_suffix);
                rename(f, bak);
            }
            if (rename(tmp, f) != 0) { fprintf(stderr, "sed: cannot rename %s\n", tmp); return 4; }
        }
        out = stdout;
    }
    fflush(stdout);
    return exitcode;
}
