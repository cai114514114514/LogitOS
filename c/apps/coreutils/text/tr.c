/* tr [-cds] SET1 [SET2]
 *
 * Sets: literal bytes, ranges a-z, escapes \n \t \r \\ \NNN (octal), classes
 * [:alpha:] etc. When SET2 is shorter than SET1 its last byte is repeated
 * (what GNU and BSD both do). -c complements SET1, -d deletes, -s squeezes
 * repeats of a byte in the last set named. Bytes, as in the C locale. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int in_class(const char *name, int n, int c)
{
#define IS(s) (n == (int)sizeof(s) - 1 && !memcmp(name, s, (size_t)n))
    if (IS("alpha")) return (c | 32) >= 'a' && (c | 32) <= 'z';
    if (IS("digit")) return c >= '0' && c <= '9';
    if (IS("alnum")) return ((c | 32) >= 'a' && (c | 32) <= 'z') || (c >= '0' && c <= '9');
    if (IS("upper")) return c >= 'A' && c <= 'Z';
    if (IS("lower")) return c >= 'a' && c <= 'z';
    if (IS("space")) return c == ' ' || (c >= '\t' && c <= '\r');
    if (IS("blank")) return c == ' ' || c == '\t';
    if (IS("punct")) return c > 32 && c < 127 && !(((c | 32) >= 'a' && (c | 32) <= 'z') || (c >= '0' && c <= '9'));
    if (IS("print")) return c >= 32 && c < 127;
    if (IS("graph")) return c > 32 && c < 127;
    if (IS("cntrl")) return c < 32 || c == 127;
    if (IS("xdigit")) return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f');
#undef IS
    return -1;
}

/* Expand a set into out[] (up to 4096 entries); returns the count, -1 bad. */
static int expand(const char *s, unsigned char *out)
{
    int n = 0;
    while (*s && n < 4096) {
        int c;
        if (s[0] == '[' && s[1] == ':') {
            const char *e = strstr(s + 2, ":]");
            if (!e) return -1;
            int len = (int)(e - s - 2);
            if (in_class(s + 2, len, 'a') < 0) return -1;
            for (int ch = 0; ch < 256 && n < 4096; ch++) if (in_class(s + 2, len, ch)) out[n++] = (unsigned char)ch;
            s = e + 2;
            continue;
        }
        if (*s == '\\' && s[1]) {
            s++;
            if (*s >= '0' && *s <= '7') { c = 0; for (int k = 0; k < 3 && *s >= '0' && *s <= '7'; k++) c = c * 8 + (*s++ - '0'); }
            else { c = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : (unsigned char)*s; s++; }
        } else c = (unsigned char)*s++;
        if (*s == '-' && s[1]) {
            int hi;
            s++;
            if (*s == '\\' && s[1]) { s++; hi = *s == 'n' ? '\n' : *s == 't' ? '\t' : (unsigned char)*s; s++; }
            else hi = (unsigned char)*s++;
            if (hi < c) return -1;
            for (int ch = c; ch <= hi && n < 4096; ch++) out[n++] = (unsigned char)ch;
            continue;
        }
        out[n++] = (unsigned char)c;
    }
    return n;
}

int main(int argc, char **argv)
{
    int o_c = 0, o_d = 0, o_s = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'c' || *f == 'C') o_c = 1;
            else if (*f == 'd') o_d = 1;
            else if (*f == 's') o_s = 1;
            else { fprintf(stderr, "tr: invalid option -- '%c'\n", *f); return 1; }
        }
    }
    if (i >= argc) { fputs("tr: missing operand\n", stderr); return 1; }
    static unsigned char s1[4096], s2[4096];
    int n1 = expand(argv[i], s1), n2 = i + 1 < argc ? expand(argv[i + 1], s2) : 0;
    if (n1 < 0 || n2 < 0) { fputs("tr: invalid set\n", stderr); return 1; }
    unsigned char in1[256] = {0}, map[256], sq[256] = {0};
    for (int k = 0; k < n1; k++) in1[s1[k]] = 1;
    if (o_c) {                                   /* complement: ascending order */
        n1 = 0;
        for (int ch = 0; ch < 256; ch++) if (!in1[ch]) s1[n1++] = (unsigned char)ch;
        memset(in1, 0, sizeof in1);
        for (int k = 0; k < n1; k++) in1[s1[k]] = 1;
    }
    for (int ch = 0; ch < 256; ch++) map[ch] = (unsigned char)ch;
    int translate = !o_d && n2 > 0;
    if (translate)
        for (int k = 0; k < n1; k++) map[s1[k]] = s2[k < n2 ? k : n2 - 1];
    /* -s squeezes the LAST set given: SET2 when translating, else SET1. */
    if (o_s) {
        if (translate || (o_d && n2)) for (int k = 0; k < n2; k++) sq[s2[k]] = 1;
        else for (int k = 0; k < n1; k++) sq[s1[k]] = 1;
    }
    int c, last = -1;
    while ((c = getchar()) != EOF) {
        if (o_d && in1[c]) continue;
        int o = map[c];
#ifndef TEXTUTIL_NEGCTL
        if (o_s && sq[o] && o == last) continue;     /* THE CONTROL never squeezes */
#endif
        putchar(o);
        last = o;
    }
    return 0;
}
