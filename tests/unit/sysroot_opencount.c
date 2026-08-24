/* sysroot_opencount.c -- an LD_PRELOAD shim for the HOST: counts every
 * open()/open64()/openat()/fopen()/fopen64() a program makes and how many of
 * them fail with ENOENT, and prints both at exit. Built and used only by
 * tests/boot/run-sysroot-device.sh, to count what `tcc -E` over the sysroot
 * costs in failed opens -- the device kernel exposes no open or lookup
 * counter (/dev/kstat, /proc: measured 2026-08-21, neither has one), and
 * strace is not installed in the build WSL. tcc 0.9.27 opens input files
 * with open() (tccpp.c tcc_open) and the output with fopen(), so these five
 * are the complete set for it. A wrapper rather than a patch to tcc: the
 * binary counted is the unmodified cross-tcc, the same source as tcc.aex.
 * With OPENCOUNT_LOG set, every call is also logged one per line, so the
 * MISSes can be read back by path and the count is not a bare number. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static long n_open, n_enoent, n_fopen;
static FILE *g_log;
static int g_log_tried;

static void note(const char *how, const char *path, int ok)
{
    int saved = errno;
    if (!g_log_tried) {
        const char *p = getenv("OPENCOUNT_LOG");
        g_log_tried = 1;
        if (p) g_log = fopen(p, "a");   /* our own fopen: counted once, harmless */
    }
    if (g_log) fprintf(g_log, "%s %s %s\n", ok ? "ok  " : "MISS", how, path);
    errno = saved;
}

static void report(void) __attribute__((destructor));
static void report(void)
{
    FILE *f = stderr;
    const char *p = getenv("OPENCOUNT_OUT");
    if (p) f = fopen(p, "w");
    if (!f) f = stderr;
    fprintf(f, "OPENCOUNT open=%ld enoent=%ld fopen=%ld\n", n_open, n_enoent, n_fopen);
    if (f != stderr) fclose(f);
    if (g_log) fclose(g_log);
}

int open(const char *path, int flags, ...)
{
    static int (*real)(const char *, int, ...);
    mode_t mode = 0;
    if (!real) real = dlsym(RTLD_NEXT, "open");
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    int r = real(path, flags, mode);
    n_open++;
    if (r < 0 && errno == ENOENT) n_enoent++;
    note("open", path, r >= 0);
    return r;
}
int open64(const char *path, int flags, ...)
{
    static int (*real)(const char *, int, ...);
    mode_t mode = 0;
    if (!real) real = dlsym(RTLD_NEXT, "open64");
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    int r = real(path, flags, mode);
    n_open++;
    if (r < 0 && errno == ENOENT) n_enoent++;
    note("open64", path, r >= 0);
    return r;
}
int openat(int dfd, const char *path, int flags, ...)
{
    static int (*real)(int, const char *, int, ...);
    mode_t mode = 0;
    if (!real) real = dlsym(RTLD_NEXT, "openat");
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap); }
    int r = real(dfd, path, flags, mode);
    n_open++;
    if (r < 0 && errno == ENOENT) n_enoent++;
    note("openat", path, r >= 0);
    return r;
}
FILE *fopen(const char *path, const char *mode)
{
    static FILE *(*real)(const char *, const char *);
    if (!real) real = dlsym(RTLD_NEXT, "fopen");
    FILE *r = real(path, mode);
    n_fopen++;
    if (!r && errno == ENOENT) n_enoent++;
    note("fopen", path, r != NULL);
    return r;
}
FILE *fopen64(const char *path, const char *mode)
{
    static FILE *(*real)(const char *, const char *);
    if (!real) real = dlsym(RTLD_NEXT, "fopen64");
    FILE *r = real(path, mode);
    n_fopen++;
    if (!r && errno == ENOENT) n_enoent++;
    note("fopen64", path, r != NULL);
    return r;
}
