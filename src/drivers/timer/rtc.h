#ifndef AQUA_RTC_H
#define AQUA_RTC_H

#include <stdint.h>

/* Wall-clock time read from the CMOS real-time clock. */
struct rtc_time {
    int year, month, day;
    int hour, minute, second;
    int weekday;                 /* 0 = Sunday */
};

/* Read the current date/time from the hardware RTC. */
void rtc_now(struct rtc_time *t);

#endif /* AQUA_RTC_H */
