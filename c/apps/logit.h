#ifndef LOGIT_USERLIB_H
#define LOGIT_USERLIB_H

#include "logit_abi.h"     /* shared with the kernel (-Iinclude) */

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

static inline void gui_rrect(int x, int y, int w, int h, int radius, unsigned color)
{ _sys(SYS_GUI_RRECT, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
       ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF), ((long)(radius & 0xFF) << 24) | (color & 0xFFFFFF)); }

static inline void gui_text(int x, int y, unsigned color, const char *s)
{ _sys(SYS_GUI_TEXT, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF), color, (long)s); }

/* Monospace text: each glyph occupies a `cell`-pixel column (CJK = 2 cells). */
static inline void gui_text_mono(int x, int y, unsigned color, int cell, const char *s)
{ _sys(SYS_GUI_TEXT_MONO, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
       ((long)(cell & 0xFF) << 24) | (color & 0xFFFFFF), (long)s); }

static inline void gui_flush(void) { _sys(SYS_GUI_FLUSH, 0, 0, 0); }
static inline int  poll_event(struct logit_event *e) { return (int)_sys(SYS_POLL_EVENT, (long)e, 0, 0); }
static inline int  get_arg(char *b, int m) { return (int)_sys(SYS_GET_ARG, (long)b, m, 0); }
static inline void get_time(struct logit_time *t) { _sys(SYS_GET_TIME, (long)t, 0, 0); }
/* Milliseconds since boot: the clock to SUBTRACT. get_time() is the wall clock
 * in whole seconds and can be set backwards, so it can neither time an
 * animation nor pace a frame.
 *
 * Resolution is 10 ms, not 1 ms -- the value comes from the kernel's 100 Hz
 * tick, so it advances in steps of ten. Time a 3 ms operation with it and the
 * honest answers are 0 and 10. Returns unsigned 64-bit and never wraps in any
 * real uptime, so `monotonic_ms() - t0` needs no wraparound guard. */
static inline unsigned long long monotonic_ms(void)
{ return (unsigned long long)_sys(SYS_MONOTONIC_MS, 0, 0, 0); }
static inline int  read_file(const char *n, void *b, int m) { return (int)_sys(SYS_READ_FILE, (long)n, (long)b, m); }
static inline void sys_yield(void) { _sys(SYS_YIELD, 0, 0, 0); }
static inline void app_exit(int c) { _sys(SYS_EXIT, c, 0, 0); }

/* --- M18: processes --- */
static inline int  sys_fork(void) { return (int)_sys(SYS_FORK, 0, 0, 0); }
static inline int  sys_getpid(void) { return (int)_sys(SYS_GETPID, 0, 0, 0); }
static inline int  sys_cpu_index(void) { return (int)_sys(SYS_CPU_INDEX, 0, 0, 0); }   /* SMP: running core's index */
static inline int  sys_ui_dark(int set) { return (int)_sys(SYS_UI_DARK, set, 0, 0); }  /* set<0 query; system dark mode 0/1 */
static inline long sys_kheap_stress(long iters, int size, unsigned long seed) { return _sys(SYS_KHEAP_STRESS, iters, size, (long)seed); }  /* BKL-free concurrent kmalloc stress -> corruption count */
static inline int  sys_waitpid(int pid, int *status) { return (int)_sys(SYS_WAITPID, pid, (long)status, 0); }
static inline int  sys_execve(const char *p, char *const argv[], char *const envp[]) { return (int)_sys(SYS_EXECVE, (long)p, (long)argv, (long)envp); }

/* --- M18: file descriptors --- */
static inline int  sys_write(int fd, const void *buf, int len) { return (int)_sys(SYS_WRITE, fd, (long)buf, len); }
static inline int  sys_read(int fd, void *buf, int len) { return (int)_sys(SYS_READ, fd, (long)buf, len); }
static inline int  sys_open(const char *p, int flags) { return (int)_sys(SYS_OPEN, (long)p, flags, 0); }
static inline int  sys_close(int fd) { return (int)_sys(SYS_CLOSE, fd, 0, 0); }
static inline long sys_lseek(int fd, long off, int whence) { return _sys(SYS_LSEEK, fd, off, whence); }
static inline int  sys_dup(int fd) { return (int)_sys(SYS_DUP, fd, 0, 0); }
static inline int  sys_dup2(int o, int n) { return (int)_sys(SYS_DUP2, o, n, 0); }
static inline int  sys_pipe(int fds[2]) { return (int)_sys(SYS_PIPE, (long)fds, 0, 0); }
static inline int  sys_getcwd(char *b, int n) { return (int)_sys(SYS_GETCWD, (long)b, n, 0); }
static inline int  sys_chdir(const char *p) { return (int)_sys(SYS_CHDIR, (long)p, 0, 0); }
static inline int  sys_set_nonblock(int fd) { return (int)_sys(SYS_SETNB, fd, 0, 0); }
static inline int  sysinfo(char *b, int m) { return (int)_sys(SYS_SYSINFO, (long)b, m, 0); }
static inline int  file_count(void) { return (int)_sys(SYS_FILE_COUNT, 0, 0, 0); }
static inline int  file_name(int i, char *b, int m) { return (int)_sys(SYS_FILE_NAME, i, (long)b, m); }
static inline int  write_file(const char *n, const void *b, int size) { return (int)_sys(SYS_WRITE_FILE, (long)n, (long)b, size); }
static inline int  delete_file(const char *n) { return (int)_sys(SYS_DELETE_FILE, (long)n, 0, 0); }
static inline int  make_dir(const char *p) { return (int)_sys(SYS_MKDIR, (long)p, 0, 0); }
static inline int  sys_rename(const char *old, const char *new_path) { return (int)_sys(SYS_RENAME, (long)old, (long)new_path, 0); }
static inline int  sys_open_path(const char *path) { return (int)_sys(SYS_OPEN_PATH, (long)path, 0, 0); }
static inline int  dir_count(const char *p) { return (int)_sys(SYS_DIR_COUNT, (long)p, 0, 0); }
/* fills buf (must be >= 64 bytes) with entry name; returns file size, -2 dir, -1 none */
static inline int  dir_name(const char *p, int i, char *buf) { return (int)_sys(SYS_DIR_NAME, (long)p, i, (long)buf); }

/* --- networking --- */
static inline int      net_info(struct logit_netinfo *ni) { return (int)_sys(SYS_NET_INFO, (long)ni, 0, 0); }
static inline int      net_ping(unsigned ip) { return (int)_sys(SYS_NET_PING, (long)ip, 0, 0); }
static inline int      net_ping_rtt(void) { return (int)_sys(SYS_NET_PING_RTT, 0, 0, 0); }
static inline int      net_dns(const char *name) { return (int)_sys(SYS_NET_DNS, (long)name, 0, 0); }
static inline unsigned net_dns_result(void) { return (unsigned)_sys(SYS_NET_DNS_RESULT, 0, 0, 0); }

/* --- M27: non-blocking client sockets ---
 * Nothing here waits. sock_open returns a handle immediately and the connection
 * (DNS -> TCP -> TLS) is driven forward by the kernel's net_poll, so an app can
 * hold several of these open at once and they progress together. Poll with
 * sock_poll until SOCK_P_CONNECTED, then send/recv; both may be short, and a
 * recv of 0 means "nothing yet", not end of stream (that is SOCK_P_EOF). */
static inline int sock_open(const char *host, int port, int flags)
{ return (int)_sys(SYS_SOCK_OPEN, (long)host,
                   ((long)(port & 0xFFFF) << 16) | (flags & 0xFFFF), 0); }
static inline int sock_poll(int fd) { return (int)_sys(SYS_SOCK_POLL, fd, 0, 0); }
static inline int sock_send(int fd, const void *buf, int len) { return (int)_sys(SYS_SOCK_SEND, fd, (long)buf, len); }
static inline int sock_recv(int fd, void *buf, int max) { return (int)_sys(SYS_SOCK_RECV, fd, (long)buf, max); }
/* The negotiated ALPN protocol ("h2" / "http/1.1"), NUL-terminated; 0 if none. */
static inline int sock_alpn(int fd, char *buf, int max) { return (int)_sys(SYS_SOCK_ALPN, fd, (long)buf, max); }
static inline int sock_close(int fd) { return (int)_sys(SYS_SOCK_CLOSE, fd, 0, 0); }

/* --- HTTP + ring-3 render pipeline (browser) --- */
static inline int http_get(const char *url) { return (int)_sys(SYS_HTTP_GET, (long)url, 0, 0); }
static inline int http_status(void) { return (int)_sys(SYS_HTTP_STATUS, 0, 0, 0); }
/* Copy the last http_get response body into buf (<= max); returns length. */
static inline int http_body(char *buf, int max) { return (int)_sys(SYS_HTTP_BODY, (long)buf, max, 0); }

/* Render primitives used by the app-side paint (kernel owns fonts + framebuffer). */
static inline int text_measure_px(const char *s, int len, int px, int mono)
{ return (int)_sys(SYS_TEXT_MEASURE, (long)s, len, ((long)px << 1) | (mono & 1)); }
static inline void gui_text_run(int x, int y, int px, int mono, unsigned color, const char *s, int len)
{ struct logit_run r = { x, y, px, mono, color, s, len }; _sys(SYS_GUI_TEXT_RUN, (long)&r, 0, 0); }
static inline void gui_blit(int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh)
{ struct logit_blit b = { x, y, w, h, rgba, sw, sh }; _sys(SYS_GUI_BLIT, (long)&b, 0, 0); }
/* Decode an image file into `rgba` (>= w*h*4 bytes); returns 0 + sets *w,*h, or -1. */
static inline int img_open(const char *path, unsigned char *rgba, int max, int *w, int *h)
{
    struct logit_imgreq q = { path, rgba, max, 0, 0 };
    int rc = (int)_sys(SYS_IMG_DECODE, (long)&q, 0, 0);
    if (rc == 0) { *w = q.w; *h = q.h; }
    return rc;
}
static inline void gui_clip(int x, int y, int w, int h)
{ _sys(SYS_GUI_CLIP, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
       ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF), 0); }
/* Procedural vector icons (ids match src/kernel/gui/icons.h), painted in `color`. */
enum { GICON_FOLDER, GICON_DOC, GICON_TERMINAL, GICON_GRID, GICON_GLOBE,
       GICON_CODE, GICON_CHART, GICON_CLOCK, GICON_IMAGE };
static inline void gui_icon(int id, int x, int y, int px, unsigned color)
{ _sys(SYS_GUI_ICON, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
       ((long)(id & 0xFFFF) << 16) | (px & 0xFFFF), color); }
/* Liquid-glass a region of THIS window over the content already drawn there
 * (frost + rim refraction + specular highlight + body tint). */
static inline void gui_glass(int x, int y, int w, int h, int radius,
                             unsigned char tr, unsigned char tg, unsigned char tb, unsigned char ta)
{ _sys(SYS_GUI_GLASS, ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF),
       ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF),
       ((long)radius << 32) | ((long)tr << 24) | ((long)tg << 16) | ((long)tb << 8) | ta); }
/* Fetch a sub-resource's raw bytes (e.g. an image) into buf (<= max); length or <0. */
static inline int res_fetch_raw(const char *src, unsigned char *buf, int max)
{ return (int)_sys(SYS_RES_FETCH, (long)src, (long)buf, max); }

/* --- display geometry (see the SYS_SCREEN_INFO note in logit_abi.h) ---
 * screen_w()/screen_h() are POINTS -- the same unit gui_create() and every
 * coordinate in struct logit_event use -- so `gui_create("X", screen_w()-80,
 * screen_h()-120)` sizes a window against the desktop at any display density.
 * ui_scale() is for reporting and asset choice ONLY: the kernel has already
 * applied it, and multiplying layout by it draws everything twice too big. */
static inline int screen_info(int what) { return (int)_sys(SYS_SCREEN_INFO, what, 0, 0); }
static inline int screen_w(void)  { return screen_info(SCREEN_W); }
static inline int screen_h(void)  { return screen_info(SCREEN_H); }
static inline int ui_scale(void)  { return screen_info(SCREEN_SCALE); }

/* --- M28 time -------------------------------------------------------------
 * monotonic_ms() above still steps in 10 ms because its ABI says so. These read
 * the real clock: nanoseconds, from a calibrated TSC when the machine has one.
 *
 * Which id to pass:
 *   LOGIT_CLOCK_MONOTONIC   intervals, timeouts, frame pacing. Never backwards.
 *   LOGIT_CLOCK_REALTIME    "what time is it", file stamps, certificate `now`.
 *                           Jumps when the wall clock is set -- do not subtract.
 *   LOGIT_CLOCK_*_CPUTIME_ID  CPU actually consumed, not elapsed. Sampled at the
 *                           tick, so it is 10 ms granular; the others are not. */
static inline int clock_gettime_ns(int clock_id, struct logit_timespec *ts)
{ return (int)_sys(SYS_CLOCK_GETTIME, clock_id, (long)ts, 0); }

static inline unsigned long long monotonic_ns(void)
{ struct logit_timespec ts = { 0, 0 };
  if (clock_gettime_ns(LOGIT_CLOCK_MONOTONIC, &ts) < 0) return 0;
  return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec; }

/* Sleep. Really sleeps: the core idles and every other thread runs, instead of
 * the poll-the-clock-and-yield loop /bin/sleep has to do today. */
static inline int sys_nanosleep(long sec, long nsec)
{ struct logit_timespec req = { sec, nsec }; return (int)_sys(SYS_NANOSLEEP, (long)&req, 0, 0); }
static inline int sys_sleep_ms(long ms)
{ return sys_nanosleep(ms / 1000, (ms % 1000) * 1000000L); }

/* Which clocksource is live, how fast it was calibrated, and how often the
 * cross-core monotonicity clamp had to intervene. `set_source` switches it
 * (0 = tsc, 1 = pit, -1 = query only). */
static inline int clock_info(struct logit_clockinfo *ci, int set_source)
{ return (int)_sys(SYS_CLOCK_INFO, (long)ci, set_source, 0); }

/* --- M29 audio -------------------------------------------------------------
 * See the long note in include/abi/logit_abi.h for the shape and why. In brief:
 * declare the format YOU have, write short, and let the kernel resample, mix
 * and clock it out.
 *
 * snd_info() first: it returns 0 on a machine with no sound card, and a program
 * that checks it degrades to silence instead of treating SND_E_NODEV as an
 * error worth dying over. */
static inline int snd_info(struct logit_sndinfo *si)
{ return (int)_sys(SYS_SND_INFO, (long)si, 0, 0); }

static inline int snd_open(struct logit_sndfmt *f)
{ return (int)_sys(SYS_SND_OPEN, (long)f, 0, 0); }

/* Convenience: the common case, s16 interleaved, kernel-default buffering. */
static inline int snd_open_s16(unsigned rate, unsigned channels)
{ struct logit_sndfmt f;
  f.rate = rate; f.channels = (unsigned short)channels;
  f.format = SND_FMT_S16; f.buffer_ms = 0; f.flags = 0;
  return snd_open(&f); }

/* Bytes ACCEPTED, which may be less than `bytes`. Parks the thread when the
 * ring is full unless the stream was opened SND_F_NONBLOCK. */
static inline int snd_write(int h, const void *buf, int bytes)
{ return (int)_sys(SYS_SND_WRITE, h, (long)buf, bytes); }

static inline int snd_avail(int h) { return (int)_sys(SYS_SND_AVAIL, h, 0, 0); }
static inline int snd_close(int h, int drain) { return (int)_sys(SYS_SND_CLOSE, h, drain, 0); }
static inline int snd_state(int h, struct logit_sndstate *st)
{ return (int)_sys(SYS_SND_STATE, h, (long)st, 0); }

/* Write it all, however many short writes that takes. Returns bytes written,
 * or the negative SND_E_* that stopped it. */
static inline int snd_write_all(int h, const void *buf, int bytes)
{ const char *p = (const char *)buf; int off = 0;
  while (off < bytes) {
      int k = snd_write(h, p + off, bytes - off);
      if (k < 0) return k;
      if (k == 0) { sys_sleep_ms(2); continue; }   /* only when SND_F_NONBLOCK */
      off += k;
  }
  return off; }

#endif /* LOGIT_USERLIB_H */
