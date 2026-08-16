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
/* Block until an event arrives (or `ms` elapses; 0 = no timeout). Prefer this
 * to poll_event+yield: the spin costs two syscalls per iteration and the BKL
 * with them, and it is 98% of this machine's kernel entries. */
static inline int  wait_event_ms(struct logit_event *e, int ms) { return (int)_sys(SYS_WAIT_EVENT, (long)e, ms, 0); }
/* Sleep until this window has an event, or `ms` elapses (0 = no timeout).
 * Does NOT consume it -- the caller's existing poll_event() drain runs next.
 * This is the one-line replacement for the sys_yield() at the bottom of an
 * event loop, and the whole reason the syscall accepts a NULL event. */
static inline void wait_idle(int ms) { _sys(SYS_WAIT_EVENT, 0, ms, 0); }
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

/* --- entropy (SYS_GETRANDOM) ------------------------------------------------
 * The kernel's SHA-256 Hash_DRBG, reachable from ring 3. Use this and nothing
 * else for anything that must be unpredictable -- there is no /dev/urandom
 * here, and every hand-rolled PRNG in this tree that was seeded from a clock
 * was, in the end, predictable.
 *
 * THE LOOP IS THE POINT. The syscall clamps one call at GRND_MAX (4096) and
 * returns the clamped count, exactly like Linux's getrandom(); a caller that
 * treats the return value as success/failure gets a key with a zero tail above
 * 4 KiB and does not find out until it matters. This wrapper never short-reads:
 * it returns 0 for "buf is full" and -1 for "the kernel refused", and there is
 * no third answer to mishandle. */
static inline int getrandom_bytes(void *buf, int len)
{
    unsigned char *p = (unsigned char *)buf;
    int done = 0;
    while (done < len) {
        long n = _sys(SYS_GETRANDOM, (long)(p + done), (long)(len - done), 0);
        if (n <= 0) return -1;
        done += (int)n;
    }
    return 0;
}

/* 1 if the DRBG is backed by RDSEED/RDRAND, 0 if it fell back to rdtsc. Ask
 * before generating a LONG-TERM key; a request id does not care. */
static inline int getrandom_strong(void) { return (int)_sys(SYS_GETRANDOM, 0, 0, 0); }

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

/* --- memory ---------------------------------------------------------------
 * Reserve address space; the pages appear when you touch them. This is what
 * lets an allocator grow instead of living inside whatever arena its program
 * happened to link with.
 *
 * Returns 0 on failure, so the idiom is `p = sys_mmap(n, ...); if (!p) ...`
 * rather than checking for MAP_FAILED. */
static inline void *sys_mmap(unsigned long len, int prot)
{ return (void *)_sys(SYS_MMAP, (long)len, prot, 0); }

static inline void *sys_mmap_at(unsigned long len, int prot, void *hint)
{ return (void *)_sys(SYS_MMAP, (long)len, prot, (long)hint); }

static inline int sys_munmap(void *addr, unsigned long len)
{ return (int)_sys(SYS_MUNMAP, (long)addr, (long)len, 0); }

/* The machine's memory in numbers. Zero-fill the struct first: it grows. */
static inline int sys_meminfo(struct logit_meminfo *mi)
{ return (int)_sys(SYS_MEMINFO, (long)mi, 0, 0); }

/* --- the clipboard --------------------------------------------------------
 *
 * The store is the kernel's and outlives every process, including the one that
 * filled it. Read the comments above SYS_CLIP_SET in include/abi/logit_abi.h --
 * particularly that a payload over clip_max() is REFUSED rather than truncated,
 * and that clip_get never hands back a partial UTF-8 character. */
static inline int clip_set(int flavour, const void *buf, int len)
{ return (int)_sys(SYS_CLIP_SET, flavour & 0xFFFF, (long)buf, len); }

/* Same, but adds this flavour to the CURRENT content instead of replacing it --
 * how one copy publishes text and HTML for the same selection. */
static inline int clip_add(int flavour, const void *buf, int len)
{ return (int)_sys(SYS_CLIP_SET, (flavour & 0xFFFF) | (CLIP_SET_ADD << 16), (long)buf, len); }

/* Copies at most `max` BYTES (no terminator is written; `max` is not a string
 * size). Returns the count, or a negative CLIP_E_*. Compare with clip_len() to
 * learn whether you got all of it. */
static inline int clip_get(int flavour, void *buf, int max)
{ return (int)_sys(SYS_CLIP_GET, flavour, (long)buf, max); }

static inline int clip_flavours(void) { return (int)_sys(SYS_CLIP_INFO, CLIP_Q_FLAVOURS, 0, 0); }
static inline int clip_len(int flavour) { return (int)_sys(SYS_CLIP_INFO, CLIP_Q_LEN, flavour, 0); }
static inline int clip_serial(void)  { return (int)_sys(SYS_CLIP_INFO, CLIP_Q_SERIAL, 0, 0); }
static inline int clip_owner(void)   { return (int)_sys(SYS_CLIP_INFO, CLIP_Q_OWNER, 0, 0); }
static inline int clip_max(void)     { return (int)_sys(SYS_CLIP_INFO, CLIP_Q_MAX, 0, 0); }

/* --- notifications --------------------------------------------------------
 * Returns at once and always. It does not block, does not take focus and does
 * not tell you whether anyone read it -- that is the difference between this
 * and a dialog. -> the notification's id (>= 1), 0 if the ring is momentarily
 * full, or -1 on a bad pointer. */
static inline int notify(const char *title, const char *body, int level)
{ return (int)_sys(SYS_NOTIFY, (long)title, (long)body, level); }

/* --- settings -------------------------------------------------------------
 * The machine's memory of its user. See the block above SYS_SETTING_GET in
 * logit_abi.h, and c/kernel/core/settings.h for the format and the argument.
 *
 * Nothing here can fail in a way an app has to handle: a key that is missing,
 * unparseable or out of range reads as its built-in default. setting_int()
 * takes the default the CALLER wants for a key the kernel has no schema for --
 * which is how another line stores its own state here without a kernel change.
 *
 * A set() does NOT go to disk unless you say so. Set several keys with commit
 * = 0 and finish with setting_commit(): one whole-file write, one LogitFS
 * transaction, one atomic replacement -- rather than N of them. */
static inline int setting_get(const char *key, char *buf, int max)
{ return (int)_sys(SYS_SETTING_GET, (long)key, (long)buf, max); }

static inline int setting_set(const char *key, const char *val, int commit)
{ return (int)_sys(SYS_SETTING_SET, (long)key, (long)val, commit ? 1 : 0); }

static inline int setting_enum(int i, struct logit_setting *out)
{ return (int)_sys(SYS_SETTING_ENUM, i, (long)out, 0); }

static inline int setting_commit(void) { return (int)_sys(SYS_SETTING_CTL, SETCTL_COMMIT, 0, 0); }
static inline int setting_reset(void)  { return (int)_sys(SYS_SETTING_CTL, SETCTL_RESET, 0, 0); }
static inline int setting_count(void)  { return (int)_sys(SYS_SETTING_CTL, SETCTL_COUNT, 0, 0); }
static inline int setting_diag(void)   { return (int)_sys(SYS_SETTING_CTL, SETCTL_DIAG, 0, 0); }
static inline int setting_reload(void) { return (int)_sys(SYS_SETTING_CTL, SETCTL_RELOAD, 0, 0); }
/* The change notification: a counter that bumps on every commit. Poll it once
 * a frame -- it is one syscall returning one integer, which is cheaper for
 * every app that does not care than an event broadcast would be. */
static inline int setting_gen(void)    { return (int)_sys(SYS_SETTING_CTL, SETCTL_GEN, 0, 0); }
static inline int setting_kv_count(void) { return (int)_sys(SYS_SETTING_CTL, SETCTL_KVCOUNT, 0, 0); }
/* Fills `buf` with "key=value" for stored key `i`, INCLUDING keys the kernel
 * has no schema for. Returns the length, or -1 past the end. */
static inline int setting_kv_at(int i, char *buf)
{ return (int)_sys(SYS_SETTING_CTL, SETCTL_KVAT, (long)buf, i); }
static inline int setting_selftest(void) { return (int)_sys(SYS_SETTING_CTL, SETCTL_SELFTEST, 0, 0); }
/* What this store will accept, from the store itself. Worth calling before
 * deciding your data belongs here: it is for SETTINGS, and bulk (a history, a
 * log, a cache) belongs in files with only its switch kept here. */
static inline int setting_limit(int which)
{ return (int)_sys(SYS_SETTING_CTL, SETCTL_LIMITS, which, 0); }

/* Typed convenience reads. Parsing here rather than in the kernel keeps the
 * syscall surface to strings, which is also what makes the file editable. */
static inline int setting_int(const char *key, int def)
{
    char b[80];
    if (setting_get(key, b, (int)sizeof b) <= 0) return def;
    int i = 0, neg = 0, base = 10, digits = 0;
    long v = 0;
    if (b[i] == '-') { neg = 1; i++; }
    if (b[i] == '0' && (b[i + 1] == 'x' || b[i + 1] == 'X')) { base = 16; i += 2; }
    for (; b[i]; i++) {
        int d;
        if (b[i] >= '0' && b[i] <= '9') d = b[i] - '0';
        else if (base == 16 && b[i] >= 'a' && b[i] <= 'f') d = b[i] - 'a' + 10;
        else if (base == 16 && b[i] >= 'A' && b[i] <= 'F') d = b[i] - 'A' + 10;
        else return def;                 /* trailing garbage: the default */
        v = v * base + d;
        if (++digits > 18) return def;
    }
    return digits ? (int)(neg ? -v : v) : def;
}


/* --- server sockets: connections this machine ANSWERS ----------------------
 *
 * The wrappers for SYS_SOCKET..SYS_SOCKSTAT. See the long note above
 * SYS_SOCKET in logit_abi.h for the blocking model; the short version is that
 * sys_accept() and sys_read() on a socket fd BLOCK (the thread parks, it does
 * not spin) unless sys_set_nonblock() has been called on the descriptor, and
 * that there is no poll() to wait on several at once.
 *
 * An accepted fd is an ORDINARY fd: sys_read/sys_write/sys_close/sys_dup2 all
 * work on it and a fork inherits it. That is the point. */

static inline int sys_socket(int domain, int type, int protocol)
{ return (int)_sys(SYS_SOCKET, domain, type, protocol); }

static inline int sys_bind(int fd, const struct logit_sockaddr *a)
{ return (int)_sys(SYS_BIND, fd, (long)a, 0); }

static inline int sys_listen(int fd, int backlog)
{ return (int)_sys(SYS_LISTEN, fd, backlog, 0); }

/* `peer` may be NULL. `timeout_ms` 0 = wait indefinitely; it is ignored on a
 * non-blocking descriptor, which returns LSK_E_AGAIN at once. */
static inline int sys_accept(int fd, struct logit_sockaddr *peer, int timeout_ms)
{ return (int)_sys(SYS_ACCEPT, fd, (long)peer, timeout_ms); }

static inline int sys_getsockname(int fd, struct logit_sockaddr *a)
{ return (int)_sys(SYS_GETSOCKNAME, fd, (long)a, 0); }

static inline int sys_setsockopt(int fd, int level, int optname, long value)
{ return (int)_sys(SYS_SETSOCKOPT, fd,
                   ((long)(level & 0xFFFF) << 16) | (optname & 0xFFFF), value); }

static inline int sys_shutdown(int fd, int how)
{ return (int)_sys(SYS_SHUTDOWN, fd, how, 0); }

static inline int sys_recvfrom(int fd, struct logit_dgram *d)
{ return (int)_sys(SYS_RECVFROM, fd, (long)d, 0); }

static inline int sys_sendto(int fd, const struct logit_dgram *d)
{ return (int)_sys(SYS_SENDTO, fd, (long)d, 0); }

/* One SOCKSTAT_* counter. The reason a test can assert a limit FIRED rather
 * than assert the machine did not crash. */
static inline long sys_sockstat(int what)
{ return _sys(SYS_SOCKSTAT, what, 0, 0); }

/* Fill an address for bind(). Port and IP are HOST order throughout this ABI. */
static inline void sockaddr_set(struct logit_sockaddr *a, unsigned ip, int port)
{ a->family = LOGIT_AF_INET; a->port = (unsigned short)port; a->addr = ip; }

/* --- M32 identity (150-159) -------------------------------------------------
 * geteuid/getegid are aliases of getuid/getgid and always will be until this
 * filesystem grows a setuid bit; see the block comment in logit_abi.h. */
static inline int sys_getuid(void)  { return (int)_sys(SYS_GETUID, 0, 0, 0); }
static inline int sys_geteuid(void) { return (int)_sys(SYS_GETEUID, 0, 0, 0); }
static inline int sys_getgid(void)  { return (int)_sys(SYS_GETGID, 0, 0, 0); }
static inline int sys_getegid(void) { return (int)_sys(SYS_GETEGID, 0, 0, 0); }

/* Root only, and one-way: a process that has dropped to a uid cannot go back,
 * and cannot even re-set the uid it already has. */
static inline int sys_setuid(unsigned uid) { return (int)_sys(SYS_SETUID, (long)uid, 0, 0); }
static inline int sys_setgid(unsigned gid) { return (int)_sys(SYS_SETGID, (long)gid, 0, 0); }

/* n == 0 with list == 0 asks HOW MANY, and never fails for want of room. */
static inline int sys_getgroups(int n, unsigned *list)
{ return (int)_sys(SYS_GETGROUPS, n, (long)list, 0); }
static inline int sys_setgroups(int n, const unsigned *list)
{ return (int)_sys(SYS_SETGROUPS, n, (long)list, 0); }

/* What /bin/login calls once it has checked a password: establishes the login
 * session (the credential every process that was never told inherits) AND the
 * caller's own, in one call, because neither order works as two. Root only. */
static inline int sys_setsession(unsigned uid, unsigned gid)
{ return (int)_sys(SYS_SETSESSION, (long)uid, (long)gid, 0); }
static inline int sys_getsession(int which)   /* 0 = uid, 1 = gid */
{ return (int)_sys(SYS_GETSESSION, which, 0, 0); }

#endif /* LOGIT_USERLIB_H */
