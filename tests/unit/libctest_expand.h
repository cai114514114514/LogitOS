/* Guest-only coverage for the libc expansion.  This is included by
 * libctest_main.c after its CHK macros and globals, so these checks exercise
 * the same objects that ship in /bin/as and the browser. */
#include <argz.h>
#include <envz.h>
#include <libintl.h>
#include <uchar.h>
#include <sys/random.h>
#include <stdckdint.h>
#include <stdbit.h>
#include <endian.h>
#include <pthread.h>
#include <threads.h>

static void t_expand_argz_envz(void)
{
    char *z = 0; size_t n = 0; char *v[8];
    CHK_INT(argz_create_sep("::one:two::", ':', &z, &n), 0, "argz_create_sep guest");
    CHK_INT(argz_count(z, n), 2, "argz_count guest");
    argz_extract(z, n, v);
    CHK_STR(v[0], "one", "argz_extract first");
    CHK_STR(v[1], "two", "argz_extract second");
    CHK(argz_insert(&z, &n, v[1], "middle") == 0, "argz_insert guest");
    unsigned int replaced = 0;
    CHK(argz_replace(&z, &n, "d", "DD", &replaced) == 0 && replaced == 2,
        "argz_replace guest multi-hit");
    argz_extract(z, n, v);
    CHK_STR(v[1], "miDDDDle", "argz_replace preserves vector");
    free(z);

    z = 0; n = 0;
    CHK_INT(envz_add(&z, &n, "HOME", "/home/logit"), 0, "envz_add guest");
    CHK_INT(envz_add(&z, &n, "EMPTY", ""), 0, "envz empty value guest");
    CHK_INT(envz_add(&z, &n, "NULL", 0), 0, "envz null value guest");
    CHK_STR(envz_get(z, n, "HOME"), "/home/logit", "envz_get guest");
    CHK(envz_get(z, n, "EMPTY") && !*envz_get(z, n, "EMPTY"), "envz empty distinct");
    CHK(envz_entry(z, n, "NULL") && !envz_get(z, n, "NULL"), "envz null distinct");
    envz_strip(&z, &n);
    CHK(!envz_entry(z, n, "NULL"), "envz_strip guest");
    free(z);
}

static void t_expand_entropy(void)
{
    static unsigned char a[5000], b[5000];
    CHK_INT(getrandom(a, sizeof a, 0), sizeof a, "getrandom loops over kernel cap");
    CHK_INT(getrandom(b, sizeof b, GRND_NONBLOCK), sizeof b, "getrandom nonblock");
    CHK_INT(getrandom(b, 1, GRND_INSECURE), 1, "getrandom insecure compatibility flag");
    int any = 0, differs = 0;
    for (size_t i = 0; i < sizeof a; i++) { any |= a[i]; differs |= a[i] ^ b[i]; }
    CHK(any != 0, "getrandom output not all zero");
    CHK(differs != 0, "successive getrandom output differs");
    errno = 0;
    CHK_INT(getrandom(a, 1, 0x80000000u), -1, "getrandom rejects unknown flags");
    CHK_INT(errno, EINVAL, "getrandom unknown flag errno");
    errno = 0;
    CHK_INT(getrandom((void *)(uintptr_t)0xffff800000000000ULL, 1, 0), -1,
            "getrandom rejects unwritable user range");
    CHK_INT(errno, EFAULT, "getrandom bad range errno");
    CHK_INT(getentropy(a, 256), 0, "getentropy 256-byte ceiling");
    errno = 0;
    CHK_INT(getentropy(a, 257), -1, "getentropy rejects oversized request");
    CHK_INT(errno, EIO, "getentropy oversized errno");
    for (int i = 0; i < 128; i++)
        CHK(arc4random_uniform(17) < 17, "arc4random_uniform bound");
}

static void t_expand_uchar(void)
{
    mbstate_t st = {0}; char32_t c32 = 0; char16_t hi = 0, lo = 0;
    CHK_INT(mbrtoc32(&c32, "\xf0\x9f\x98\x80", 4, &st), 4, "mbrtoc32 emoji bytes");
    CHK(c32 == 0x1f600u, "mbrtoc32 emoji value");
    memset(&st, 0, sizeof st);
    CHK_INT(mbrtoc16(&hi, "\xf0\x9f\x98\x80", 4, &st), 4, "mbrtoc16 high surrogate");
    CHK(hi == 0xd83du, "mbrtoc16 high value");
    CHK(mbrtoc16(&lo, "x", 1, &st) == (size_t)-3 && lo == 0xde00u,
        "mbrtoc16 pending low surrogate");
    CHK_INT(mbrtoc16(&lo, "x", 1, &st), 1, "mbrtoc16 does not consume on -3");
    CHK(lo == 'x', "mbrtoc16 resumed input");

    char out[8] = {0}; memset(&st, 0, sizeof st);
    CHK_INT(c16rtomb(out, 0xd83d, &st), 0, "c16rtomb stores high surrogate");
    CHK_INT(c16rtomb(out, 0xde00, &st), 4, "c16rtomb completes pair");
    CHK(memcmp(out, "\xf0\x9f\x98\x80", 4) == 0, "c16rtomb emoji bytes");
    errno = 0; memset(&st, 0, sizeof st);
    CHK(c16rtomb(out, 0xde00, &st) == (size_t)-1, "c16rtomb rejects lone low surrogate");
    CHK_INT(errno, EILSEQ, "c16rtomb lone low errno");
}

static void t_expand_compat(void)
{
    unsigned int sum;
    CHK(!ckd_add(&sum, 20u, 22u) && sum == 42, "ckd_add guest");
    CHK(ckd_mul(&sum, ~0u, 2u), "ckd_mul overflow guest");
    CHK_INT(stdc_leading_zeros((unsigned char)1), 7, "stdbit leading guest");
    CHK_INT(stdc_count_ones(0xf0f0u), 8, "stdbit popcount guest");
    CHK(stdc_bit_floor(100ul) == 64 && stdc_bit_ceil(100ul) == 128,
        "stdbit floor/ceil guest");
    CHK(be64toh(0x0102030405060708ULL) == 0x0807060504030201ULL,
        "endian 64 guest");
    CHK_STR(gettext("untranslated"), "untranslated", "gettext guest fallback");
    CHK_STR(ngettext("one", "many", 0), "many", "ngettext zero plural");
    char wipe[16] = "credential";
    explicit_bzero(wipe, sizeof wipe);
    CHK(wipe[0] == 0 && wipe[15] == 0, "explicit_bzero guest");
    CHK(timingsafe_bcmp("token", "token", 5) == 0, "timingsafe_bcmp equal guest");
    CHK(timingsafe_memcmp("token", "tokeo", 5) < 0, "timingsafe_memcmp order guest");
    const char *err = 0;
    CHK(strtonum("-8", -10, 10, &err) == -8 && !err, "strtonum guest");
    CHK(strtonum("-11", -10, 10, &err) == 0 && err && !strcmp(err, "too small"),
        "strtonum guest bound");
}

static pthread_spinlock_t expand_spin;
static int expand_spin_total;

static void *expand_spin_worker(void *unused)
{
    (void)unused;
    for (int i = 0; i < 750; i++) {
        pthread_spin_lock(&expand_spin);
        expand_spin_total++;
        pthread_spin_unlock(&expand_spin);
    }
    return 0;
}

static pthread_rwlock_t expand_rw;
static volatile int expand_rw_started;
static int expand_rw_value;

static void *expand_rw_writer(void *unused)
{
    (void)unused;
    __atomic_store_n(&expand_rw_started, 1, __ATOMIC_SEQ_CST);
    pthread_rwlock_wrlock(&expand_rw);
    expand_rw_value = 77;
    pthread_rwlock_unlock(&expand_rw);
    return 0;
}

static pthread_barrier_t expand_barrier;
static volatile int expand_before[2], expand_after[2], expand_serial[2];

static void *expand_barrier_worker(void *unused)
{
    (void)unused;
    for (int round = 0; round < 2; round++) {
        __atomic_add_fetch(&expand_before[round], 1, __ATOMIC_SEQ_CST);
        int rc = pthread_barrier_wait(&expand_barrier);
        if (rc == PTHREAD_BARRIER_SERIAL_THREAD)
            __atomic_add_fetch(&expand_serial[round], 1, __ATOMIC_SEQ_CST);
        __atomic_add_fetch(&expand_after[round], 1, __ATOMIC_SEQ_CST);
    }
    return 0;
}

static void t_expand_threads(void)
{
    pthread_t th[3];
    CHK_INT(pthread_spin_init(&expand_spin, PTHREAD_PROCESS_PRIVATE), 0,
            "pthread_spin_init guest");
    CHK_INT(pthread_spin_lock(&expand_spin), 0, "pthread_spin_lock guest");
    CHK_INT(pthread_spin_trylock(&expand_spin), EBUSY, "pthread_spin_trylock busy");
    CHK_INT(pthread_spin_unlock(&expand_spin), 0, "pthread_spin_unlock guest");
    expand_spin_total = 0;
    for (int i = 0; i < 3; i++)
        CHK_INT(pthread_create(&th[i], 0, expand_spin_worker, 0), 0,
                "pthread_create spin worker");
    for (int i = 0; i < 3; i++)
        CHK_INT(pthread_join(th[i], 0), 0, "pthread_join spin worker");
    CHK_INT(expand_spin_total, 2250, "pthread spin mutual exclusion");
    CHK_INT(pthread_spin_destroy(&expand_spin), 0, "pthread_spin_destroy guest");

    CHK_INT(pthread_rwlock_init(&expand_rw, 0), 0, "pthread_rwlock_init guest");
    CHK_INT(pthread_rwlock_rdlock(&expand_rw), 0, "pthread_rwlock_rdlock guest");
    CHK_INT(pthread_rwlock_trywrlock(&expand_rw), EBUSY,
            "pthread rw writer excluded by reader");
    expand_rw_started = 0;
    expand_rw_value = 0;
    CHK_INT(pthread_create(&th[0], 0, expand_rw_writer, 0), 0,
            "pthread_create rw writer");
    while (!__atomic_load_n(&expand_rw_started, __ATOMIC_SEQ_CST)) sched_yield();
    for (int i = 0; i < 64; i++) sched_yield();
    CHK_INT(expand_rw_value, 0, "pthread rw writer blocks behind live reader");
    CHK_INT(pthread_rwlock_unlock(&expand_rw), 0, "pthread rw reader unlock");
    CHK_INT(pthread_join(th[0], 0), 0, "pthread_join rw writer");
    CHK_INT(expand_rw_value, 77, "pthread rw writer proceeds after readers");
    CHK_INT(pthread_rwlock_wrlock(&expand_rw), 0, "pthread_rwlock_wrlock guest");
    CHK_INT(pthread_rwlock_tryrdlock(&expand_rw), EBUSY,
            "pthread rw reader excluded by writer");
    CHK_INT(pthread_rwlock_unlock(&expand_rw), 0, "pthread rw writer unlock");
    CHK_INT(pthread_rwlock_destroy(&expand_rw), 0, "pthread_rwlock_destroy guest");

    memset((void *)expand_before, 0, sizeof expand_before);
    memset((void *)expand_after, 0, sizeof expand_after);
    memset((void *)expand_serial, 0, sizeof expand_serial);
    CHK_INT(pthread_barrier_init(&expand_barrier, 0, 4), 0,
            "pthread_barrier_init guest");
    for (int i = 0; i < 3; i++)
        CHK_INT(pthread_create(&th[i], 0, expand_barrier_worker, 0), 0,
                "pthread_create barrier worker");
    for (int round = 0; round < 2; round++) {
        while (__atomic_load_n(&expand_before[round], __ATOMIC_SEQ_CST) != 3)
            sched_yield();
        CHK_INT(expand_after[round], 0, "pthread barrier holds early arrivals");
        int rc = pthread_barrier_wait(&expand_barrier);
        CHK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
            "pthread barrier main return");
        if (rc == PTHREAD_BARRIER_SERIAL_THREAD)
            __atomic_add_fetch(&expand_serial[round], 1, __ATOMIC_SEQ_CST);
    }
    for (int i = 0; i < 3; i++)
        CHK_INT(pthread_join(th[i], 0), 0, "pthread_join barrier worker");
    CHK(expand_after[0] == 3 && expand_after[1] == 3,
        "pthread reusable barrier releases every worker");
    CHK(expand_serial[0] == 1 && expand_serial[1] == 1,
        "pthread barrier elects one serial thread per round");
    CHK_INT(pthread_barrier_destroy(&expand_barrier), 0,
            "pthread_barrier_destroy guest");

    pthread_rwlockattr_t rwa;
    pthread_barrierattr_t ba;
    CHK_INT(pthread_rwlockattr_init(&rwa), 0, "pthread rw attr init");
    CHK_INT(pthread_rwlockattr_setpshared(&rwa, PTHREAD_PROCESS_SHARED), ENOTSUP,
            "pthread rw shared truthfully unsupported");
    CHK_INT(pthread_barrierattr_init(&ba), 0, "pthread barrier attr init");
    CHK_INT(pthread_barrierattr_setpshared(&ba, PTHREAD_PROCESS_SHARED), ENOTSUP,
            "pthread barrier shared truthfully unsupported");
}

static mtx_t expand_c11_mutex;
static cnd_t expand_c11_cond;
static int expand_c11_count, expand_c11_ready;
static once_flag expand_c11_once = ONCE_FLAG_INIT;
static int expand_c11_once_count;

static void expand_c11_init_once(void) { expand_c11_once_count++; }

static int expand_c11_worker(void *arg)
{
    int add = (int)(long)arg;
    for (int i = 0; i < 400; i++) {
        mtx_lock(&expand_c11_mutex);
        expand_c11_count += add;
        mtx_unlock(&expand_c11_mutex);
    }
    mtx_lock(&expand_c11_mutex);
    expand_c11_ready++;
    cnd_signal(&expand_c11_cond);
    mtx_unlock(&expand_c11_mutex);
    call_once(&expand_c11_once, expand_c11_init_once);
    return 40 + add;
}

static void t_expand_c11_threads(void)
{
    thrd_t a, b;
    int ra = 0, rb = 0;
    expand_c11_count = expand_c11_ready = expand_c11_once_count = 0;
    CHK_INT(mtx_init(&expand_c11_mutex, mtx_plain), thrd_success,
            "C11 mtx_init guest");
    CHK_INT(cnd_init(&expand_c11_cond), thrd_success, "C11 cnd_init guest");
    CHK(thrd_equal(thrd_current(), thrd_current()), "C11 thrd_current/equal");
    CHK_INT(thrd_create(&a, expand_c11_worker, (void *)1L), thrd_success,
            "C11 thrd_create first");
    CHK_INT(thrd_create(&b, expand_c11_worker, (void *)2L), thrd_success,
            "C11 thrd_create second");
    mtx_lock(&expand_c11_mutex);
    while (expand_c11_ready != 2) cnd_wait(&expand_c11_cond, &expand_c11_mutex);
    mtx_unlock(&expand_c11_mutex);
    CHK_INT(thrd_join(a, &ra), thrd_success, "C11 thrd_join first");
    CHK_INT(thrd_join(b, &rb), thrd_success, "C11 thrd_join second");
    CHK(ra == 41 && rb == 42, "C11 thread integer results");
    CHK_INT(expand_c11_count, 1200, "C11 mutex protects shared counter");
    CHK_INT(expand_c11_once_count, 1, "C11 call_once across threads");
    cnd_destroy(&expand_c11_cond);
    mtx_destroy(&expand_c11_mutex);

    tss_t key;
    CHK_INT(tss_create(&key, NULL), thrd_success, "C11 tss_create guest");
    CHK_INT(tss_set(key, (void *)0x1234L), thrd_success, "C11 tss_set guest");
    CHK(tss_get(key) == (void *)0x1234L, "C11 tss_get guest");
    tss_delete(key);
}
