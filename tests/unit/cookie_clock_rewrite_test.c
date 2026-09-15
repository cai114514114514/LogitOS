/* Untouched older records must survive save/reopen after clock rollback.
 * Reading a Cookie header first updates access times and would hide this bug. */
#define main prior_persistence_main
#include "cookie_persistence_test.c"
#undef main

int main(void)
{
    cookie_jar_init(&jar);
    fresh();
    int64_t future = now + 28800;
    ck(set("older=kept; Path=/other; Secure; Max-Age=34560000", future - 172800),
       "older horizon cookie is stored");
    ck(set("recent=kept; Path=/; Secure; Max-Age=3600", future),
       "newer access timestamp is stored");
    ck(cookie_persistence_open(&state, &jar, &store, now) == 0 && jar.n == 2,
       "mixed-age snapshot opens after rollback");
    int deadlines = jar.n == 2;
    for (int i = 0; i < jar.n; i++)
        deadlines &= jar.v[i].expires <= (strcmp(jar.v[i].name, "older") ?
            future + 3600 : future - 172800 + CK_MAX_AGE_SECONDS);
    ck(deadlines, "rebasing never extends either original deadline");
    ck(cookie_persistence_flush(&state, &jar, now) == 0,
       "untouched rebased records can be saved");
    ck(cookie_persistence_open(&state, &jar, &store, now + 1) == 0 && jar.n == 2,
       "saved mixed-age records reopen without header access");
    ck(has("older", "kept") && has("recent", "kept"),
       "both cookie values survive the actual rewrite");
    cookie_persistence_open(&state, &jar, NULL, now);
    for (int i = 0; i < 2; i++) free(files[i]);
    cookie_jar_free(&jar);
    printf("cookie-clock-rewrite: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
