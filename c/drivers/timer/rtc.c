#include <stdint.h>
#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void)
{
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

void rtc_now(struct rtc_time *t)
{
    struct rtc_time a, b;

    /* Read twice and require agreement, so we never observe a mid-update. */
    do {
        while (update_in_progress())
            ;
        a.second  = cmos_read(0x00);
        a.minute  = cmos_read(0x02);
        a.hour    = cmos_read(0x04);
        a.weekday = cmos_read(0x06);
        a.day     = cmos_read(0x07);
        a.month   = cmos_read(0x08);
        a.year    = cmos_read(0x09);

        while (update_in_progress())
            ;
        b.second = cmos_read(0x00);
        b.minute = cmos_read(0x02);
        b.hour   = cmos_read(0x04);
    } while (a.second != b.second || a.minute != b.minute || a.hour != b.hour);

    uint8_t regb = cmos_read(0x0B);
    int bcd  = !(regb & 0x04);      /* values are BCD unless bit 2 set */
    int h24  = (regb & 0x02);       /* 24-hour unless bit 1 clear */
    int pm   = a.hour & 0x80;       /* PM flag in 12-hour mode */

    if (bcd) {
        a.second  = (a.second & 0x0F) + (a.second >> 4) * 10;
        a.minute  = (a.minute & 0x0F) + (a.minute >> 4) * 10;
        a.hour    = ((a.hour & 0x0F) + ((a.hour & 0x70) >> 4) * 10) | (a.hour & 0x80);
        a.day     = (a.day & 0x0F) + (a.day >> 4) * 10;
        a.month   = (a.month & 0x0F) + (a.month >> 4) * 10;
        a.year    = (a.year & 0x0F) + (a.year >> 4) * 10;
        a.weekday = (a.weekday & 0x0F) + (a.weekday >> 4) * 10;
    }
    a.hour &= 0x7F;
    if (!h24 && pm)
        a.hour = (a.hour + 12) % 24;

    t->second  = a.second;
    t->minute  = a.minute;
    t->hour    = a.hour;
    t->day     = a.day;
    t->month   = a.month;
    t->year    = a.year + 2000;
    t->weekday = a.weekday % 7;
}
