#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <time.h>
struct timeval { time_t tv_sec; long tv_usec; };
int gettimeofday(struct timeval *, void *);

#define timerisset(tvp) ((tvp)->tv_sec || (tvp)->tv_usec)
#define timerclear(tvp) ((tvp)->tv_sec = (tvp)->tv_usec = 0)
#define timercmp(a, b, cmp) \
    (((a)->tv_sec == (b)->tv_sec) ? ((a)->tv_usec cmp (b)->tv_usec) : \
                                    ((a)->tv_sec cmp (b)->tv_sec))
#define timeradd(a, b, out) do { \
    (out)->tv_sec = (a)->tv_sec + (b)->tv_sec; \
    (out)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
    if ((out)->tv_usec >= 1000000L) { (out)->tv_sec++; (out)->tv_usec -= 1000000L; } \
} while (0)
#define timersub(a, b, out) do { \
    (out)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
    (out)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
    if ((out)->tv_usec < 0) { (out)->tv_sec--; (out)->tv_usec += 1000000L; } \
} while (0)
#endif
