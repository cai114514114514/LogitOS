/* Host AUI harness: a mock window-manager transport for ring-3 aui.c.
 *
 * aui.c talks to the compositor through the gui_* wrappers in c/apps/logit.h,
 * every one of which ends in _sys(). Built with -DLOGIT_SYS_HOOK=aui_host_sys
 * (the seam build-prewire added), those calls land in aui_host_sys() here
 * instead of trapping, and each legacy call is executed by the SAME
 * ol_display_* function the matching `case SYS_GUI_*` in c/kernel/gui/wm.c
 * calls -- same point->device scaling, same argument unpacking
 * (include/abi/logit_pack.h). The result is a pixel buffer a host test can
 * diff against a golden, in milliseconds, without booting anything.
 *
 * Nothing else in the backlog may invent its own mock transport: adoption
 * items add v2 syscall handlers as tests/unit/aui_host/v2_<name>.c (see
 * AUI_HOST_V2 below) and the harness recipe picks them up by wildcard.
 */
#ifndef AUI_HOST_H
#define AUI_HOST_H

#include <stdint.h>
#include "openlogit_display.h"
#include "logit_abi.h"

/* v2 numbers 210-221 are KNOWN. gui-abi-v2 lands in the same wave as this
 * harness, so a name is used when logit_abi.h has it and a local #define of
 * the same number otherwise. */
#ifndef SYS_GUI2_INFO
#define SYS_GUI2_INFO 210
#endif
#define AUI_HOST_V2_FIRST 210
#define AUI_HOST_V2_LAST  221
#define AUI_HOST_UNSUPPORTED (-3)      /* OL_UNSUPPORTED */

#define AUI_HOST_NSYS 512              /* syscall numbers are counted below this */
#define AUI_HOST_DMG_MAX 256
#define AUI_HOST_EVQ 32

struct aui_host_rect { int x, y, w, h; };      /* DEVICE pixels */

struct aui_host {
    int pct;                           /* backing scale, percent */
    int win_w_pt, win_h_pt;            /* content size, points */
    int w, h;                          /* surface size, device pixels */
    uint32_t *px;                      /* BGRX, w*h */
    struct ol_display disp;            /* bound to px (allocate=malloc, release=free) */
    int clip_on, clx0, cly0, clx1, cly1;   /* target clip, device pixels */
    char title[64];

    /* configurable world */
    unsigned long long clock_ms;       /* fake monotonic clock */
    struct logit_time wall;            /* SYS_GET_TIME answer */
    int dark;                          /* SYS_UI_DARK */
    int focused, minimized;            /* SYS_GUI_WIN_STATE */
    int screen_w_pt, screen_h_pt;      /* SYS_SCREEN_INFO */
    unsigned info;                     /* SYS_GUI2_INFO answer */
    struct logit_event evq[AUI_HOST_EVQ];
    int evq_head, evq_n;

    /* per-frame stats; aui_host_stats_reset() clears them */
    unsigned long long calls[AUI_HOST_NSYS];       /* syscall count per number */
    unsigned long long unhandled[AUI_HOST_NSYS];   /* numbers nobody handled */
    unsigned long long px_written;     /* device pixels written (clipped bboxes) */
    struct aui_host_rect dmg[AUI_HOST_DMG_MAX];    /* FLUSH / FLUSH_RECT log */
    int ndmg;
};

/* Lifetime. The host most recently created is the one aui_host_sys() serves. */
struct aui_host *aui_host_new(int win_w_pt, int win_h_pt, int pct);
void aui_host_free(struct aui_host *);
extern struct aui_host *aui_host_current;

/* The transport. Its signature is what LOGIT_SYS_HOOK requires. */
long aui_host_sys(long n, long a, long b, long c);
/* The built-in handling alone, for a v2 handler that overrides a legacy
 * number and wants to chain to the default. */
long aui_host_sys_builtin(struct aui_host *h, long n, long a, long b, long c);

/* Extra handlers. A registered handler runs BEFORE the built-in decode. */
typedef long (*aui_host_fn)(struct aui_host *, long a, long b, long c);
void aui_host_on(long num, aui_host_fn fn);
void aui_host_handlers_clear(void);

/* AUI_HOST_V2(name, info_bits, register_fn): in tests/unit/aui_host/v2_<name>.c,
 *   static void reg(struct aui_host *h) { aui_host_on(SYS_GUI2_..., fn); }
 *   AUI_HOST_V2(name, 0x1, reg)
 * register_fn runs once per host in v2 mode (aui_host_enable_v2), and
 * info_bits is ORed into the SYS_GUI2_INFO answer. */
struct aui_host_v2 {
    const char *name;
    unsigned info_bits;
    void (*reg)(struct aui_host *);
    struct aui_host_v2 *next;
};
void aui_host_v2_register(struct aui_host_v2 *);
#define AUI_HOST_V2(name_, bits_, fn_)                                         \
    static struct aui_host_v2 aui_host_v2_rec_##name_ = { #name_, (bits_), (fn_), 0 }; \
    __attribute__((constructor)) static void aui_host_v2_ctor_##name_(void)    \
    { aui_host_v2_register(&aui_host_v2_rec_##name_); }
/* Switch a host to v2 mode: INFO = OR of the registered bits, handlers on.
 * Returns that INFO mask (0 until the first v2_*.c lands). */
unsigned aui_host_enable_v2(struct aui_host *);

/* Fake clock and scripted input. */
void aui_host_clock_set(unsigned long long ms);
void aui_host_clock_advance(unsigned long long ms);
void aui_host_push_event(const struct logit_event *);

/* Stats. */
void aui_host_stats_reset(struct aui_host *);
int  aui_host_unhandled_total(const struct aui_host *);   /* distinct unexpected numbers */

/* Scaling exactly as wm.c does (S(v) = fb_pt, PT(v) = fb_dev2pt) at an
 * explicit pct. aui_host_pt is a verbatim copy of fb_pt (fb.c:184-188) and
 * aui_host_scale_check.c proves it cannot drift. */
int aui_host_pt(int points, int pct);
int aui_host_dev2pt(int px, int pct);

/* Output. Binary P6, 8 bits per channel. 0 on success. */
int aui_host_write_ppm(const struct aui_host *, const char *path);

/* Deterministic text, shared with tests that want to predict widths: advance in
 * DEVICE pixels of a UTF-8 run at device size px. */
int aui_host_text_advance(const char *s, int len, int px);

/* ---- scenes (aui_host_scenes.c) -------------------------------------------
 * Each is drawn like an app frame (aui_begin ... aui_end) at one scale, in a
 * window of at most 160x120 points. */
#define AUI_HOST_NSCENES 4
#define AUI_HOST_NPCT 4
extern const int aui_host_pcts[AUI_HOST_NPCT];     /* 100 125 150 200 */
const char *aui_host_scene_name(int scene);        /* "s1_controls" ... */
void aui_host_scene_size(int scene, int *w_pt, int *h_pt);
/* Draw scene `scene` into h (already created at the right size and pct). */
void aui_host_scene_draw(int scene, struct aui_host *h);

#endif
