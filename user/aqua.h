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
static inline int  sysinfo(char *b, int m) { return (int)_sys(SYS_SYSINFO, (long)b, m, 0); }
static inline int  file_count(void) { return (int)_sys(SYS_FILE_COUNT, 0, 0, 0); }
static inline int  file_name(int i, char *b, int m) { return (int)_sys(SYS_FILE_NAME, i, (long)b, m); }
static inline int  write_file(const char *n, const void *b, int size) { return (int)_sys(SYS_WRITE_FILE, (long)n, (long)b, size); }
static inline int  delete_file(const char *n) { return (int)_sys(SYS_DELETE_FILE, (long)n, 0, 0); }
static inline int  make_dir(const char *p) { return (int)_sys(SYS_MKDIR, (long)p, 0, 0); }
static inline int  dir_count(const char *p) { return (int)_sys(SYS_DIR_COUNT, (long)p, 0, 0); }
/* fills buf (must be >= 64 bytes) with entry name; returns file size, -2 dir, -1 none */
static inline int  dir_name(const char *p, int i, char *buf) { return (int)_sys(SYS_DIR_NAME, (long)p, i, (long)buf); }

/* --- networking --- */
static inline int      net_info(struct aqua_netinfo *ni) { return (int)_sys(SYS_NET_INFO, (long)ni, 0, 0); }
static inline int      net_ping(unsigned ip) { return (int)_sys(SYS_NET_PING, (long)ip, 0, 0); }
static inline int      net_ping_rtt(void) { return (int)_sys(SYS_NET_PING_RTT, 0, 0, 0); }
static inline int      net_dns(const char *name) { return (int)_sys(SYS_NET_DNS, (long)name, 0, 0); }
static inline unsigned net_dns_result(void) { return (unsigned)_sys(SYS_NET_DNS_RESULT, 0, 0, 0); }

/* --- HTTP (browser) --- */
static inline int http_get(const char *url) { return (int)_sys(SYS_HTTP_GET, (long)url, 0, 0); }
static inline int http_status(void) { return (int)_sys(SYS_HTTP_STATUS, 0, 0, 0); }
static inline int http_read(int off, char *buf, int max) { return (int)_sys(SYS_HTTP_READ, off, (long)buf, max); }
static inline int http_link(int i, char *buf, int max) { return (int)_sys(SYS_HTTP_LINK, i, (long)buf, max); }

#endif /* AQUA_USERLIB_H */
