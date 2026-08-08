/* <fnmatch.h>: shell wildcard matching, recursive-descent over pattern and
 * string. The recursion is bounded by the PATTERN length (each '*' either
 * consumes one string character and retries, or is done), so a pathological
 * "many stars" pattern is the only thing that can make it slow -- never
 * unbounded, because every recursive call advances through the pattern. */
#include <fnmatch.h>
#include <ctype.h>
#include <stddef.h>

static int chreq(int a, int b, int fold)
{
    if (fold) return tolower(a) == tolower(b);
    return a == b;
}

/* Match a bracket expression starting at pat[0]=='[' against character c.
 * *out_next is set to the position just past the closing ']'. Returns 1 if c
 * is in the set, 0 if not, -1 if the bracket is malformed (treat '[' as a
 * literal in that case, per POSIX). */
static int match_bracket(const char *pat, int c, int fold, const char **out_next)
{
    const char *p = pat + 1;   /* skip '[' */
    int neg = 0;
    if (*p == '!' || *p == '^') { neg = 1; p++; }
    const char *start = p;     /* first char may be ']' literally */
    int matched = 0;
    int first = 1;
    while (*p && (*p != ']' || first)) {
        first = 0;
        if (*p == '[' && p[1] == ':') {
            /* named class [:alpha:] etc. */
            const char *cls = p + 2;
            const char *end = cls;
            while (*end && !(*end == ':' && end[1] == ']')) end++;
            if (!*end) { p++; continue; }   /* malformed class: treat '[' literally */
            size_t len = (size_t)(end - cls);
            int hit = 0;
            #define CLSIS(name, fn) (len == sizeof(name) - 1 && __builtin_memcmp(cls, name, len) == 0 && (fn)(c))
            if (CLSIS("alpha", isalpha)) hit = 1;
            else if (CLSIS("digit", isdigit)) hit = 1;
            else if (CLSIS("alnum", isalnum)) hit = 1;
            else if (CLSIS("space", isspace)) hit = 1;
            else if (CLSIS("upper", isupper)) hit = 1;
            else if (CLSIS("lower", islower)) hit = 1;
            else if (CLSIS("punct", ispunct)) hit = 1;
            else if (CLSIS("cntrl", iscntrl)) hit = 1;
            else if (CLSIS("print", isprint)) hit = 1;
            else if (CLSIS("graph", isgraph)) hit = 1;
            else if (CLSIS("blank", isblank)) hit = 1;
            else if (CLSIS("xdigit", isxdigit)) hit = 1;
            #undef CLSIS
            if (hit) matched = 1;
            p = end + 2;
            continue;
        }
        int lo = (unsigned char)*p;
        p++;
        if (*p == '-' && p[1] && p[1] != ']') {
            int hi = (unsigned char)p[1];
            p += 2;
            if (fold) {
                int cl = tolower(c);
                if (cl >= tolower(lo) && cl <= tolower(hi)) matched = 1;
                if (c >= lo && c <= hi) matched = 1;
            } else if (c >= lo && c <= hi) matched = 1;
        } else {
            if (chreq(lo, c, fold)) matched = 1;
        }
    }
    if (*p != ']') { *out_next = start; return -1; }   /* malformed: literal '[' */
    *out_next = p + 1;
    return neg ? !matched : matched;
}

/* `leading` says whether *str is a "leading" position for FNM_PERIOD: the
 * very start of the whole string, or (only when FNM_PATHNAME is also set)
 * the character right after a '/'. It is threaded through explicitly rather
 * than derived from str[-1] because the top-level start position has nothing
 * valid before it to look at. */
static int domatch(const char *pat, const char *str, int flags, int leading)
{
    int fold = (flags & FNM_CASEFOLD) != 0;
    int pathname = (flags & FNM_PATHNAME) != 0;
    int period = (flags & FNM_PERIOD) != 0;
    int noescape = (flags & FNM_NOESCAPE) != 0;

    while (1) {
        if (period && leading && *str == '.' && *pat != '.') return FNM_NOMATCH;

        if (*pat == 0) return *str == 0 ? 0 : FNM_NOMATCH;

        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (*pat == 0) {
                if (pathname) {
                    for (const char *s = str; *s; s++) if (*s == '/') return FNM_NOMATCH;
                }
                return 0;
            }
            int first = 1;
            for (const char *s = str; ; s++, first = 0) {
                int sub_leading = first ? leading : 0;
                if (pathname && *s == '/') {
                    /* '*' does not cross '/': only try matching right here */
                    return domatch(pat, s, flags, sub_leading);
                }
                if (domatch(pat, s, flags, sub_leading) == 0) return 0;
                if (*s == 0) return FNM_NOMATCH;
            }
        }

        if (*pat == '?') {
            if (*str == 0) return FNM_NOMATCH;
            if (pathname && *str == '/') return FNM_NOMATCH;
            leading = (pathname && *str == '/');
            pat++; str++;
            continue;
        }

        if (*pat == '[') {
            if (*str == 0) return FNM_NOMATCH;
            if (pathname && *str == '/') return FNM_NOMATCH;
            const char *next;
            int r = match_bracket(pat, (unsigned char)*str, fold, &next);
            if (r < 0) {
                if (!chreq('[', (unsigned char)*str, fold)) return FNM_NOMATCH;
                leading = (pathname && *str == '/');
                pat++; str++;
                continue;
            }
            if (!r) return FNM_NOMATCH;
            leading = (pathname && *str == '/');
            pat = next; str++;
            continue;
        }

        if (*pat == '\\' && !noescape && pat[1]) {
            pat++;
            if (!chreq((unsigned char)*pat, (unsigned char)*str, fold)) return FNM_NOMATCH;
            leading = (pathname && *str == '/');
            pat++; str++;
            continue;
        }

        if (*str == 0) return FNM_NOMATCH;
        if (!chreq((unsigned char)*pat, (unsigned char)*str, fold)) return FNM_NOMATCH;
        leading = (pathname && *str == '/');
        pat++; str++;
    }
}

int fnmatch(const char *pattern, const char *string, int flags)
{
    if (!pattern || !string) return FNM_NOMATCH;
    return domatch(pattern, string, flags, 1);
}
