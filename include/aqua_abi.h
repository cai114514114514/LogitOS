#ifndef AQUA_ABI_H
#define AQUA_ABI_H

/* System-call ABI shared between the kernel and userland applications.
 * Convention: rax = number, rdi/rsi/rdx = args, return value in rax. */

#define SYS_WRITE       1   /* (fd, buf, len) -> len  (fd 1 = serial log) */
#define SYS_EXIT        2   /* (code) -> terminate the process */
#define SYS_GUI_CREATE  3   /* (title, (w<<16)|h) -> 0  create the app window */
#define SYS_GUI_CLEAR   4   /* (color) */
#define SYS_GUI_RECT    5   /* ((x<<16)|y, (w<<16)|h, color) */
#define SYS_GUI_TEXT    6   /* ((x<<16)|y, color, str) */
#define SYS_GUI_FLUSH   7   /* () present the window */
#define SYS_POLL_EVENT  8   /* (event*) -> 1 if an event was returned else 0 */
#define SYS_GET_ARG     9   /* (buf, max) -> length of launch argument */
#define SYS_GET_TIME   10   /* (rtc_time*) fill wall-clock time */
#define SYS_READ_FILE  11   /* (name, buf, max) -> bytes read, or -1 */
#define SYS_YIELD      12   /* () yield the CPU */
#define SYS_SYSINFO    13   /* (buf, max) -> kernel writes uptime/mem/process text */
#define SYS_FILE_COUNT 14   /* () -> number of files on the volume */
#define SYS_FILE_NAME  15   /* (i, buf, max) -> file size; fills name */

/* Event types returned by SYS_POLL_EVENT. */
#define EV_NONE   0
#define EV_KEY    1   /* a = character */
#define EV_MOUSE  2   /* a = x, b = y (window-local), mouse-button down */
#define EV_CLOSE  3   /* the window's close button was pressed */

struct aqua_event {
    int type;
    int a;
    int b;
};

/* Wall-clock time (mirrors the kernel's struct rtc_time field order). */
struct aqua_time {
    int year, month, day;
    int hour, minute, second;
    int weekday;
};

#endif /* AQUA_ABI_H */
