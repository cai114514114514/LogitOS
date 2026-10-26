#ifndef LOGIT_WM_DRAW2_H
#define LOGIT_WM_DRAW2_H
/* GUI ABI v2, kernel side: the router (wm_draw2.c), the per-module handlers
 * (wm_draw2_path.c, wm_draw2_state.c, wm_draw2_text.c, wm_draw2_fx.c,
 * wm_backdrop.c) and the hooks wm.c calls. The ABI itself -- numbers, structs,
 * feature bits -- is include/abi/logit_abi.h, "GUI ABI v2".
 *
 * SHAPE. wm.c's SYS_GUI_* case for a v2 number finds the caller's window,
 * targets its surface (fb_target), fills a struct wm2_env and calls
 * wm2_dispatch(). A handler never touches wm.c state: everything it may use
 * arrives in the env, which is what lets each module be host-tested against a
 * fake env (tests/unit/gui_abi_v2_test.c and the per-module tests).
 *
 * STATUS. Every handler returns a long ol_status (openlogit.h): OL_OK, or
 * OL_ARGUMENT for a malformed call (a wrong `size` first of all), or
 * OL_UNSUPPORTED while its LOGIT_GUI2_F_* bit is clear.
 *
 * FEATURE BITS. Each module exports one const wm2_<mod>_info holding the
 * LOGIT_GUI2_F_* bits it implements; the router ORs them into
 * SYS_GUI2_INFO(0). A module sets a bit only by changing its own constant,
 * in the same change that implements the call -- never anywhere else.
 * wm2_carets_info lives in text.c, next to the code that will answer it.
 *
 * BACKDROP FOREGROUND SEPARATION (decided here; logit_abi.h has the app-side
 * contract): a marker-byte ink mask. SYS_GUI_BACKDROP fills its region's
 * surface pixels with fill_rgb | LOGIT_BACKDROP_MARK << 24; the compositor
 * shows the material wherever a window pixel carries MARK and the app pixel
 * elsewhere. That only holds if NO OTHER PATH can store a high byte of
 * LOGIT_BACKDROP_MARK into a window surface. Audit of the app-controlled
 * writers as of this header (wm-backdrop-regions repeats and enforces it):
 *   - SYS_GUI_CLEAR passes the raw app value (wm.c, case SYS_GUI_CLEAR) and
 *     ol_display_clear / ol_display_fill_rect store it unchanged
 *     (openlogit_display.c), so an app colour 0x01xxxxxx forges MARK TODAY.
 *     SYS_GUI_RECT has the same shape. Both must mask with & 0xFFFFFF before
 *     LOGIT_GUI2_F_BACKDROP is set.
 *   - The display_rgb-packed draws (RRECT, TEXT, TEXT_MONO, TEXT_RUN, ICON,
 *     GLASS, BLIT's RGBA unpack) rebuild the pixel from channels and write a
 *     high byte of 0.
 *   - ol_display_blit_surface copies whole pixels, high byte included. The
 *     compositor's blits READ window surfaces into the back buffer and never
 *     write a window surface; any future surface-to-surface copy into a
 *     window must mask or be audited.
 *   - The v2 handlers must mask any raw app colour they store.
 *   Only wm2_backdrop may write MARK. Its known limitation: AA fringes of
 *   app ink inside a region are blended against fill_rgb.
 *
 * LOCKING. wm.c calls the router with graphics_lock held. For the pixel
 * calls (is_draw_call) wm_lock is dropped across the dispatch, exactly as
 * for the legacy draw calls; for the others (INFO, CLIP_PUSH/POP,
 * GROUP_BEGIN, BACKDROP_CLEAR) wm_lock is also held. Both are sleeping
 * mutexes, so a handler may kmalloc through env->alloc and copy user memory
 * through env->copyin. */
#include "openlogit_display.h"   /* struct ol_display, enum ol_status */
#include "logit_abi.h"

/* Half-open, DEVICE pixels. Moved here from wm.c so wm_backdrop.c and
 * wm_cover.h share the one definition with the damage machinery. */
struct drect { int x0, y0, x1, y1; };

/* Per-window side tables are indexed by wm.c's window slot; wm.c asserts
 * MAXWIN == WM2_MAX_WINDOWS. */
#define WM2_MAX_WINDOWS 16

struct wm2_env {
    struct ol_display *d;     /* bound to the window surface (fb_display_cur);
                               * valid until the next fb_target or clip change */
    int scale_pct;            /* fb_scale() snapshot: convert with wm2_scale.h */
    int surf_w, surf_h;       /* the surface, device pixels */
    int slot;                 /* window slot, 0..WM2_MAX_WINDOWS-1 */
    /* Copy n bytes from user memory; 0 on success, < 0 on a fault. */
    int (*copyin)(void *dst, const void *usrc, unsigned long n);
    /* fb_clip_push / fb_clip_pop on the window surface (device pixels,
     * half-open). OL_UNSUPPORTED until fb-clip-stack; propagate it. */
    int (*clip_push)(void *opaque, int x0, int y0, int x1, int y1);
    void (*clip_pop)(void *opaque);
    void *(*alloc)(unsigned long n);
    void (*release)(void *p);
    void *opaque;             /* passed back to clip_push / clip_pop */
};

/* Copy a size-prefixed v2 struct from user memory. Reads the leading
 * `unsigned size` first and refuses (OL_ARGUMENT) a null pointer, a fault or
 * any size other than the kernel's own sizeof -- so a smaller struct from an
 * older or confused caller is never over-read. OL_OK on success. */
static inline long wm2_copy_struct(const struct wm2_env *e, void *dst, long uptr,
                                   unsigned want)
{
    unsigned size = 0;
    if (!uptr || !e->copyin) return OL_ARGUMENT;
    if (e->copyin(&size, (const void *)uptr, sizeof size) < 0) return OL_ARGUMENT;
    if (size != want) return OL_ARGUMENT;
    if (e->copyin(dst, (const void *)uptr, want) < 0) return OL_ARGUMENT;
    if (*(const unsigned *)dst != want) return OL_ARGUMENT;   /* changed under us */
    return OL_OK;
}

/* The router: SYS_GUI2_INFO itself, everything else to its module. */
long wm2_dispatch(long num, struct wm2_env *e, long a, long b, long c);

/* Handlers, one per call (b and c are unused where the call takes less). */
long wm2_path(struct wm2_env *e, long a, long b, long c);          /* wm_draw2_path.c */
long wm2_clip_push(struct wm2_env *e, long a, long b, long c);     /* wm_draw2_state.c */
long wm2_clip_pop(struct wm2_env *e, long a, long b, long c);
long wm2_group_begin(struct wm2_env *e, long a, long b, long c);
long wm2_group_end(struct wm2_env *e, long a, long b, long c);
long wm2_blit2(struct wm2_env *e, long a, long b, long c);
long wm2_text_run2(struct wm2_env *e, long a, long b, long c);     /* wm_draw2_text.c */
long wm2_shadow2(struct wm2_env *e, long a, long b, long c);       /* wm_draw2_fx.c */
long wm2_glass2(struct wm2_env *e, long a, long b, long c);
long wm2_backdrop(struct wm2_env *e, long a, long b, long c);      /* wm_backdrop.c */
long wm2_backdrop_clear(struct wm2_env *e, long a, long b, long c);

/* Router hooks, called by wm.c.
 *   wm2_win_reset: window create, destroy and surface resize -- the slot's
 *     v2 state (stacks, arenas, backdrop regions) belongs to the old canvas.
 *   wm2_win_flush: SYS_GUI_FLUSH / SYS_GUI_FLUSH_RECT, before the present;
 *     closes whatever the frame left open. */
void wm2_win_reset(int slot);
void wm2_win_flush(struct wm2_env *e);
/* ...and the module hooks they fan out to. */
void wm2_state_win_reset(int slot);          /* wm_draw2_state.c */
void wm2_state_win_flush(struct wm2_env *e);
void wm2_backdrop_win_reset(int slot);       /* wm_backdrop.c */

/* Levels auto-closed at this slot's flushes (SYS_GUI2_INFO(1)). */
unsigned wm2_state_err_count(int slot);

/* Feature bits, one constant per module (see FEATURE BITS above). */
extern const unsigned wm2_path_info, wm2_state_info, wm2_text_info,
                      wm2_fx_info, wm2_backdrop_info, wm2_carets_info;

/* Backdrop regions, for the compositor (wm-backdrop-regions). `r` is in
 * window-surface DEVICE pixels; radius[] in device pixels, TL TR BR BL. */
struct wm2_region {
    int id;
    struct drect r;
    int radius[4];
    unsigned flags;
};
/* Copy up to `max` of the slot's regions to out; returns how many. */
int wm_backdrop_regions(int slot, struct wm2_region *out, int max);
/* Render the material of every region of window `win_slot` (whose surface
 * origin is at back-buffer (dx,dy)) intersected with damage rect R, on the
 * display bound to the back buffer, BEFORE the window's content blit. */
void wm_backdrop_composite(const struct drect *R, int win_slot, int dx, int dy,
                           struct ol_display *d);
#endif /* LOGIT_WM_DRAW2_H */
