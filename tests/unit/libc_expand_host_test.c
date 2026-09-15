/* Host gate for the pure-computation half of the libc expansion.  The entropy
 * and UTF conversion halves need the real Logit syscall/mbstate_t and live in
 * libctest's guest battery; everything here runs without QEMU. */
#include <argz.h>
#include <alloca.h>
#include <byteswap.h>
#include <envz.h>
#include <libintl.h>
#include <malloc.h>
#include <memory.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <locale.h>
#include <endian.h>
#include <stdckdint.h>
#include <stdbit.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/errno.h>
#include <sys/limits.h>
#include <xlocale.h>
#include <stdio.h>

static int checks, fails;
#ifdef LIBC_EXPAND_NEGATIVE_CONTROL
static int argz_control_failed;
#endif
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; printf("FAIL: %s\n", m); } } while (0)

struct qnode {
    int value;
    SLIST_ENTRY(qnode) slink;
    LIST_ENTRY(qnode) link;
    STAILQ_ENTRY(qnode) stlink;
    TAILQ_ENTRY(qnode) tlink;
};
SLIST_HEAD(slist_head, qnode);
LIST_HEAD(list_head, qnode);
STAILQ_HEAD(stailq_head, qnode);
TAILQ_HEAD(tailq_head, qnode);

static void test_argz(void)
{
    char *z = NULL; size_t n = 0;
    CHECK(argz_create_sep(":alpha::beta:", ':', &z, &n) == 0, "argz_create_sep");
    CHECK(n == 11 && argz_count(z, n) == 2, "argz separator collapse");
    char *av[8]; argz_extract(z, n, av);
    CHECK(!strcmp(av[0], "alpha") && !strcmp(av[1], "beta") && !av[2], "argz_extract");
    CHECK(argz_insert(&z, &n, av[1], "middle") == 0, "argz_insert");
    CHECK(argz_add(&z, &n, "tail") == 0 && argz_count(z, n) == 4, "argz_add");
    char *middle = argz_next(z, n, argz_next(z, n, NULL));
    argz_delete(&z, &n, middle);
    CHECK(argz_count(z, n) == 3, "argz_delete");
    argz_stringify(z, n, ',');
    CHECK(!strcmp(z, "alpha,beta,tail"), "argz_stringify");
    free(z);

    char *src[] = { (char *)"banana", (char *)"ananas", NULL };
    unsigned int replaced = 0;
    CHECK(argz_create(src, &z, &n) == 0, "argz_create");
    CHECK(argz_replace(&z, &n, "ana", "X", &replaced) == 0, "argz_replace result");
    argz_extract(z, n, av);
    int multi_ok = replaced == 2 && !strcmp(av[0], "bXna") && !strcmp(av[1], "Xnas");
    CHECK(multi_ok, "argz_replace multi-hit");
#ifdef LIBC_EXPAND_NEGATIVE_CONTROL
    argz_control_failed = !multi_ok;
#endif
    free(z);
}

static void test_envz(void)
{
    char *z = NULL; size_t n = 0;
    CHECK(envz_add(&z, &n, "A", "one") == 0, "envz_add value");
    CHECK(envz_add(&z, &n, "B", NULL) == 0, "envz_add null");
    CHECK(!strcmp(envz_get(z, n, "A"), "one"), "envz_get value");
    CHECK(envz_entry(z, n, "B") && !envz_get(z, n, "B"), "envz null entry");
    CHECK(envz_add(&z, &n, "A", "two=three") == 0 &&
          !strcmp(envz_get(z, n, "A"), "two=three"), "envz replace and embedded equals");

    char *z2 = NULL; size_t n2 = 0;
    envz_add(&z2, &n2, "A", "override"); envz_add(&z2, &n2, "C", "new");
    CHECK(envz_merge(&z, &n, z2, n2, 0) == 0 && !strcmp(envz_get(z, n, "A"), "two=three"),
          "envz_merge no override");
    CHECK(envz_merge(&z, &n, z2, n2, 1) == 0 && !strcmp(envz_get(z, n, "A"), "override"),
          "envz_merge override");
    CHECK(envz_merge(&z, &n, z, n, 1) == 0 &&
          !strcmp(envz_get(z, n, "A"), "override") && envz_entry(z, n, "C"),
          "envz_merge aliased source");
    envz_strip(&z, &n);
    CHECK(!envz_entry(z, n, "B") && envz_entry(z, n, "C"), "envz_strip");
    envz_remove(&z, &n, "C"); CHECK(!envz_entry(z, n, "C"), "envz_remove");
    free(z2); free(z);
}

static void test_headers(void)
{
    unsigned int out;
    CHECK(!ckd_add(&out, 10u, 20u) && out == 30, "ckd_add success");
    CHECK(ckd_add(&out, ~0u, 1u), "ckd_add overflow");
    CHECK(stdc_leading_zeros((unsigned char)1) == 7, "stdbit leading zeros");
    CHECK(stdc_trailing_zeros(16u) == 4 && stdc_count_ones(0xf0u) == 4, "stdbit count");
    CHECK(stdc_has_single_bit(32ul) && stdc_bit_floor(39ul) == 32, "stdbit floor");
    CHECK(stdc_bit_ceil(39ull) == 64, "stdbit ceil");
    CHECK(stdc_bit_ceil(~0ull) == 0, "stdbit ceil overflow is defined");
    CHECK(bswap_16(0x1234u) == 0x3412u &&
          bswap_64(0x0102030405060708ULL) == 0x0807060504030201ULL,
          "byteswap typed helpers");
    CHECK(be32toh(0x12345678u) == 0x78563412u, "endian conversion");
    dev_t d = makedev(0x1234, 0x56789);
    CHECK(major(d) == 0x1234 && minor(d) == 0x56789, "major/minor/makedev");
    CHECK(howmany(10, 4) == 3 && roundup(10, 4) == 12 && nitems("abc") == 4,
          "sys/param macros");
    struct timeval a = {1, 900000}, b = {2, 200000}, c;
    timeradd(&a, &b, &c); CHECK(c.tv_sec == 4 && c.tv_usec == 100000, "timeradd carry");
    timersub(&b, &a, &c); CHECK(c.tv_sec == 0 && c.tv_usec == 300000, "timersub borrow");
    char *stack = alloca(4); memcpy(stack, "ok", 3);
    CHECK(!strcmp(stack, "ok"), "alloca compatibility header");
}

static void test_queues(void)
{
    struct qnode a = {1}, b = {2}, c = {3}, *p, *tmp;
    struct slist_head sh; SLIST_INIT(&sh);
    SLIST_INSERT_HEAD(&sh, &b, slink); SLIST_INSERT_HEAD(&sh, &a, slink);
    SLIST_INSERT_AFTER(&b, &c, slink);
    int sum = 0; SLIST_FOREACH(p, &sh, slink) sum = sum * 10 + p->value;
    CHECK(sum == 123, "SLIST insertion/iteration");
    SLIST_FOREACH_SAFE(p, &sh, slink, tmp) SLIST_REMOVE(&sh, p, qnode, slink);
    CHECK(SLIST_EMPTY(&sh), "SLIST safe removal");

    struct list_head lh; LIST_INIT(&lh);
    LIST_INSERT_HEAD(&lh, &b, link); LIST_INSERT_BEFORE(&b, &a, link);
    LIST_INSERT_AFTER(&b, &c, link); LIST_REMOVE(&b, link);
    CHECK(LIST_FIRST(&lh) == &a && LIST_NEXT(&a, link) == &c, "LIST before/remove");

    struct stailq_head sq, sq2; STAILQ_INIT(&sq); STAILQ_INIT(&sq2);
    STAILQ_INSERT_TAIL(&sq, &a, stlink); STAILQ_INSERT_TAIL(&sq, &b, stlink);
    STAILQ_INSERT_TAIL(&sq2, &c, stlink); STAILQ_CONCAT(&sq, &sq2);
    CHECK(STAILQ_LAST(&sq, qnode, stlink) == &c && STAILQ_EMPTY(&sq2), "STAILQ concat/last");

    struct tailq_head tq; TAILQ_INIT(&tq);
    TAILQ_INSERT_HEAD(&tq, &b, tlink); TAILQ_INSERT_BEFORE(&b, &a, tlink);
    TAILQ_INSERT_AFTER(&tq, &b, &c, tlink);
    CHECK(TAILQ_FIRST(&tq) == &a && TAILQ_LAST(&tq, tailq_head) == &c &&
          TAILQ_PREV(&c, tailq_head, tlink) == &b, "TAILQ head/last/prev");
    TAILQ_REMOVE(&tq, &b, tlink);
    CHECK(TAILQ_NEXT(&a, tlink) == &c && TAILQ_PREV(&c, tailq_head, tlink) == &a,
          "TAILQ remove relinks both ways");
}

static void test_functions(void)
{
    CHECK(!strcmp(gettext("hello"), "hello") && !strcmp(ngettext("one", "many", 2), "many"),
          "gettext fallback/plural");
    CHECK(textdomain("demo") && !strcmp(textdomain(NULL), "demo"), "textdomain state");
    CHECK(!strcmp(bindtextdomain("demo", "/locale"), "/locale") &&
          !strcmp(bindtextdomain("demo", NULL), "/locale"), "bindtextdomain query");
    CHECK(!strcmp(bind_textdomain_codeset("demo", "UTF-8"), "UTF-8"), "gettext codeset");

    CHECK(timingsafe_bcmp("abc", "abc", 3) == 0 && timingsafe_bcmp("abc", "abd", 3) != 0,
          "timingsafe_bcmp");
    CHECK(timingsafe_memcmp("abc", "abd", 3) < 0 && timingsafe_memcmp("abd", "abc", 3) > 0,
          "timingsafe_memcmp ordering");
    CHECK(strnstr("abcdef", "cde", 5) != NULL && strnstr("abcdef", "def", 5) == NULL,
          "strnstr bound");
    CHECK(strnstr("a", "a-long-needle", 64) == NULL, "strnstr stops at haystack nul");
    char secret[8] = "secret"; explicit_bzero(secret, sizeof secret);
    CHECK(secret[0] == 0 && secret[6] == 0, "explicit_bzero");
    const char *err;
    errno = EDOM;
    CHECK(strtonum("42", 0, 100, &err) == 42 && !err && errno == EDOM,
          "strtonum success preserves errno");
    CHECK(strtonum("101", 0, 100, &err) == 0 && err && !strcmp(err, "too large"),
          "strtonum bounds");
    long word = 0x12345; CHECK(a64l(l64a(word)) == word, "a64l/l64a roundtrip");

    struct drand48_data st = {0}; double rd;
    srand48(1234); srand48_r(1234, &st);
    CHECK(drand48_r(&st, &rd) == 0 && drand48() == rd, "drand48 reentrant sequence");
    long lr; CHECK(lrand48_r(&st, &lr) == 0 && lr >= 0 && lr <= 0x7fffffffL,
                   "lrand48 range");

    locale_t loc = newlocale(LC_ALL_MASK, "C", NULL);
    CHECK(loc && isalpha_l('A', loc) && !isalpha_l('7', loc), "newlocale/ctype_l");
    CHECK(!strcasecmp_l("AbC", "aBc", loc), "strcasecmp_l");
    CHECK(uselocale(loc) == LC_GLOBAL_LOCALE && uselocale(NULL) == loc, "uselocale state");
    freelocale(loc);
}

int main(void)
{
    test_argz(); test_envz(); test_headers(); test_queues(); test_functions();
#ifdef LIBC_EXPAND_NEGATIVE_CONTROL
    if (argz_control_failed)
        printf("EXPECTED_CONTROL_FAILURE argz_replace multi-hit\n");
#endif
    if (!fails) printf("LIBC_EXPAND_HOST_OK %d/%d\n", checks, checks);
    return fails != 0;
}
