/* chmod MODE FILE...
 *
 * MODE is octal (0755) or symbolic: comma-separated clauses of [ugoa]*
 * followed by one or more [+-=][rwxst]*, as POSIX specifies. No -R: nothing on
 * this machine needs it yet, and a recursive walk is exactly the kind of flag
 * that is better absent than half there.
 *
 * Written against POSIX, not against this machine's syscalls, so the SAME
 * source builds natively on the host and make test-textutils-host can compare
 * its effect with the host's own chmod. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int apply_symbolic(const char *spec, mode_t cur, mode_t *out)
{
    mode_t m = cur & 07777;
    const char *p = spec;
    for (;;) {
        mode_t who = 0;
        for (; *p == 'u' || *p == 'g' || *p == 'o' || *p == 'a'; p++)
            who |= *p == 'u' ? 04700 : *p == 'g' ? 02070 : *p == 'o' ? 01007 : 07777;
        int all = who == 0;
        if (all) who = 07777;
        if (*p != '+' && *p != '-' && *p != '=') return -1;
        while (*p == '+' || *p == '-' || *p == '=') {
            char op = *p++;
            mode_t bits = 0;
            for (; *p && strchr("rwxst", *p); p++)
                bits |= *p == 'r' ? 0444 : *p == 'w' ? 0222 : *p == 'x' ? 0111
                      : *p == 's' ? 06000 : 01000;
            bits &= who;
            /* With no `who`, POSIX masks by the umask: `chmod +w f` does not
             * grant what the umask would withhold on creation. */
#ifndef TEXTUTIL_NEGCTL
            if (all) { mode_t um = umask(0); umask(um); bits &= ~um; }
#endif
            if (op == '+') m |= bits;
            else if (op == '-') m &= ~bits;
            else m = (m & ~(who & 0777)) | bits;
        }
        if (*p == 0) break;
        if (*p != ',') return -1;
        p++;
    }
    *out = m;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fputs("usage: chmod MODE FILE...\n", stderr); return 1; }
    const char *spec = argv[1];
    int octal = 1;
    for (const char *q = spec; *q; q++) if (*q < '0' || *q > '7') octal = 0;
    if (octal && strlen(spec) > 4) octal = 0;
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        mode_t m;
        if (octal) {
            m = 0; for (const char *q = spec; *q; q++) m = m * 8 + (mode_t)(*q - '0');
        } else {
            struct stat st;
            if (stat(argv[i], &st) < 0) { fprintf(stderr, "chmod: cannot access '%s'\n", argv[i]); rc = 1; continue; }
            if (apply_symbolic(spec, st.st_mode, &m) < 0) { fprintf(stderr, "chmod: invalid mode: '%s'\n", spec); return 1; }
        }
        if (chmod(argv[i], m) < 0) { fprintf(stderr, "chmod: changing permissions of '%s' failed\n", argv[i]); rc = 1; }
    }
    return rc;
}
