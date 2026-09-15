/* Ring-3 runtime proof for the high-thread-count CPU path. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int decimal(const char *s)
{
    int n = 0;
    if (!s || !*s) return -1;
    while (*s >= '0' && *s <= '9') n = n * 10 + *s++ - '0';
    return *s ? -1 : n;
}

static void *worker(void *arg)
{
    for (int i = 0; i < 64; i++) sched_yield();
    return arg;
}

static long long millis(const struct timespec *a, const struct timespec *b)
{
    return (long long)(b->tv_sec - a->tv_sec) * 1000 +
           (b->tv_nsec - a->tv_nsec) / 1000000;
}

int main(int argc, char **argv)
{
    int expected = argc > 1 ? decimal(argv[1]) : -1;
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (expected < 1 || online != expected || online > 32) {
        printf("XEON_E5_GUEST_FAIL online=%ld expected=%d\n", online, expected);
        return 1;
    }

    /* A guarded pthread stack consumes two VMA records.  The current
     * VMA_MAXAREA=32 process layout reliably permits twelve simultaneous
     * workers, which is enough to prove that ring-3 scheduling crossed the
     * former eight-CPU ceiling without pretending that every online CPU needs
     * a permanently resident pthread. */
    int workers = online < 12 ? (int)online : 12;
    pthread_t threads[32];
    for (int i = 0; i < workers; i++) {
        if (pthread_create(&threads[i], 0, worker,
                           (void *)(intptr_t)(i + 1)) != 0) {
            printf("XEON_E5_GUEST_FAIL create=%d workers=%d online=%ld\n",
                   i, workers, online);
            return 2;
        }
    }
    long sum = 0;
    for (int i = 0; i < workers; i++) {
        void *value = 0;
        if (pthread_join(threads[i], &value) != 0) {
            printf("XEON_E5_GUEST_FAIL join=%d workers=%d online=%ld\n",
                   i, workers, online);
            return 3;
        }
        sum += (long)(intptr_t)value;
    }

    struct timespec begin, end;
    if (clock_gettime(CLOCK_MONOTONIC, &begin) != 0 ||
        usleep(50000) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        printf("XEON_E5_GUEST_FAIL monotonic\n");
        return 4;
    }
    long long elapsed = millis(&begin, &end);
    long want_sum = workers * (workers + 1) / 2;
    printf("XEON_E5_GUEST online=%ld expected=%d workers=%d sum=%ld elapsed_ms=%lld\n",
           online, expected, workers, sum, elapsed);
    if (workers <= 8 || sum != want_sum || elapsed < 20) {
        printf("XEON_E5_GUEST_FAIL runtime\n");
        return 5;
    }
    printf("XEON_E5_GUEST_OK\n");
    return 0;
}
