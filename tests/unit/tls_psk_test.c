/* The ticket cache and the pre_shared_key extension, tested at the unit level
 * against a clock we control.
 *
 * WHY this exists as well as the interop suite. Both bugs this file pins were
 * INVISIBLE to `openssl s_server`: it resumed happily with an
 * obfuscated_ticket_age that was ten times too small, and it never issued
 * enough tickets at once for the one-per-host cache to matter. They only
 * appeared against a real production server (www.kimi.com, which issues eight
 * tickets per handshake and treats them as single-use), where two of three
 * pooled connections were refused and fell back to a full handshake.
 *
 * An interop test cannot assert either of these, because the only signal it
 * gets is "the server resumed or it did not", and a lenient server resumes
 * either way. So they are asserted here, on the bytes we put on the wire and on
 * the state of the cache, where the answer does not depend on the peer's mood.
 *
 *   1. obfuscated_ticket_age is elapsed MILLISECONDS + age_add. timer_ticks()
 *      is 10 ms per tick, so reading it as ms understates the age by 10x.
 *   2. A ticket is single-use: arming a session must REMOVE it, so two
 *      connections opened at once never offer the same identity.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "tls_int.h"

/* ------------------------------------------------------------ host "kernel" */
static uint64_t g_ms;                    /* the clock the test drives */
uint64_t timer_ms(void)    { return g_ms; }
uint64_t timer_ticks(void) { return g_ms / 10; }   /* 100 Hz, as in pit.c */
void kprintf(const char *fmt, ...) { (void)fmt; }  /* quiet */

static int fails, checks;
static void ok(const char *nm, int cond)
{
    checks++;
    if (cond) printf("ok   %s\n", nm);
    else { printf("FAIL %s\n", nm); fails++; }
}

/* --------------------------------------------------------------- utilities */

static struct tls_sess S;

/* One fixed "unix now" for both storing and arming. Expiry is measured on this
 * clock, so a test that let it drift between the two would see its own
 * tickets expire -- which is how the first draft of this file failed. The ms
 * clock (g_ms) is separate and IS driven, because the age is what case_age is
 * about. */
#define NOW0 1000000

static void sess_reset(const char *host)
{
    memset(&S, 0, sizeof S);
    S.used = 1;
    S.now  = NOW0;
    int i = 0; while (host[i]) { S.host[i] = host[i]; i++; }
    S.host[i] = 0;
    sha256_init(&S.th);
    sha384_init(&S.th384);
    S.hashlen = 32;
}

/* Feed a synthetic NewSessionTicket into the cache. */
static int store(const char *host, const char *idtext, uint32_t age_add, uint32_t life)
{
    uint8_t res_master[32];
    for (int i = 0; i < 32; i++) res_master[i] = (uint8_t)(i + 1);
    uint8_t nonce[8] = { 0,1,2,3,4,5,6,7 };
    return tls_psk_store(host, TLS_AES_128_GCM_SHA256, res_master, nonce, 8,
                         (const uint8_t *)idtext, (int)strlen(idtext),
                         life, age_add, NOW0);
}

/* Pull the identity and the obfuscated age back out of a built extension.
 * Layout: type(2) len(2) ids_len(2) id_len(2) id[] age(4) binders... */
static int parse_ext(const uint8_t *p, int n, char *id, int idmax, uint32_t *age)
{
    if (n < 12) return -1;
    if (((p[0] << 8) | p[1]) != EXT_PSK) return -1;
    int idlen = (p[6] << 8) | p[7];
    if (idlen <= 0 || idlen >= idmax || 8 + idlen + 4 > n) return -1;
    memcpy(id, p + 8, (size_t)idlen); id[idlen] = 0;
    const uint8_t *a = p + 8 + idlen;
    *age = ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) |
           ((uint32_t)a[2] << 8)  |  (uint32_t)a[3];
    return idlen;
}

/* Arm a session for `host` and build its extension. Returns bytes, or -1. */
static int arm_and_build(const char *host, uint8_t *buf, int max)
{
    sess_reset(host);
    if (!tls_psk_arm(&S)) return -1;
    int tr = 0, bo = 0;
    return tls_psk_ext(&S, buf, max, &tr, &bo);
}

/* ------------------------------------------------------------------- cases */

/* 1. THE AGE UNIT. This is the assertion that fails if timer_ticks() is read
 *    as milliseconds -- it comes out at exactly a tenth of the elapsed time. */
static void case_age(void)
{
    tls_psk_clear_all();
    g_ms = 5000;
    ok("store accepted", store("a.example", "TICKET-A", 0x11223344u, 7200) == 0);

    g_ms = 5000 + 3210;                  /* 3210 ms later */
    uint8_t buf[1024]; char id[256]; uint32_t age = 0;
    int n = arm_and_build("a.example", buf, (int)sizeof buf);
    ok("extension built", n > 0);
    ok("identity is the ticket we stored",
       n > 0 && parse_ext(buf, n, id, sizeof id, &age) > 0 && strcmp(id, "TICKET-A") == 0);
    /* age_add is added modulo 2^32; the age itself must be the elapsed
     * MILLISECONDS. 3210 -- not 321, which is what the tick counter gives. */
    ok("obfuscated_ticket_age is elapsed ms + age_add",
       age == (uint32_t)(3210u + 0x11223344u));
    ok("...and is NOT the 10x-too-small tick value",
       age != (uint32_t)(321u + 0x11223344u));

    /* A longer gap, to be sure it scales rather than happening to match. */
    tls_psk_clear_all();
    g_ms = 1000;
    store("a.example", "TICKET-A2", 0, 7200);
    g_ms = 1000 + 60000;                 /* one minute */
    n = arm_and_build("a.example", buf, (int)sizeof buf);
    ok("one minute reads as 60000 ms",
       n > 0 && parse_ext(buf, n, id, sizeof id, &age) > 0 && age == 60000u);
}

/* 2. SINGLE USE. The field failure: a connection pool opening several
 *    connections must not offer the same identity twice. */
static void case_single_use(void)
{
    tls_psk_clear_all();
    g_ms = 1000;
    /* A batch, as a real server issues it. www.kimi.com sends eight. */
    for (int i = 0; i < 8; i++) {
        char nm[32]; snprintf(nm, sizeof nm, "T%d", i);
        store("k.example", nm, 0, 43200);
    }
    ok("all 8 of the batch are cached", tls_psk_count() == 8);

    uint8_t buf[1024]; char id[256]; uint32_t age;
    char seen[8][256];
    for (int i = 0; i < 8; i++) {
        int n = arm_and_build("k.example", buf, (int)sizeof buf);
        if (n <= 0 || parse_ext(buf, n, id, sizeof id, &age) <= 0) {
            printf("FAIL could not arm connection %d\n", i); fails++; checks++;
            seen[i][0] = 0; continue;
        }
        snprintf(seen[i], sizeof seen[i], "%s", id);
    }
    /* Every one of the eight connections got a DIFFERENT identity. Before the
     * fix the cache held one ticket per host, so all eight of these were the
     * same string and a single-use server refused seven of them. */
    int dup = 0;
    for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++)
            if (seen[i][0] && strcmp(seen[i], seen[j]) == 0) dup = 1;
    ok("8 connections get 8 DISTINCT identities", !dup);
    ok("the batch is used FIFO", strcmp(seen[0], "T0") == 0 && strcmp(seen[7], "T7") == 0);
    ok("cache is empty once the batch is spent", tls_psk_count() == 0);

    /* A ninth connection has nothing left and must say so rather than
     * re-offering a spent identity. */
    sess_reset("k.example");
    ok("9th connection does not resume", tls_psk_arm(&S) == 0 && S.psk_offered == 0);
}

/* 3. Scoping and expiry -- unchanged behaviour, re-asserted because the cache
 *    was rewritten underneath them. */
static void case_scope_expiry(void)
{
    tls_psk_clear_all();
    g_ms = 1000;
    store("one.example", "ONE", 0, 7200);
    store("two.example", "TWO", 0, 7200);

    uint8_t buf[1024]; char id[256]; uint32_t age;
    int n = arm_and_build("two.example", buf, (int)sizeof buf);
    ok("a host gets ITS OWN ticket, not the other one",
       n > 0 && parse_ext(buf, n, id, sizeof id, &age) > 0 && strcmp(id, "TWO") == 0);

    sess_reset("three.example");
    ok("an unknown host does not resume", tls_psk_arm(&S) == 0);

    /* Expiry is on the unix clock, not the ms clock. */
    tls_psk_clear_all();
    store("exp.example", "OLD", 0, 100);         /* 100 s lifetime */
    sess_reset("exp.example");
    S.now += 101;
    ok("an expired ticket is not offered", tls_psk_arm(&S) == 0);
    ok("...and is dropped from the cache", tls_psk_count() == 0);

    /* A zero lifetime is a ticket that is already dead on arrival. */
    tls_psk_clear_all();
    ok("a zero-lifetime ticket is refused at store", store("z.example", "Z", 0, 0) != 0);

    /* Eviction: more tickets than the cache holds must not corrupt it. */
    tls_psk_clear_all();
    for (int i = 0; i < 40; i++) {
        char nm[32]; snprintf(nm, sizeof nm, "E%d", i);
        store("eviction.example", nm, 0, 7200);
    }
    ok("cache never exceeds its bound", tls_psk_count() <= 16);
    n = arm_and_build("eviction.example", buf, (int)sizeof buf);
    ok("a surviving ticket is still usable after eviction",
       n > 0 && parse_ext(buf, n, id, sizeof id, &age) > 0);

    /* An over-long identity must be refused rather than truncated -- a
     * truncated identity is a ticket the server cannot match. */
    tls_psk_clear_all();
    char big[2048]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    ok("an over-long ticket is refused", store("big.example", big, 0, 7200) != 0);
    ok("...and nothing was cached", tls_psk_count() == 0);
}

/* 4. The binder must change when the transcript does. Same ticket, different
 *    ClientHello -> different binder, or a replayed binder would be accepted. */
static void case_binder_binds(void)
{
    tls_psk_clear_all();
    g_ms = 1000;
    store("b.example", "BIND", 0, 7200);

    uint8_t buf[1024];
    sess_reset("b.example");
    if (!tls_psk_arm(&S)) { printf("FAIL arm\n"); fails++; checks++; return; }
    int tr = 0, bo = 0;
    int n = tls_psk_ext(&S, buf, (int)sizeof buf, &tr, &bo);
    if (n <= 0) { printf("FAIL ext\n"); fails++; checks++; return; }

    struct sha256 th1, th2;
    sha256_init(&th1); sha256_update(&th1, "hello-one", 9);
    sha256_init(&th2); sha256_update(&th2, "hello-two", 9);

    uint8_t b1[32], b2[32];
    tls_psk_binder(&S, &th1, buf, tr, bo); memcpy(b1, buf + bo, 32);
    tls_psk_binder(&S, &th2, buf, tr, bo); memcpy(b2, buf + bo, 32);
    ok("binder depends on the transcript", memcmp(b1, b2, 32) != 0);

    tls_psk_binder(&S, &th1, buf, tr, bo);
    ok("binder is deterministic for one transcript", memcmp(b1, buf + bo, 32) == 0);

    /* And on the PSK: a different secret must not produce the same proof. */
    S.psk[0] ^= 0xff;
    tls_psk_binder(&S, &th1, buf, tr, bo);
    ok("binder depends on the PSK", memcmp(b1, buf + bo, 32) != 0);
}

int main(void)
{
    printf("== TLS 1.3 ticket cache / pre_shared_key ==\n");
    case_age();
    case_single_use();
    case_scope_expiry();
    case_binder_binds();
    printf("\n%d checks, %d failed\n", checks, fails);
    if (fails) { printf("TLS PSK FAILED\n"); return 1; }
    printf("TLS PSK ALL PASS\n");
    return 0;
}
