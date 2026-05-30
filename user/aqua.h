#ifndef AQUA_USERLIB_H
#define AQUA_USERLIB_H

#include "aqua_abi.h"     /* shared with the kernel (-Iinclude) */

static inline long _sys(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return r;
}

static inline unsigned rgb(int r, int g, int b)
{
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

static inline void gui_create(const char *title, int w, int h)
{ _sys(SYS_GUI_CREATE, (long)title, ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF), 0); }

static inline void gui_clear(unsigned color) { _sys(SYS_GUI_CLEAR, color, 0, 0); }

static inline void gui_rect(int x, int y, int w, int h, unsigned color)
{ _sys(SYS_GUI_RECT, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
       ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF), color); }

static inline void gui_text(int x, int y, unsigned color, const char *s)
{ _sys(SYS_GUI_TEXT, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF), color, (long)s); }

static inline void gui_flush(void) { _sys(SYS_GUI_FLUSH, 0, 0, 0); }
static inline int  poll_event(struct aqua_event *e) { return (int)_sys(SYS_POLL_EVENT, (long)e, 0, 0); }
static inline int  get_arg(char *b, int m) { return (int)_sys(SYS_GET_ARG, (long)b, m, 0); }
static inline void get_time(struct aqua_time *t) { _sys(SYS_GET_TIME, (long)t, 0, 0); }
static inline int  read_file(const char *n, void *b, int m) { return (int)_sys(SYS_READ_FILE, (long)n, (long)b, m); }
static inline void sys_yield(void) { _sys(SYS_YIELD, 0, 0, 0); }
static inline void app_exit(int c) { _sys(SYS_EXIT, c, 0, 0); }

#endif /* AQUA_USERLIB_H */
