#ifndef LOGIT_FS_CHECK_H
#define LOGIT_FS_CHECK_H

#include <stdio.h>
#include <stdarg.h>

static int fs_checks, fs_failures;

static void fs_ok(int cond, const char *fmt, ...)
{
    va_list ap;
    fs_checks++;
    if (cond) return;
    fs_failures++;
    fputs("  FAIL: ", stdout);
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n');
}

static int fs_verdict(const char *name)
{
    printf("%s: %d checks, %d failure(s)\n", name, fs_checks, fs_failures);
    if (fs_failures) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}

#endif
