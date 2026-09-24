/* The `#!` parser (c/kernel/exec/load/shebang.h) against Linux's rules
 * (fs/binfmt_script.c), case by case. Each expectation is what Linux does with
 * the same first line, not what this parser happens to do.
 *
 * The control, -DSHEBANG_NEGCTL_NO_TRIM, keeps a trailing CR on the line --
 * the bug a CRLF file triggers ("/bin/as\r: not found") -- and must fail. */
#include <stdio.h>
#include <string.h>

#ifdef SHEBANG_NEGCTL_NO_TRIM
#define shebang_parse shebang_parse_real
#endif
#include "shebang.h"
#ifdef SHEBANG_NEGCTL_NO_TRIM
#undef shebang_parse
/* The control: the same parser handed a line whose CR was never trimmed. */
static int shebang_parse(char *buf, int n, int whole, char **interp, char **arg)
{
    int r = shebang_parse_real(buf, n, whole, interp, arg);
    if (r == SHEBANG_OK && *interp) {
        char *e = *interp + strlen(*interp);
        if (n > 0 && memchr(buf, '\r', (size_t)n) && !*arg) { e[0] = '\r'; e[1] = 0; }
    }
    return r;
}
#endif

static int checks, fails;

static void t(const char *name, const char *file, int whole, int want,
              const char *wi, const char *wa)
{
    char buf[SHEBANG_MAX + 2];
    int n = (int)strlen(file);
    if (n > SHEBANG_MAX) n = SHEBANG_MAX;
    memcpy(buf, file, (size_t)n);
    buf[n] = 0;
    char *i = 0, *a = 0;
    int r = shebang_parse(buf, n, whole, &i, &a);
    int ok = r == want;
    if (ok && want == SHEBANG_OK) {
        ok = i && strcmp(i, wi) == 0 && ((!wa && !a) || (wa && a && strcmp(a, wa) == 0));
    }
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: r=%d interp=[%s] arg=[%s]\n", name, r, i ? i : "(null)", a ? a : "(null)");
    }
}

int main(void)
{
    t("plain", "#!/bin/as\nprint(1)\n", 1, SHEBANG_OK, "/bin/as", 0);
    t("blank after #!", "#! /bin/as\n", 1, SHEBANG_OK, "/bin/as", 0);
    t("one arg", "#!/bin/as -q\n", 1, SHEBANG_OK, "/bin/as", "-q");
    t("args are ONE string", "#!/bin/as -x -y\n", 1, SHEBANG_OK, "/bin/as", "-x -y");
    t("tabs", "#!\t/bin/as\t  -q  \n", 1, SHEBANG_OK, "/bin/as", "-q");
    t("CRLF", "#!/bin/as\r\nprint(1)\r\n", 1, SHEBANG_OK, "/bin/as", 0);
    t("CRLF with arg", "#!/bin/as -q\r\n", 1, SHEBANG_OK, "/bin/as", "-q");
    t("line ends at EOF", "#!/bin/as", 1, SHEBANG_OK, "/bin/as", 0);
    t("not a script", "\x7f" "ELF....", 1, SHEBANG_NOT, 0, 0);
    t("one byte", "#", 1, SHEBANG_NOT, 0, 0);
    t("# only comment", "# hello\n", 1, SHEBANG_NOT, 0, 0);
    t("no interpreter", "#!\n", 1, SHEBANG_EMPTY, 0, 0);
    t("only blanks", "#!   \n", 1, SHEBANG_EMPTY, 0, 0);

    /* A line that does not end within the window, of a file that goes on:
     * refused, never truncated to a different interpreter. */
    char longf[SHEBANG_MAX + 40];
    memcpy(longf, "#!/", 3);
    memset(longf + 3, 'a', sizeof longf - 4);
    longf[sizeof longf - 1] = 0;
    t("too long", longf, 0, SHEBANG_TOOLONG, 0, 0);

    printf("shebang: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
