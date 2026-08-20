#ifndef _SYS_TIMERFD_H
#define _SYS_TIMERFD_H

#include <sys/time.h>
#include <time.h>

/* timerfd(2). A deadline that is a descriptor, so several independent ones can
 * be waited on by one poll(). read() yields a uint64_t count of expirations
 * since the last read.
 *
 * THE CLOCK IS THE 100 Hz TICK, so the resolution is 10 ms whatever the
 * nanosecond fields say -- struct itimerspec is kept because every ported
 * program already builds one, not because this machine can honour it. A value
 * under one tick is rounded UP to one tick rather than down to zero: a timer
 * that was asked for and never fires is the worse failure.
 *
 * CLOCK_MONOTONIC is the only clock. CLOCK_REALTIME is REFUSED (EINVAL) rather
 * than silently substituted: the difference between them is whether the timer
 * survives the wall clock being set, and answering the wrong one quietly is how
 * a scheduler ends up firing every timer at once when the RTC is read. */
#define TFD_NONBLOCK  0x0800     /* == O_NONBLOCK */
#define TFD_TIMER_ABSTIME 1      /* accepted only as 0; see timerfd_settime */

/* Defined HERE and not in <time.h>, because timerfd is its only user in this
 * tree: timer_create()/timer_settime() do not exist, and putting the struct in
 * the shared header would advertise a POSIX timers facility that is not here.
 * Guarded so that adding those later needs no edit to this file. */
#ifndef _STRUCT_ITIMERSPEC_DECLARED
#define _STRUCT_ITIMERSPEC_DECLARED
struct itimerspec {
    struct timespec it_interval;   /* period after the first expiry */
    struct timespec it_value;      /* time to the first expiry; 0 = disarm */
};
#endif

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *newv,
                    struct itimerspec *oldv);

#endif /* _SYS_TIMERFD_H */
