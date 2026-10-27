/* ui_aui_core_hook.h -- the syscall recorder shared by ui_aui_core_test.c and
 * ui_aim_test.c (make test-ui-aui-core).
 *
 * Both tests build the REAL ring-3 aui.c with -DLOGIT_SYS_HOOK=core_sys, so
 * every gui_* / ol_window_* call lands in core_sys() below instead of
 * trapping. Nothing is drawn: each v2 call's struct is copied into a log (the
 * call SHAPE is what these tests assert), FLUSH_RECT rectangles are kept, and
 * SYS_WRITE to fd 1 (the serial log the aims go to) is captured as text. The
 * world is configurable: GUI2_INFO bits, backing scale, dark mode, the
 * reduce_transparency / reduce_motion settings, the monotonic clock.
 *
 * aui.c keeps its scale, capabilities and theme in file-scope statics, so each
 * case runs in a forked child (core_case). Exits non-zero on any failure. */
#ifndef UI_AUI_CORE_HOOK_H
#define UI_AUI_CORE_HOOK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aui.h"
#include "openlogit_glass_material.h"

#define CH_LOG 32
#define CH_OUT 65536

struct core_world {
    unsigned info;                 /* SYS_GUI2_INFO(0) feature bits */
    int pct, dark, reduce_t, reduce_m;
    unsigned long long ms;
    /* logs, reset by ch_reset() */
    int nglass, nbd, nbdclr, nblit2, nshadow, npath, nblit, nrect;
    struct logit_glass2 glass[CH_LOG];
    struct logit_backdrop2 bd[CH_LOG];
    int bdclr[CH_LOG];
    struct logit_blit2 blit2[CH_LOG];
    struct logit_shadow2 shadow[CH_LOG];
    struct logit_paint2 paint[CH_LOG];
    struct { int x, y, w, h; } blit[CH_LOG], flush[CH_LOG];
    int nflush, nfull, nwrite;
    char out[CH_OUT];
    int nout;
};
static struct core_world W = { .pct = 100, .ms = 1000 };

static void ch_reset(void)
{
    W.nglass = W.nbd = W.nbdclr = W.nblit2 = W.nshadow = W.npath = W.nblit = W.nrect = 0;
    W.nflush = W.nfull = W.nwrite = 0;
    W.nout = 0; W.out[0] = 0;
}

static int ch_set(char *buf, int max, const char *v)
{
    int n = (int)strlen(v);
    if (n >= max) return -1;
    memcpy(buf, v, (size_t)n + 1);
    return n;
}

long core_sys(long n, long a, long b, long c)
{
    (void)c;
    switch (n) {
    case SYS_GUI2_INFO:   return a == 0 ? (long)W.info : 0;
    case SYS_SCREEN_INFO: return a == SCREEN_SCALE ? W.pct : a == SCREEN_W ? 1280 : a == SCREEN_H ? 800 : -1;
    case SYS_UI_DARK:     return W.dark;
    case SYS_MONOTONIC_MS: return (long)W.ms;
    case SYS_SETTING_GET: {
        const char *k = (const char *)a;
        if (!strcmp(k, "ui.reduce_transparency")) return ch_set((char *)b, (int)c, W.reduce_t ? "1" : "0");
        if (!strcmp(k, "ui.reduce_motion")) return ch_set((char *)b, (int)c, W.reduce_m ? "1" : "0");
        return -1;
    }
    case SYS_WRITE:
        W.nwrite++;
        if (a == 1 && c > 0 && W.nout + c < CH_OUT) {
            memcpy(W.out + W.nout, (const void *)b, (size_t)c);
            W.nout += (int)c; W.out[W.nout] = 0;
        }
        return c;
    case SYS_GUI_GLASS2:
        if (W.nglass < CH_LOG) memcpy(&W.glass[W.nglass], (const void *)a, sizeof W.glass[0]);
        W.nglass++;
        return (W.info & LOGIT_GUI2_F_GLASS2) ? 0 : -3;
    case SYS_GUI_BACKDROP:
        if (W.nbd < CH_LOG) memcpy(&W.bd[W.nbd], (const void *)a, sizeof W.bd[0]);
        W.nbd++;
        return (W.info & LOGIT_GUI2_F_BACKDROP) ? 0 : -3;
    case SYS_GUI_BACKDROP_CLEAR:
        if (W.nbdclr < CH_LOG) W.bdclr[W.nbdclr] = (int)a;
        W.nbdclr++;
        return 0;
    case SYS_GUI_BLIT2:
        if (W.nblit2 < CH_LOG) memcpy(&W.blit2[W.nblit2], (const void *)a, sizeof W.blit2[0]);
        W.nblit2++;
        return (W.info & LOGIT_GUI2_F_BLIT2) ? 0 : -3;
    case SYS_GUI_SHADOW2:
        if (W.nshadow < CH_LOG) memcpy(&W.shadow[W.nshadow], (const void *)a, sizeof W.shadow[0]);
        W.nshadow++;
        return (W.info & LOGIT_GUI2_F_SHADOW2) ? 0 : -3;
    case SYS_GUI_PATH:
        if (W.npath < CH_LOG) memcpy(&W.paint[W.npath], (const void *)b, sizeof W.paint[0]);
        W.npath++;
        return (W.info & LOGIT_GUI2_F_PATH) ? 0 : -3;
    case SYS_GUI_BLIT: {
        const struct logit_blit *bl = (const struct logit_blit *)a;
        if (W.nblit < CH_LOG) { W.blit[W.nblit].x = bl->x; W.blit[W.nblit].y = bl->y;
                                W.blit[W.nblit].w = bl->w; W.blit[W.nblit].h = bl->h; }
        W.nblit++;
        return 0;
    }
    case SYS_GUI_RECT: W.nrect++; return 0;
    case SYS_GUI_FLUSH: W.nfull++; return 0;
    case SYS_GUI_FLUSH_RECT:
        if (W.nflush < CH_LOG) {
            W.flush[W.nflush].x = (short)((a >> 16) & 0xFFFF); W.flush[W.nflush].y = (short)(a & 0xFFFF);
            W.flush[W.nflush].w = (short)((b >> 16) & 0xFFFF); W.flush[W.nflush].h = (short)(b & 0xFFFF);
        }
        W.nflush++;
        return 0;
    default:
        return 0;
    }
}

static int bad;
#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); fflush(stdout); bad = 1; } while (0)

/* Run fn in a forked child (fresh aui statics); a crash is a failure too. */
static int core_failures;
static void core_case(const char *name, void (*fn)(int), int arg)
{
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) { bad = 0; fn(arg); fflush(stdout); _exit(bad ? 1 : 0); }
    int st = 0;
    waitpid(p, &st, 0);
    if (!WIFEXITED(st)) { printf("FAIL %s crashed (signal %d)\n", name, WIFSIGNALED(st) ? WTERMSIG(st) : 0); core_failures++; }
    else if (WEXITSTATUS(st)) core_failures++;
}

/* One frame: aui_begin, the caller's drawing, aui_end. */
typedef void (*core_draw_fn)(void *);
static void core_frame(core_draw_fn fn, void *arg)
{
    aui_begin(AUI_BG);
    if (fn) fn(arg);
    aui_end();
}

/* The union of this frame's FLUSH_RECTs, points; 0 when there was none. */
static __attribute__((unused)) int core_flush_union(int *x0, int *y0, int *x1, int *y1)
{
    if (!W.nflush) return 0;
    *x0 = *y0 = 1 << 30; *x1 = *y1 = -(1 << 30);
    for (int i = 0; i < W.nflush && i < CH_LOG; i++) {
        if (W.flush[i].x < *x0) *x0 = W.flush[i].x;
        if (W.flush[i].y < *y0) *y0 = W.flush[i].y;
        if (W.flush[i].x + W.flush[i].w > *x1) *x1 = W.flush[i].x + W.flush[i].w;
        if (W.flush[i].y + W.flush[i].h > *y1) *y1 = W.flush[i].y + W.flush[i].h;
    }
    return 1;
}

/* fb_pt: points -> device pixels, floors (the kernel's S()). */
static __attribute__((unused)) int core_dev(int pt, int pct) { return (int)(((long long)pt * pct) / 100); }

#endif
