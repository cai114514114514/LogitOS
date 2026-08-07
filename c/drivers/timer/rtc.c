#include <stdint.h>
#include "rtc.h"
#include "io.h"
#include "spinlock.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

/* The CMOS is a two-port STATEFUL interface: write an index to 0x70, read the
 * value from 0x71. Two readers interleaving those writes read each other's
 * registers, and the kernel had no lock on it -- harmless while rtc_now() was
 * the only caller and only the window manager called it, and immediately not
 * harmless once the timer IRQ started sampling the seconds register at 100 Hz
 * to cross-check the clock. Measured, not theorised: the cross-check saw five
 * "second edges" in 639 ms, because a menu-bar clock redraw on another core was
 * leaving the index pointing at the hour register mid-read.
 *
 * Field discipline: the IRQ side uses trylock and answers its cached value if
 * the lock is busy. A timer interrupt must never wait on a lock held by a
 * thread -- least of all on the pre-BKL path, which exists precisely so the
 * tick cannot queue behind anything. */
static spinlock_t cmos_lock = SPINLOCK_INIT;

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

/* Bounded UIP wait: a stuck CMOS must not hang the caller. */
static void wait_update_done(void)
{
    for (long spins = 0; spins < 1000000 && update_in_progress(); spins++)
        ;
}

void rtc_now(struct rtc_time *t)
{
    struct rtc_time a, b;
    uint64_t fl = spin_lock_irqsave(&cmos_lock);

    /* Read twice and require agreement, so we never observe a mid-update. */
    do {
        wait_update_done();
        a.second  = cmos_read(0x00);
        a.minute  = cmos_read(0x02);
        a.hour    = cmos_read(0x04);
        a.weekday = cmos_read(0x06);
        a.day     = cmos_read(0x07);
        a.month   = cmos_read(0x08);
        a.year    = cmos_read(0x09);

        wait_update_done();
        b.second = cmos_read(0x00);
        b.minute = cmos_read(0x02);
        b.hour   = cmos_read(0x04);
    } while (a.second != b.second || a.minute != b.minute || a.hour != b.hour);

    uint8_t regb = cmos_read(0x0B);
    spin_unlock_irqrestore(&cmos_lock, fl);
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

    /* Clamp insane CMOS values -- callers index month-length tables with
     * md[month - 1], so a corrupt register must not escape as an index. */
    if (a.month < 1 || a.month > 12) a.month = 1;
    if (a.day < 1 || a.day > 31) a.day = 1;

    t->second  = a.second;
    t->minute  = a.minute;
    t->hour    = a.hour;
    t->day     = a.day;
    t->month   = a.month;
    t->year    = a.year + 2000;
    t->weekday = a.weekday % 7;
}

int rtc_second(void)
{
    static int last = 0;
    static int bcd = -1;                     /* register B does not change; read once */
    uint64_t fl;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(fl));
    if (!spin_trylock(&cmos_lock)) {         /* never WAIT from the timer IRQ */
        if (fl & 0x200) __asm__ volatile ("sti");
        return last;
    }
    /* One status read. If the RTC is mid-update the seconds register may be
     * half-written, and a torn value read as a second EDGE would corrupt the
     * calibration cross-check that samples this at 100 Hz -- so answer the
     * previous value and let the caller see the edge 10 ms later instead. */
    outb(CMOS_ADDR, 0x0A);
    if (!(inb(CMOS_DATA) & 0x80)) {
        if (bcd < 0) { outb(CMOS_ADDR, 0x0B); bcd = !(inb(CMOS_DATA) & 0x04); }
        uint8_t s = cmos_read(0x00);
        if (bcd) s = (uint8_t)((s & 0x0F) + (s >> 4) * 10);
        if (s <= 59) last = s;               /* else: corrupt register, keep the old value */
    }
    spin_unlock(&cmos_lock);
    if (fl & 0x200) __asm__ volatile ("sti");
    return last;
}

int64_t rtc_unix(void)
{
    struct rtc_time t;
    rtc_now(&t);
    static const int md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int64_t days = 0;
    for (int y = 1970; y < t.year; y++)
        days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    for (int m = 1; m < t.month && m <= 12; m++) {
        days += md[m-1];
        if (m == 2 && (t.year % 4 == 0 && (t.year % 100 != 0 || t.year % 400 == 0))) days++;
    }
    days += t.day - 1;
    return ((days * 24 + t.hour) * 60 + t.minute) * 60 + t.second;
}
