/* Exercise the shipping stat command with fake metadata syscalls.  The old
 * parser silently treated an invalid first digit as zero, so checking only
 * its return value would miss the permission change that matters here. */
#include <stdio.h>
#include <string.h>

#include "logit_abi.h"

#define LOGIT_CLIB_H
#define LOGIT_APP_STAT_H

static int mask_calls, chmod_calls;
static int last_mask, last_mode;
static char error_text[160];

static int c_streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static int c_atoi(const char *s) { int n = 0; while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0'); return n; }
static void outs(const char *s) { (void)s; }
static void outc(char c) { (void)c; }
static void outn(long n) { (void)n; }
static void errs(const char *s)
{
    size_t used = strlen(error_text), left = sizeof error_text - used - 1;
    strncat(error_text, s, left);
}
static int st_stat(const char *p, struct logit_stat *s) { (void)p; (void)s; return -1; }
static int st_lstat(const char *p, struct logit_stat *s) { (void)p; (void)s; return -1; }
static int st_readlink(const char *p, char *b, int n) { (void)p; (void)b; (void)n; return -1; }
static int st_chmod(const char *p, int mode) { (void)p; chmod_calls++; last_mode = mode; return 0; }
static int st_chown(const char *p, int uid, int gid) { (void)p; (void)uid; (void)gid; return 0; }
static int st_umask(int mask) { if (mask >= 0) { mask_calls++; last_mask = mask; } return 022; }
static int st_symlink(const char *t, const char *p) { (void)t; (void)p; return 0; }
static int st_link(const char *a, const char *b) { (void)a; (void)b; return 0; }

#define main stat_main
#include "../../c/apps/coreutils/stat.c"
#undef main

static int failures;

static void check(int yes, const char *what)
{
    printf("%s %s\n", yes ? "ok" : "FAIL", what);
    failures += !yes;
}

static void reset(void)
{
    mask_calls = chmod_calls = 0;
    last_mask = last_mode = -1;
    error_text[0] = 0;
}

static void invalid(const char *op, const char *arg)
{
    char *args[] = { "stat", (char *)op, (char *)arg, "/file" };
    reset();
    int rc = stat_main(op[1] == 'u' ? 3 : 4, args);
    char label[96];
    snprintf(label, sizeof label, "%s '%s' rejects without changing metadata", op, arg);
    check(rc != 0 && mask_calls == 0 && chmod_calls == 0 &&
          strstr(error_text, "invalid") != NULL, label);
}

int main(void)
{
    char *mask[] = { "stat", "-u", "077" };
    char *query[] = { "stat", "-u" };
    char *mode[] = { "stat", "-c", "04755", "/file" };

    reset();
    check(stat_main(3, mask) == 0 && mask_calls == 1 && last_mask == 077,
          "valid umask is applied");
    reset();
    check(stat_main(2, query) == 0 && mask_calls == 0,
          "umask query is read only");
    reset();
    check(stat_main(4, mode) == 0 && chmod_calls == 1 && last_mode == 04755,
          "valid chmod mode is applied");

    invalid("-u", "9");
    invalid("-u", "08");
    invalid("-u", "");
    invalid("-u", "077x");
    invalid("-u", "1000");
    invalid("-u", "77777777777777777777777777777777");
    invalid("-c", "9");
    invalid("-c", "0644x");
    invalid("-c", "10000");

    printf("stat-oct: %d failures\n", failures);
    return failures != 0;
}
