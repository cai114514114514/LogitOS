/* Second half of the on-target mini-libc battery -- the surface added when this
 * library was made something a program that never heard of LogitOS could build
 * against. Included by libctest_main.c (one translation unit, one binary; the
 * split is only so neither file becomes unreadable).
 *
 * WHERE THE EXPECTED VALUES COME FROM. Every literal below was produced by
 * GLIBC on the host and pasted here -- not read off this implementation. A
 * conformance test written from the code it tests proves only that the code is
 * self-consistent, which is a property a wrong strtod already has.
 *
 * WHY THIS EXISTS ALONGSIDE `make test-libc-diff`. The host differential suite
 * is far broader (800k cases against glibc), but it runs on Linux, where
 * everything most likely to break is absent. This binary runs in ring 3 on the
 * real kernel, and is the only place that exercises the int-0x80 syscall layer,
 * the arena allocator, -mno-red-zone, SSE state across preemption, and LogitFS.
 * A host pass is necessary and not sufficient. */

#define CHK_FMT(want, ...) do { char _b[512]; snprintf(_b, sizeof _b, __VA_ARGS__); \
    CHK_STR(_b, want, "printf -> " want); } while (0)

static void t_printf_float(void)
{
    /* Exact decimal conversion (dtoa.c). These are glibc's bytes. */
    CHK_FMT("0.10000000000000001", "%.17g", 0.1);
    CHK_FMT("3.33333333333333314830e-01", "%.20e", 1.0/3.0);
    CHK_FMT("100000000000000000000.000000", "%f", 1e20);
    CHK_FMT("0.100000000000000005551115123126", "%.30f", 0.1);
    /* Round-half-to-EVEN -- the case that silently rots a running total. */
    CHK_FMT("2", "%.0f", 2.5);
    CHK_FMT("4", "%.0f", 3.5);
    CHK_FMT("0", "%.0f", 0.5);
    /* %g style selection, and zero-stripping limited to the FRACTION. */
    CHK_FMT("100000", "%g", 100000.0);
    CHK_FMT("1e+06", "%g", 1000000.0);
    CHK_FMT("0.0001", "%g", 0.0001);
    CHK_FMT("1e-05", "%g", 0.00001);
    CHK_FMT("1.23e+03", "%.3g", 1234.0);
    CHK_FMT("0.000000e+00", "%e", 0.0);
    CHK_FMT("-0.000e+00", "%+.3e", -0.0);
    /* %a: at least ONE exponent digit, and 0x is a pad-aware prefix. */
    CHK_FMT("0x1p+0", "%a", 1.0);
    CHK_FMT("0x1p-1", "%a", 0.5);
    CHK_FMT("0x1.ffp+7", "%a", 255.5);
    CHK_FMT("0x1.00p+0", "%.2a", 1.0);
    CHK_FMT("0x000000000p+0", "%014a", 0.0);
    /* Field width on floats, which used to be ignored outright. */
    CHK_FMT("      3.14", "%10.2f", 3.14159);
    CHK_FMT("3.14      ", "%-10.2f", 3.14159);
    CHK_FMT("0000003.14", "%010.2f", 3.14159);
    /* inf/nan never take the zero flag. */
    CHK_FMT("       inf", "%010f", 1e308 * 10);
    CHK_FMT("      -inf", "%010f", -1e308 * 10);
}

static void t_printf_int(void)
{
    CHK_FMT("01", "%#o", 1);              /* '#' acts on the FINISHED digits */
    CHK_FMT("00001", "%#.5o", 1);
    CHK_FMT("0", "%#.0o", 0);
    CHK_FMT("", "%.0d", 0);               /* zero at precision 0 prints nothing */
    CHK_FMT("  007", "%5.3d", 7);
    CHK_FMT("005", "%.3d", 5);            /* precision on integers: was ignored */
    CHK_FMT("44", "%hhd", 300);           /* length modifiers must truncate */
    CHK_FMT("4464", "%hd", 70000);
    CHK_FMT("(nil)", "%p", (void *)0);
    CHK_FMT("0x1f", "%#x", 31);
    CHK_FMT("-9223372036854775808", "%lld", LLONG_MIN);
    CHK_FMT("18446744073709551615", "%llu", ULLONG_MAX);
    { int n = -1; char b[64];
      snprintf(b, sizeof b, "ab%dcd%n!", 123, &n);
      CHK_INT(n, 7, "printf %n counts what was produced"); }
    /* %lc / %ls emit UTF-8 */
    CHK_FMT("\xe4\xb8\xad", "%lc", (wint_t)0x4E2D);
    CHK_FMT("\xe4\xb8\xad\xe6\x96\x87", "%ls", L"中文");
}

static void t_strto(void)
{
    char *e;
    /* Correctly rounded -- compared against the COMPILER's parse of the same
     * literal, which is an independent correctly-rounded implementation. */
    CHK(strtod("1e23", 0) == 1e23, "strtod 1e23 agrees with the compiler");
    CHK(strtod("0.1", 0) == 0.1, "strtod 0.1");
    CHK(strtod("1.7976931348623157e308", 0) == 1.7976931348623157e308, "strtod DBL_MAX");
    CHK(strtod("2.2250738585072014e-308", 0) == 2.2250738585072014e-308, "strtod DBL_MIN");
    CHK(strtod("0x1.8p3", 0) == 12.0, "strtod hex float");
    CHK(strtof("0.1", 0) == 0.1f, "strtof is a real 32-bit conversion");
    /* endptr on the two inputs the old parser looped forever on. */
    CHK_INT(strtol("08:30", &e, 0), 0, "strtol 08:30 value");
    CHK_INT(e - (char *)"08:30", 1, "strtol 08:30 consumed just the 0");
    { const char *h = "0x"; CHK_INT(strtol(h, &e, 16), 0, "strtol bare 0x value");
      CHK_INT(e - h, 1, "strtol bare 0x consumed just the 0"); }
    { const char *b = "0b101"; CHK_INT(strtol(b, &e, 0), 5, "strtol C23 0b prefix");
      CHK_INT(e - b, 5, "strtol 0b endptr"); }
    errno = 0;
    CHK(strtoll("99999999999999999999", &e, 10) == LLONG_MAX, "strtoll saturates");
    CHK_INT(errno, ERANGE, "strtoll overflow sets ERANGE");
    errno = 0; CHK(strtod("1e-400", 0) == 0.0, "strtod underflows to zero");
    CHK_INT(errno, ERANGE, "total underflow sets ERANGE");
    /* ERANGE means precision was LOST, not merely "subnormal": this is the
     * smallest subnormal and it is exact, so glibc reports no error. */
    errno = 0; (void)strtod("0x1p-1074", 0);
    CHK_INT(errno, 0, "an EXACT subnormal does not set ERANGE");
    CHK(strtod("inf", 0) > 1e308, "strtod inf");
    CHK(strtod("nan", 0) != strtod("nan", 0), "strtod nan");
}

static void t_time2(void)
{
    struct tm tm; char b[128];
    time_t t = -1;
    gmtime_r(&t, &tm);
    CHK(tm.tm_year == 69 && tm.tm_mon == 11 && tm.tm_mday == 31, "gmtime before the epoch");
    CHK(tm.tm_hour == 23 && tm.tm_min == 59 && tm.tm_sec == 59, "gmtime pre-epoch time");
    CHK_INT(tm.tm_wday, 3, "gmtime pre-epoch wday");
    CHK_INT(tm.tm_yday, 364, "gmtime pre-epoch yday");
    strftime(b, sizeof b, "%G-%V", &tm);
    CHK_STR(b, "1970-01", "ISO week-based year crosses the calendar year");
    strftime(b, sizeof b, "%a %A %b %B %j %u %w", &tm);
    CHK_STR(b, "Wed Wednesday Dec December 365 3 3", "strftime names");

    /* mktime/timegm NORMALISE -- this is how date arithmetic is done in C. */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 70; tm.tm_mon = 13; tm.tm_mday = 40;
    tm.tm_hour = 25; tm.tm_min = 70; tm.tm_sec = 70;
    CHK_INT(timegm(&tm), 37678270L, "timegm of out-of-range fields");
    CHK(tm.tm_year == 71 && tm.tm_mon == 2 && tm.tm_mday == 13, "timegm normalised the date");
    CHK(tm.tm_hour == 2 && tm.tm_min == 11 && tm.tm_sec == 10, "timegm normalised the time");
    CHK_INT(tm.tm_wday, 6, "timegm normalised wday");
    CHK_INT(tm.tm_yday, 71, "timegm normalised yday");

    t = 1234567890;
    gmtime_r(&t, &tm);
    CHK_INT(timegm(&tm), 1234567890L, "timegm inverts gmtime");
    gmtime_r(&t, &tm);
    strftime(b, sizeof b, "%c", &tm);
    CHK_STR(b, "Fri Feb 13 23:31:30 2009", "strftime %c");
    strftime(b, sizeof b, "%F %T %p %I %C %e %D %R %U %W %V %G", &tm);
    CHK_STR(b, "2009-02-13 23:31:30 PM 11 20 13 02/13/09 23:31 06 06 07 2009", "strftime many");
    CHK_STR(asctime(&tm), "Fri Feb 13 23:31:30 2009\n", "asctime");
    CHK_INT((long)difftime(1000, 400), 600, "difftime");
    { struct timespec ts; CHK_INT(timespec_get(&ts, TIME_UTC), TIME_UTC, "timespec_get"); }
    /* The monotonic clock must not step backwards across a preemption. */
    { struct timespec a, c; clock_gettime(CLOCK_MONOTONIC, &a);
      for (volatile int i = 0; i < 300000; i++) { }
      clock_gettime(CLOCK_MONOTONIC, &c);
      CHK(c.tv_sec > a.tv_sec || (c.tv_sec == a.tv_sec && c.tv_nsec >= a.tv_nsec),
          "CLOCK_MONOTONIC never goes backwards"); }
}

static void t_wchar(void)
{
    wchar_t w[16]; char b[32];
    /* UTF-8 is this system's C-locale encoding, by design (see <wchar.h>). */
    CHK_INT(mbstowcs(w, "\xe4\xb8\xad\xe6\x96\x87", 16), 2, "mbstowcs CJK length");
    CHK(w[0] == 0x4E2D && w[1] == 0x6587, "mbstowcs CJK code points");
    CHK_INT(wcstombs(b, w, sizeof b), 6, "wcstombs byte count");
    CHK(memcmp(b, "\xe4\xb8\xad\xe6\x96\x87", 6) == 0, "wcstombs bytes");
    CHK_INT(mbstowcs(0, "\xf0\x9f\x98\x80", 0), 1, "mbstowcs counts without storing");
    CHK_INT(mblen("\xf0\x9f\x98\x80", 4), 4, "mblen of a 4-byte sequence");
    /* A strict decoder: overlong, surrogate and > U+10FFFF are all rejected. */
    CHK(mbstowcs(w, "\xc0\x80", 16) == (size_t)-1, "reject overlong");
    CHK(mbstowcs(w, "\xed\xa0\x80", 16) == (size_t)-1, "reject surrogate");
    CHK(mbstowcs(w, "\xf5\x80\x80\x80", 16) == (size_t)-1, "reject above U+10FFFF");
    CHK(mbstowcs(w, "\x80", 16) == (size_t)-1, "reject a stray continuation byte");
    /* Wide strings. */
    CHK_INT(wcslen(L"abc"), 3, "wcslen");
    CHK(wcscmp(L"abc", L"abc") == 0, "wcscmp eq");
    CHK(wcscmp(L"abc", L"abd") < 0, "wcscmp lt");
    CHK(wcschr(L"hello", L'l') == wcsstr(L"hello", L"llo"), "wcschr and wcsstr agree");
    CHK_INT(wcsspn(L"aabbc", L"ab"), 4, "wcsspn");
    { wchar_t d[8]; wcscpy(d, L"ab"); wcscat(d, L"cd");
      CHK(wcscmp(d, L"abcd") == 0, "wcscpy + wcscat"); }
    CHK(wcstod(L"2.5e3", 0) == 2500.0, "wcstod");
    CHK_INT(wcstol(L"ff", 0, 16), 255, "wcstol");
    { wchar_t o[32]; int r = swprintf(o, 32, L"%d-%s", 42, "ab");
      CHK_INT(r, 5, "swprintf return");
      CHK(wcscmp(o, L"42-ab") == 0, "swprintf output"); }
    /* wctype is ASCII in the C locale, deliberately (see <wctype.h>). */
    CHK(iswalpha(L'a') && !iswalpha(0x4E2D), "iswalpha is C-locale ASCII");
    CHK(iswdigit(L'7') && !iswdigit(L'x'), "iswdigit");
    CHK_INT(towupper(L'a'), L'A', "towupper");
    CHK(iswctype(L'5', wctype("digit")), "wctype / iswctype by name");
}

static void t_locale(void)
{
    CHK(setlocale(LC_ALL, "C") != 0, "setlocale C succeeds");
    CHK(setlocale(LC_ALL, "POSIX") != 0, "setlocale POSIX succeeds");
    CHK(setlocale(LC_ALL, "") != 0, "setlocale of the native environment succeeds");
    /* An unsupported locale must FAIL rather than silently pretend. */
    CHK(setlocale(LC_ALL, "de_DE.UTF-8") == 0, "setlocale of a real locale fails honestly");
    { struct lconv *l = localeconv();
      CHK_STR(l->decimal_point, ".", "localeconv decimal_point");
      CHK_STR(l->thousands_sep, "", "localeconv thousands_sep");
      CHK_INT(l->frac_digits, CHAR_MAX, "unavailable lconv fields are CHAR_MAX"); }
    CHK(strcoll("abc", "abd") < 0, "strcoll orders like strcmp in the C locale");
    { char x[16], y[16];
      strxfrm(x, "abc", sizeof x); strxfrm(y, "abd", sizeof y);
      CHK(strcmp(x, y) < 0, "strxfrm preserves collation order"); }
}

static volatile int g_sig;
static void on_sig(int s) { g_sig = s; }

static void t_signal(void)
{
    /* raise() is the one signal path that genuinely works here: it is
     * synchronous by definition. Asynchronous delivery does not exist. */
    CHK(signal(SIGUSR1, on_sig) != SIG_ERR, "signal installs a handler");
    g_sig = 0;
    CHK_INT(raise(SIGUSR1), 0, "raise returns 0");
    CHK_INT(g_sig, SIGUSR1, "raise actually ran the handler");
    CHK(signal(SIGKILL, on_sig) == SIG_ERR, "SIGKILL is uncatchable");
    CHK(signal(SIGUSR2, SIG_IGN) != SIG_ERR, "SIG_IGN installs");
    CHK_INT(raise(SIGUSR2), 0, "an ignored signal returns 0");
    { sigset_t set; sigemptyset(&set); sigaddset(&set, SIGINT);
      CHK_INT(sigismember(&set, SIGINT), 1, "sigaddset then sigismember");
      CHK_INT(sigismember(&set, SIGTERM), 0, "sigismember negative"); }
    CHK_STR(strsignal(SIGSEGV), "Segmentation fault", "strsignal");
}

static void t_strings2(void)
{
    char b[64];
    /* strerror returned the word "error" for every code; each must be distinct. */
    CHK_STR(strerror(ENOENT), "No such file or directory", "strerror ENOENT");
    CHK_STR(strerror(EINVAL), "Invalid argument", "strerror EINVAL");
    CHK_STR(strerror(ERANGE), "Numerical result out of range", "strerror ERANGE");
    CHK(strcmp(strerror(EIO), strerror(EBADF)) != 0, "strerror distinguishes codes");
    CHK(strstr(strerror(31337), "Unknown error") != 0, "strerror names an unknown code");
    { char eb[64]; CHK_INT(strerror_r(ENOENT, eb, sizeof eb), 0, "strerror_r");
      CHK_STR(eb, "No such file or directory", "strerror_r text"); }
    CHK(strverscmp("file9", "file10") < 0, "strverscmp orders numerically");
    { char *p = stpcpy(b, "abc"); CHK(*p == 0 && p == b + 3, "stpcpy returns the end"); }
    { const char *s = "abc"; CHK(strchrnul(s, 'z') == s + 3, "strchrnul lands on the NUL"); }
    { const char *s = "abcabc"; CHK(memrchr(s, 'b', 6) == s + 4, "memrchr finds the last"); }
    CHK(strcasestr("Hello World", "WORLD") != 0, "strcasestr");
    { char d[8] = "ab"; CHK(memccpy(d, "xyz", 'y', 3) == d + 2, "memccpy stops after the byte"); }
}

static void t_alloc(void)
{
    /* The allocator is c/apps/libc/src/malloc.c, owned separately. These assert
     * the CONTRACT this library is written against and documents. */
    CHK(malloc((size_t)-1) == 0, "malloc(SIZE_MAX) returns NULL, not a wrapped size");
    { void *p = malloc(0); free(p); checks++; }     /* must not crash either way */
    { char *p = malloc(64);
      CHK(p != 0, "malloc 64");
      if (p) {
        memset(p, 'x', 64);
        char *q = realloc(p, 16);
        CHK(q != 0 && q[0] == 'x' && q[15] == 'x', "realloc shrink preserves content");
        char *r = realloc(q, 256);
        CHK(r != 0 && r[0] == 'x', "realloc grow preserves content");
        free(r);
      } }
    /* aligned_alloc REFUSES what it cannot do, rather than returning an interior
     * pointer that free() would silently leak (free of an unrecognised pointer
     * is a deliberate no-op in this allocator). */
    { void *p = aligned_alloc(16, 32);
      CHK(p != 0, "aligned_alloc 16 succeeds");
      CHK(((unsigned long)p & 15) == 0, "aligned_alloc result really is 16-aligned");
      free(p); }
    errno = 0;
    CHK(aligned_alloc(64, 128) == 0, "aligned_alloc above malloc's guarantee fails");
    CHK_INT(errno, EINVAL, "aligned_alloc failure sets EINVAL");
    { void *p = 0; CHK_INT(posix_memalign(&p, 64, 128), EINVAL, "posix_memalign refuses 64"); }
    { void *p = 0; CHK_INT(posix_memalign(&p, 16, 128), 0, "posix_memalign 16 succeeds");
      CHK(p != 0, "posix_memalign stored a pointer"); free(p); }
    { void *p = calloc(8, 8); CHK(p != 0, "calloc");
      if (p) { int z = 1; for (int i = 0; i < 64; i++) if (((char *)p)[i]) z = 0;
               CHK(z, "calloc zeroes"); free(p); } }
    CHK(calloc((size_t)-1, 2) == 0, "calloc overflow returns NULL");
    /* strdup must set ENOMEM on failure, and must not be the thing that fails. */
    { char *d = strdup("hello"); CHK(d && !strcmp(d, "hello"), "strdup"); free(d); }
}

static int cmp_ctx(const void *a, const void *b, void *arg)
{ int s = *(int *)arg; int x = *(const int *)a, y = *(const int *)b;
  return s * (x < y ? -1 : (x > y ? 1 : 0)); }

static void t_stdlib2(void)
{
    int arr[7] = { 5, 2, 9, 1, 7, 3, 8 };
    int desc = -1, asc = 1;
    qsort_r(arr, 7, sizeof(int), cmp_ctx, &desc);
    CHK(arr[0] == 9 && arr[6] == 1, "qsort_r passes the context through");
    /* Introsort: already-sorted input is the classic quicksort blow-up, and the
     * comparator here can be a page's JavaScript. It must stay O(n log n). */
    { static int big[4096];
      for (int i = 0; i < 4096; i++) big[i] = i;
      qsort_r(big, 4096, sizeof(int), cmp_ctx, &asc);
      CHK(big[0] == 0 && big[4095] == 4095, "qsort of already-sorted input");
      for (int i = 0; i < 4096; i++) big[i] = 4095 - i;
      qsort_r(big, 4096, sizeof(int), cmp_ctx, &asc);
      CHK(big[0] == 0 && big[4095] == 4095, "qsort of reverse-sorted input"); }
    /* Environment: empty at startup (crt0 does not hand over envp), then
     * internally coherent -- which is what getenv-or-default code needs. */
    CHK(getenv("LIBCTEST_X") == 0, "getenv of an unset name");
    CHK_INT(setenv("LIBCTEST_X", "hello", 1), 0, "setenv");
    CHK_STR(getenv("LIBCTEST_X"), "hello", "getenv after setenv");
    CHK_INT(setenv("LIBCTEST_X", "other", 0), 0, "setenv without overwrite returns 0");
    CHK_STR(getenv("LIBCTEST_X"), "hello", "setenv without overwrite kept the old value");
    CHK_INT(setenv("LIBCTEST_X", "other", 1), 0, "setenv with overwrite");
    CHK_STR(getenv("LIBCTEST_X"), "other", "setenv overwrote");
    CHK_INT(unsetenv("LIBCTEST_X"), 0, "unsetenv");
    CHK(getenv("LIBCTEST_X") == 0, "getenv after unsetenv");
    { imaxdiv_t d = imaxdiv(17, 5); CHK(d.quot == 3 && d.rem == 2, "imaxdiv"); }
    CHK_INT((long)imaxabs((intmax_t)-9), 9, "imaxabs");
    CHK_INT((long)strtoimax("123", 0, 10), 123, "strtoimax");
    /* fenv: the exception flags are real MXCSR bits now, not stubs returning 0. */
    feclearexcept(FE_ALL_EXCEPT);
    CHK_INT(fetestexcept(FE_ALL_EXCEPT), 0, "feclearexcept clears");
    feraiseexcept(FE_OVERFLOW);
    CHK(fetestexcept(FE_OVERFLOW) != 0, "feraiseexcept is observable");
    feclearexcept(FE_ALL_EXCEPT);
    { volatile double a = 1.0, b = 3.0, c = a / b; (void)c;
      CHK(fetestexcept(FE_INEXACT) != 0, "an inexact divide really sets FE_INEXACT"); }
    feclearexcept(FE_ALL_EXCEPT);
    CHK_INT(fegetround(), FE_TONEAREST, "default rounding is to-nearest");
}

static void t_scanf2(void)
{
    int a, b; char s1[32];
    /* The scanset, which did not exist at all. */
    CHK(sscanf("abc123", "%[a-z]%d", s1, &a) == 2 && !strcmp(s1, "abc") && a == 123,
        "sscanf %[a-z]");
    CHK(sscanf("123abc", "%[^a-z]", s1) == 1 && !strcmp(s1, "123"), "sscanf negated scanset");
    CHK(sscanf("]x", "%[]]", s1) == 1 && !strcmp(s1, "]"), "sscanf leading ] is literal");
    /* EOF vs 0 -- the distinction every scanf read loop is built on. */
    CHK_INT(sscanf("", "%d", &a), EOF, "EOF before any conversion");
    CHK_INT(sscanf("abc", "%d", &a), 0, "a matching failure returns 0, not EOF");
    /* C's rule: consume the longest PREFIX of a match, then require a match. */
    { double d; CHK_INT(sscanf("1e", "%lf", &d), 0, "'1e' under %lf is a matching failure"); }
    { void *p = 0; CHK(sscanf("0x1234", "%p", &p) == 1 && p == (void *)0x1234, "sscanf %p"); }
    { long long ll = 0; CHK(sscanf("-9223372036854775808", "%lld", &ll) == 1 && ll == LLONG_MIN,
                            "sscanf LLONG_MIN"); }
    { signed char c = 0; CHK(sscanf("300", "%hhd", &c) == 1 && c == 44, "sscanf %hhd truncates"); }
    CHK(sscanf("12 34", "%d%*c%d", &a, &b) == 2 && a == 12 && b == 34, "sscanf suppression");
}

static void t_stdio2(void)
{
    const char *p = "/libctest2.tmp";
    FILE *f = fopen(p, "w");
    CHK(f != 0, "fopen w");
    if (!f) return;
    /* Real buffering: setvbuf must be honoured and fflush must actually flush. */
    CHK_INT(setvbuf(f, 0, _IOFBF, 4096), 0, "setvbuf _IOFBF");
    fputs("alpha\nbeta\ngamma\n", f);
    CHK_INT(ftell(f), 17, "ftell accounts for buffered bytes");
    CHK_INT(fflush(f), 0, "fflush");
    CHK_INT(fclose(f), 0, "fclose");

    f = fopen(p, "r");
    CHK(f != 0, "fopen r");
    if (f) {
        char *line = 0; size_t cap = 0;
        CHK_INT(getline(&line, &cap, f), 6, "getline length");
        CHK_STR(line, "alpha\n", "getline content");
        CHK_INT(getline(&line, &cap, f), 5, "getline second line");
        free(line);
        fpos_t pos;
        CHK_INT(fgetpos(f, &pos), 0, "fgetpos");
        { char w[8]; CHK(fgets(w, sizeof w, f) != 0, "fgets");
          CHK_STR(w, "gamma\n", "fgets content"); }
        CHK_INT(fsetpos(f, &pos), 0, "fsetpos");
        { char w[8]; CHK(fgets(w, sizeof w, f) != 0, "fgets after fsetpos");
          CHK_STR(w, "gamma\n", "fsetpos returned to the right place"); }
        fclose(f);
    }
    /* fscanf, which did not exist. */
    f = fopen(p, "r");
    if (f) { char w1[16], w2[16];
        CHK(fscanf(f, "%s %s", w1, w2) == 2 && !strcmp(w1, "alpha") && !strcmp(w2, "beta"),
            "fscanf");
        fclose(f); }
    /* rename, which did not exist. */
    CHK_INT(rename(p, "/libctest3.tmp"), 0, "rename");
    { FILE *g = fopen(p, "r"); CHK(g == 0, "rename removed the old name"); if (g) fclose(g); }
    { FILE *g = fopen("/libctest3.tmp", "r"); CHK(g != 0, "rename created the new name");
      if (g) fclose(g); }
    CHK_INT(remove("/libctest3.tmp"), 0, "remove");
    /* snprintf's truncation contract. */
    { char b[4]; int n = snprintf(b, sizeof b, "%d", 123456);
      CHK_INT(n, 6, "snprintf returns the would-be length");
      CHK_STR(b, "123", "snprintf truncates and terminates"); }
    CHK_INT(snprintf(0, 0, "%s=%d", "key", 42), 6, "snprintf(NULL,0) measures");
    { char *o = 0; int n = asprintf(&o, "%s/%03d", "path", 7);
      CHK(n == 8 && o && !strcmp(o, "path/007"), "asprintf sizes exactly"); free(o); }
}

static void t_dirstat(void)
{
    struct stat st;
    CHK_INT(stat("/bin", &st), 0, "stat /bin");
    CHK(S_ISDIR(st.st_mode), "/bin is a directory");
    CHK_INT(stat("/bin/libctest", &st), 0, "stat self");
    CHK(S_ISREG(st.st_mode), "/bin/libctest is a regular file");
    CHK(st.st_size > 1000, "stat reports a real size");
    errno = 0;
    CHK_INT(stat("/definitely-not-here", &st), -1, "stat of a missing path fails");
    CHK_INT(errno, ENOENT, "stat sets ENOENT");
    CHK_INT(access("/bin", F_OK), 0, "access on a directory");
    CHK_INT(access("/definitely-not-here", F_OK), -1, "access on a missing path");

    DIR *d = opendir("/bin");
    CHK(d != 0, "opendir /bin");
    if (d) {
        int n = 0, found = 0;
        struct dirent *e;
        while ((e = readdir(d))) { n++; if (!strcmp(e->d_name, "libctest")) found = 1; }
        CHK(n > 0, "readdir returned entries");
        CHK(found, "readdir found libctest in /bin");
        rewinddir(d);
        CHK(readdir(d) != 0, "rewinddir restarts the scan");
        CHK_INT(closedir(d), 0, "closedir");
    }
    CHK(opendir("/definitely-not-here") == 0, "opendir of a missing path fails");
    { DIR *r = opendir("/"); CHK(r != 0, "opendir /"); if (r) closedir(r); }
    { int fd = open("/bin/libctest", O_RDONLY);
      CHK(fd >= 0, "open self");
      if (fd >= 0) { struct stat s2; CHK_INT(fstat(fd, &s2), 0, "fstat");
                     CHK(s2.st_size == st.st_size, "fstat size agrees with stat");
                     close(fd); } }
}

/* atexit and exit()'s stream flush, proved in a CHILD process -- a process
 * cannot observe its own exit. Also exercises fork/waitpid and <sys/wait.h>. */
static void ax_b(void) { FILE *f = fopen("/libctest_ax.tmp", "a"); if (f) { fputs("B", f); fclose(f); } }
static void ax_a(void) { FILE *f = fopen("/libctest_ax.tmp", "a"); if (f) { fputs("A", f); fclose(f); } }

static void t_atexit(void)
{
    remove("/libctest_ax.tmp");
    int pid = fork();
    if (pid == 0) {
        atexit(ax_b);            /* handlers run in REVERSE registration order, */
        atexit(ax_a);            /* so the file must read "AB". */
        exit(0);
    }
    CHK(pid > 0, "fork for the atexit test");
    if (pid > 0) {
        int status = 0;
        CHK(waitpid(pid, &status, 0) == pid, "waitpid reaped the child");
        CHK(WIFEXITED(status), "child exited normally");
        CHK_INT(WEXITSTATUS(status), 0, "child exit status");
        FILE *f = fopen("/libctest_ax.tmp", "r");
        CHK(f != 0, "the atexit handlers ran at all");
        if (f) { char b[8]; size_t n = fread(b, 1, 4, f); b[n] = 0; fclose(f);
                 CHK_STR(b, "AB", "atexit handlers ran in reverse order"); }
        remove("/libctest_ax.tmp");
    }
}
