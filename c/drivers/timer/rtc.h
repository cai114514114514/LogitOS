#ifndef LOGIT_RTC_H
#define LOGIT_RTC_H

#include <stdint.h>

/* Wall-clock time read from the CMOS real-time clock. */
struct rtc_time {
    int year, month, day;
    int hour, minute, second;
    int weekday;                 /* 0 = Sunday */
};

/* Read the current date/time from the hardware RTC. */
void rtc_now(struct rtc_time *t);

/* Just the seconds field, 0..59, decoded but WITHOUT the double-read agreement
 * loop rtc_now() does. Two properties matter to the caller:
 *   - it is CHEAP (three port accesses), so it can be sampled at every 10 ms
 *     tick, which is what makes second-edge detection accurate to 10 ms;
 *   - it never returns a torn value: if the RTC's update-in-progress bit is set
 *     it answers the previous reading instead of a half-updated one.
 * Used by the time subsystem to cross-check the calibrated clocksource against
 * an independent oscillator. */
int rtc_second(void);

/* Whole seconds since the Unix epoch, from the RTC (assumed UTC). This is the
 * `now` a certificate validity check or an HTTP date wants. The time subsystem
 * latches it ONCE into a wall-clock offset -- read it repeatedly and you are
 * back to a clock with one-second resolution that a user can wind backwards. */
int64_t rtc_unix(void);

#endif /* LOGIT_RTC_H */
