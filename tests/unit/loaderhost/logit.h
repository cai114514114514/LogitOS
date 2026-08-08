/* Host stand-in for c/apps/logit.h, used ONLY by tests/unit/loader_test.c.
 *
 * tests/unit/painthost/logit.h already shadows the five drawing syscalls so
 * that the REAL browser_paint.c links on the host. This shadows the rest of
 * the surface c/apps/browser/browser.c uses -- window creation, the event
 * queue, the clock, exit -- so that the REAL LOADER links too.
 *
 * That is the point: browser.c's load() is where a page's own navigation is
 * (now) followed, and until this existed the only way to exercise it was to
 * boot QEMU. The recorders below plus tests/unit/loader_fakebfetch.c replace
 * the two things a loader cannot have on the host -- a window and a network --
 * and nothing else is faked.
 *
 * The event queue is scriptable: host_post_event() enqueues, poll_event()
 * drains, and app_exit() longjmps out rather than exiting the process, so a
 * test can drive app_main() and get control back.
 */
#ifndef LOADERHOST_LOGIT_H
#define LOADERHOST_LOGIT_H

#include "../painthost/logit.h"     /* the five drawing recorders + struct paintop */
/* logit_abi.h declares struct logit_run / logit_blit identically to the
 * painthost recorder's copies. C forbids the duplicate tag even when every
 * member matches, and painthost/logit.h belongs to another test -- so the
 * ABI's copies are renamed out of the way here. Nothing in this link uses
 * them: the recorder's versions are what browser_paint.c fills in. Everything
 * else in logit_abi.h (EV_*, KEY_*, struct logit_event) is taken as-is,
 * because a hand-copied event ABI in a test is an ABI that drifts. */
#define logit_run  logit_abi_run_unused
#define logit_blit logit_abi_blit_unused
#include "logit_abi.h"              /* EV_* / KEY_* / struct logit_event */
#undef logit_run
#undef logit_blit
#include <setjmp.h>

/* ---- the window ---- */
static inline unsigned rgb(int r, int g, int b)
{ return ((unsigned)(r & 255) << 16) | ((unsigned)(g & 255) << 8) | (unsigned)(b & 255); }

extern int host_win_w, host_win_h, host_flushes;
static inline void gui_create(const char *title, int w, int h)
{ (void)title; host_win_w = w; host_win_h = h; }
static inline void gui_clear(unsigned color)
{ struct paintop *o = paint_push(OP_RECT); o->x=0; o->y=0; o->w=host_win_w; o->h=host_win_h; o->color=color; }
static inline void gui_glass(int x, int y, int w, int h, int radius,
                             int r, int g, int b, int a)
{ struct paintop *o = paint_push(OP_RRECT); o->x=x; o->y=y; o->w=w; o->h=h;
  o->radius=radius; o->color=rgb(r,g,b); o->alpha=a; }
static inline void gui_text(int x, int y, unsigned color, const char *s)
{ int n = 0; while (s[n]) n++;
  struct paintop *o = paint_push(OP_TEXT); o->x=x; o->y=y; o->px=16; o->mono=0;
  o->color=color; o->text=s; o->len=n; }
static inline void gui_flush(void) { host_flushes++; }

/* ---- the event queue ---- */
#define HOST_EVQ 64
extern struct logit_event host_evq[HOST_EVQ];
extern int host_evq_head, host_evq_tail;
static inline void host_post_event(const struct logit_event *e)
{ if (host_evq_tail < HOST_EVQ) host_evq[host_evq_tail++] = *e; }
static inline int poll_event(struct logit_event *e)
{ if (host_evq_head >= host_evq_tail) return 0; *e = host_evq[host_evq_head++]; return 1; }

/* ---- lifetime + clock ---- */
extern jmp_buf host_exit_jmp;
extern int host_exit_code, host_exited;
static inline void app_exit(int code)
{ host_exit_code = code; host_exited = 1; longjmp(host_exit_jmp, 1); }

extern unsigned long long host_clock;
static inline unsigned long long monotonic_ms(void) { return host_clock; }
static inline void sys_yield(void) { }

/* ---- the clipboard ----
 * browser.c's text fields do cut/copy/paste through the clipboard syscalls, so
 * the host build of that file needs them to resolve. An in-process buffer, not
 * a no-op: a paste that silently produced nothing would make a clipboard bug
 * invisible to a host test rather than merely untested. */
#define CLIP_F_TEXT 0
#define HOST_CLIP_MAX 4096
extern char host_clip[HOST_CLIP_MAX];
extern int  host_clip_len;
static inline int clip_set(int flavour, const void *buf, int len)
{
    (void)flavour;
    if (len < 0 || len > HOST_CLIP_MAX) return -2;
    for (int i = 0; i < len; i++) host_clip[i] = ((const char *)buf)[i];
    host_clip_len = len;
    return len;
}
static inline int clip_get(int flavour, void *buf, int max)
{
    (void)flavour;
    int n = host_clip_len < max ? host_clip_len : max;
    for (int i = 0; i < n; i++) ((char *)buf)[i] = host_clip[i];
    return n;
}

#endif /* LOADERHOST_LOGIT_H */
