#include <stdint.h>
#include <stddef.h>
#include "fb.h"
#include "openlogit_display.h"
#include "gui_sync.h"
/* Shared software-raster scratch is also reached by SVG image decoding.
 * Keep its ownership separate from window metadata: decoder -> graphics;
 * WM -> graphics -> text. No graphics-locked path enters a decoder. */
static struct gui_mutex graphics_lock = GUI_MUTEX_INIT;
void fb_graphics_lock(void) { gui_mutex_lock(&graphics_lock); }
void fb_graphics_unlock(void) { gui_mutex_unlock(&graphics_lock); }
#include "vmm.h"
#include "text.h"
#include "virtio_gpu.h"
#include "gfx.h"
#include "glass.h"        /* Open Logit's mask cache -- see corner_mask_for() below */

/* --- Multiboot2 framebuffer info tag (type 8) --- */
struct mb2_tag { uint32_t type, size; };

struct mb2_fb_tag {
    uint32_t type, size;
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t  bpp, fb_type;
    uint16_t reserved;
    uint8_t  red_pos, red_size;
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos, blue_size;
};

#define MB2_FB_TYPE_RGB 1

void *kmalloc(unsigned long);
void  kfree(void *);

static volatile uint8_t *fb_mem;
static uint32_t fb_pitch, fb_w, fb_h;
static uint8_t rpos, gpos, bpos;
static int using_gpu;             /* fb_mem is a virtio-gpu RAM resource, not VGA MMIO */
/* Physical aperture supplied by a Multiboot2 framebuffer tag.  Virtio-gpu's
 * CPU backing is ordinary RAM and deliberately has no entry here: a passive
 * PCI display observer may use this range to prove that the active scanout
 * actually belongs to one of its BARs. */
static uint64_t boot_lfb_addr, boot_lfb_bytes;

/* BKL serializes normal display calls, but the present-band IPI explicitly
 * bypasses it. A removed GPU cannot free its RAM while a late AP is copying.
 * Count only front-buffer writers (one lease per band, not per copied pixel),
 * revoke the pointer first, then wait for old leases to retire. A late worker
 * obtains NULL and never touches released backing. */
static unsigned fb_front_writers;
static unsigned fb_front_blocked, fb_front_quarantined;
static fb_native_present_fn native_present;
void fb_set_native_present(fb_native_present_fn fn)
{
    fb_graphics_lock();
    native_present = fn;
    /* Unregistering is not a hardware reset: never clear quarantine here. */
    fb_graphics_unlock();
}
static volatile uint8_t *fb_front_acquire(void)
{
    __atomic_add_fetch(&fb_front_writers, 1, __ATOMIC_SEQ_CST);
    volatile uint8_t *mem = __atomic_load_n(&fb_mem, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&fb_front_blocked, __ATOMIC_SEQ_CST) ||
        __atomic_load_n(&fb_front_quarantined, __ATOMIC_SEQ_CST)) mem = NULL;
    if (!mem) __atomic_sub_fetch(&fb_front_writers, 1, __ATOMIC_SEQ_CST);
    return mem;
}
static void fb_front_release(void)
{ __atomic_sub_fetch(&fb_front_writers, 1, __ATOMIC_SEQ_CST); }

int fb_detach_gpu(const uint32_t *backing)
{
    volatile uint8_t *current = __atomic_load_n(&fb_mem, __ATOMIC_SEQ_CST);
    if (current && (!using_gpu || current != (const volatile uint8_t *)backing)) return 0;
    __atomic_store_n(&fb_mem, NULL, __ATOMIC_SEQ_CST);
    using_gpu = 0;
#ifdef FB_DMA_HOSTTEST
    const unsigned limit = 32;
#else
    const unsigned limit = 10000000;
#endif
    for (unsigned i = 0; i < limit; i++)
        if (!__atomic_load_n(&fb_front_writers, __ATOMIC_SEQ_CST)) return 0;
    return -1;  /* caller quarantines instead of freeing under a stalled AP */
}
#ifdef FB_DMA_HOSTTEST
/* Hold the same lease a real AP takes, so the host test can deterministically
 * exercise drain timeout without relying on host thread scheduling. */
void *fb_dma_test_hold_writer(void) { return (void *)fb_front_acquire(); }
void fb_dma_test_release_writer(void) { fb_front_release(); }
#endif


static struct surface screen;     /* wraps the back buffer (the visible screen) */
static struct surface *T;         /* current draw target (defaults to screen) */
static int sdk_target_dirty=1;
static uint64_t sdk_draw_calls;

/* The clip rectangle lives ON THE CURRENT TARGET (struct surface), not in a
 * global -- so a clip set while drawing into one app's surface can never affect
 * a draw into another app's surface (the cross-app leak that left a freshly
 * opened Terminal white). */
void fb_set_clip(int x, int y, int w, int h)
{
    struct surface *s = T ? T : &screen;
    sdk_target_dirty=1;
    s->clip_on = 1; s->clx0 = x; s->cly0 = y; s->clx1 = x + w; s->cly1 = y + h;
}
void fb_clear_clip(void) { struct surface *s = T ? T : &screen; s->clip_on = 0; sdk_target_dirty=1; }

uint32_t fb_width(void)  { return fb_w; }
uint32_t fb_height(void) { return fb_h; }

int fb_boot_lfb_range(uint64_t *addr, uint64_t *bytes)
{
    if (using_gpu || !boot_lfb_addr || !boot_lfb_bytes) return 0;
    if (addr) *addr = boot_lfb_addr;
    if (bytes) *bytes = boot_lfb_bytes;
    return 1;
}

/* ---- UI scale ----------------------------------------------------------
 * See the long comment in fb.h. 100 until fb_init() picks one, so anything that
 * draws before a mode is known behaves exactly as it did before scaling existed. */
static int ui_scale = 100;

void fb_set_scale(int pct)
{
    if (pct < 100) pct = 100;          /* the UI is authored at 1x; never shrink it */
    if (pct > 400) pct = 400;
    ui_scale = pct;
}
int fb_scale(void) { return ui_scale; }

/* Floor toward negative infinity, not toward zero: a window dragged off the left
 * edge has negative content coordinates, and truncation there makes fb_pt
 * non-monotonic across 0 -- two adjacent logical columns would map to the same
 * device column and the frame would visibly shear at x=0. */
int fb_pt(int points)
{
    if (points >= 0) return points * ui_scale / 100;
    return -(((-points) * ui_scale + 99) / 100);
}
/* THE INVERSE OF A FLOOR IS NOT A FLOOR, and getting this wrong is the exact
 * shape of the classic scaled-UI bug: at 150% the point 3 lands on device pixel
 * 4, but 4*100/150 floors back to 2, so a click one row below a button's top
 * edge reports as being above it. Every third row is misattributed and the
 * widget's hit box quietly drifts from the pixels it drew.
 *
 * What is wanted is the point whose CELL contains this pixel -- the largest v
 * with fb_pt(v) <= px -- which is ceil(100*(px+1)/scale) - 1. The host test
 * (tests/unit/scale_test.c) asserts both directions of this for every scale, and
 * it is what caught the floor-inverse version. */
int fb_dev2pt(int px)
{
    int a = 100 * (px + 1), b = ui_scale;
    int q = (a >= 0) ? (a + b - 1) / b : -((-a) / b);   /* ceil, negatives too */
    return q - 1;
}
int fb_ui_px(void)    { return TEXT_UI_PX * ui_scale / 100; }
int fb_width_pt(void)  { return fb_dev2pt((int)fb_w); }
int fb_height_pt(void) { return fb_dev2pt((int)fb_h); }

/* The desktop is designed against a 1280x800 point canvas: the browser alone
 * asks for a 1180x620 window, and a logical desktop smaller than that would make
 * SYS_GUI_CREATE refuse it. So the rule is not "pick a pretty number", it is
 * "spend every pixel beyond the design canvas on DENSITY, never on shrinking the
 * UI" -- which is exactly what makes a resolution bump an improvement instead of
 * the usual make-everything-tiny regression.
 *
 * Quantised to 25% steps so the arithmetic stays predictable and a one-pixel
 * change in the reported mode cannot jitter the whole layout. */
#define DESIGN_W_PT 1280
#define DESIGN_H_PT 800
static int pick_scale(uint32_t w, uint32_t h)
{
    int sw = (int)w * 100 / DESIGN_W_PT, sh = (int)h * 100 / DESIGN_H_PT;
    int s = sw < sh ? sw : sh;
    s = (s / 25) * 25;
    if (s < 100) s = 100;              /* a mode smaller than the design canvas
                                        * stays 1x: shrinking is never the answer */
    if (s > 300) s = 300;
    return s;
}

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << rpos) | ((uint32_t)g << gpos) | ((uint32_t)b << bpos);
}



int fb_init(uint64_t mb_info_addr)
{
    sdk_target_dirty=1;
    boot_lfb_addr = boot_lfb_bytes = 0;
    /* Prefer virtio-gpu: a normal-RAM framebuffer (fast CPU writes) presented by
     * DMA (TRANSFER+FLUSH) instead of byte-copying into uncached VGA MMIO. */
    if (virtio_gpu_init() == 0) {
        fb_w = virtio_gpu_width(); fb_h = virtio_gpu_height();
        fb_pitch = fb_w * 4;
        rpos = 16; gpos = 8; bpos = 0;          /* B8G8R8X8 backing == 0x00RRGGBB */
        fb_mem = (volatile uint8_t *)virtio_gpu_fb();
        using_gpu = 1;
        fb_set_scale(pick_scale(fb_w, fb_h));
        return 1;
    }

    uint32_t total = *(volatile uint32_t *)mb_info_addr;
    uint8_t *p = (uint8_t *)(mb_info_addr + 8);
    uint8_t *end = (uint8_t *)(mb_info_addr + total);
    struct mb2_fb_tag *fb = NULL;

    while (p < end) {
        struct mb2_tag *tag = (struct mb2_tag *)p;
        if (tag->type == 0 || tag->size < 8)
            break;                       /* terminator, or a malformed zero-size tag (would loop forever) */
        if (tag->type == 8) {
            fb = (struct mb2_fb_tag *)tag;
            break;
        }
        p += (tag->size + 7) & ~7u;
    }

    if (!fb || fb->fb_type != MB2_FB_TYPE_RGB || fb->bpp != 32)
        return 0;

    fb_pitch = fb->pitch;
    fb_w     = fb->width;
    fb_h     = fb->height;
    rpos     = fb->red_pos;
    gpos     = fb->green_pos;
    bpos     = fb->blue_pos;

    uint64_t bytes = (uint64_t)fb->pitch * fb->height;
    if (!bytes || fb->addr + bytes < fb->addr) return 0;

    /* The framebuffer is high-MMIO, outside the identity map — map it. */
    vmm_map_range(fb->addr, fb->addr, bytes,
                  VMM_WRITABLE | VMM_NOCACHE);
    fb_mem = (volatile uint8_t *)fb->addr;
    boot_lfb_addr = fb->addr;
    boot_lfb_bytes = bytes;
    fb_set_scale(pick_scale(fb_w, fb_h));
    return 1;
}

/* The clip rect is carried by the target surface (struct surface), so it bounds
 * only draws into that surface and never leaks onto another app's surface or the
 * WM's screen compositing. The screen back buffer's clip is never set, so chrome
 * is always drawn in full. */
static struct ol_display sdk_display = {.allocate=kmalloc,.release=kfree};
static struct ol_display *fb_sdk(void)
{
    const struct surface *s=T?T:&screen;
    sdk_draw_calls++;
    /* The old adapter revalidated/copy-bound the same target for EVERY pixel
     * of the fallback wallpaper (1,024,000 binds at 1280x800). Target/clip
     * changes are explicit owner operations; cache their binding so a thin
     * compatibility call does not repeat frame setup inside the pixel loop. */
    if(sdk_target_dirty || sdk_display.target.px!=s->px ||
       sdk_display.target.w!=s->w || sdk_display.target.h!=s->h) {
        struct ol_pixel_target t={s->px,s->w,s->h,s->clip_on,s->clx0,s->cly0,s->clx1,s->cly1};
        sdk_display.target=t; /* failed early binds must not retain an old target */
        ol_display_bind(&sdk_display,&t,rpos,gpos,bpos);
        sdk_target_dirty=0;
    }
    return &sdk_display;
}
uint64_t fb_openlogit_batches(void) {return sdk_draw_calls;}

/* 2026-09-13: normal drawing is owned by OpenLogit. These ABI
 * adapters preserve the existing target/clip and contain no pixel loops. */
void fb_put(int x, int y, uint32_t color)
{
    ol_display_put(fb_sdk() ,x,y,color);
}

void fb_set_backbuffer(uint32_t *buf)
{
    screen.px = buf;
    screen.w  = (int)fb_w;
    screen.h  = (int)fb_h;
    T = &screen;
    sdk_target_dirty=1;
}

void fb_target(struct surface *s)
{
    T = s ? s : &screen;
    sdk_target_dirty=1;
}

/* Composite a window surface onto the current target with a direct row copy
 * (bounds-clamped once), not per-pixel fb_put. The browser's 1180x620 surface
 * is ~700K pixels; per-pixel fb_put (call + bounds check each) cost ~60-100 ms
 * under TCG and dominated every frame -- this is several times faster. */
/* HONOURS THE TARGET CLIP, which it did not before. That is what lets the
 * compositor blit only the part of an app's canvas that lies inside the damage
 * rectangle instead of copying the whole 1180x620 surface for a one-line
 * repaint -- and, more importantly, it is what guarantees a partial frame
 * cannot write a single pixel outside the region it is allowed to touch. The
 * old version was unclipped because the only caller was a full composite. */
/* 2026-09-13: normal drawing is owned by OpenLogit. These ABI
 * adapters preserve the existing target/clip and contain no pixel loops. */
void fb_blit_surface(int dx, int dy, const struct surface *src)
{
    if(!src)return;
    struct ol_pixel_target source={src->px,src->w,src->h,src->clip_on,src->clx0,src->cly0,src->clx1,src->cly1};
    ol_display_blit_surface(fb_sdk() ,dx,dy,&source);
}

void fb_blit_surface_scaled(int dx, int dy, int dw, int dh, const struct surface *src)
{
    if(!src)return;
    struct ol_pixel_target source={src->px,src->w,src->h,src->clip_on,src->clx0,src->cly0,src->clx1,src->cly1};
    ol_display_blit_surface_scaled(fb_sdk() ,dx,dy,dw,dh,&source);
}

void fb_blit_surface_scaled_bl(int dx, int dy, int dw, int dh, const struct surface *src)
{
    if(!src)return;
    struct ol_pixel_target source={src->px,src->w,src->h,src->clip_on,src->clx0,src->cly0,src->clx1,src->cly1};
    ol_display_blit_surface_scaled_bl(fb_sdk() ,dx,dy,dw,dh,&source);
}

void fb_copy_rect(int x, int y, int w, int h)
{
    if (!screen.px) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) w = (int)fb_w - x;
    if (y + h > (int)fb_h) h = (int)fb_h - y;
    if (w <= 0 || h <= 0) return;
    volatile uint8_t *mem = fb_front_acquire();
    if (!mem) return;
    for (int yy = 0; yy < h; yy++) {
        volatile uint32_t *dst = (volatile uint32_t *)(mem + (uint32_t)(y + yy) * fb_pitch);
        const uint32_t *src = screen.px + (uint32_t)(y + yy) * fb_w;
        for (int xx = 0; xx < w; xx++) dst[x + xx] = src[x + xx];
    }
    fb_front_release();
}

/* When SMP is up, smp.c registers a parallel implementation that splits a tall
 * rect's rows across all CPUs via work IPIs. */
static void (*g_par_present)(int, int, int, int);
void fb_set_present_par(void (*fn)(int, int, int, int)) { g_par_present = fn; }

void fb_present(void) { fb_present_rect(0, 0, (int)fb_w, (int)fb_h); }

/* Pixels actually pushed to the display since boot, and the number of pushes.
 * Clamped, so a caller that hands in a rect hanging off the edge is charged for
 * what was really copied and not for what it asked for. */
static uint64_t present_px, present_calls;
uint64_t fb_present_px(void)    { return present_px; }
uint64_t fb_present_calls(void) { return present_calls; }

/* Push one rectangle of the back buffer to the framebuffer. Big rects are split
 * across CPUs; small ones (cursor, clock strip) copy locally (no IPI overhead).
 *
 * The clamp moved up here from fb_copy_rect: virtio_gpu_flush clamps again for
 * itself, but the ACCOUNTING has to be done on the rect that was really pushed,
 * and a compositor that presents a rect straddling the edge would otherwise be
 * credited with pixels no one copied. */
void fb_present_rect(int x, int y, int w, int h)
{
    fb_graphics_lock();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) w = (int)fb_w - x;
    if (y + h > (int)fb_h) h = (int)fb_h - y;
    if (w <= 0 || h <= 0 || !screen.px ||
        __atomic_load_n(&fb_front_quarantined, __ATOMIC_SEQ_CST)) {
        fb_graphics_unlock();
        return;
    }
    if (!using_gpu && native_present) {
        /* AP band copies do not take the graphics mutex. Revoke admission
         * first and drain acquired leases before the GPU can touch scanout.
         * A timed-out AP drain has submitted no GPU work, so CPU fallback is
         * safe; a GPU timeout is different and must latch quarantine. */
        __atomic_store_n(&fb_front_blocked, 1, __ATOMIC_SEQ_CST);
        unsigned spins = 100000;
        while (__atomic_load_n(&fb_front_writers, __ATOMIC_SEQ_CST) && spins)
            --spins;
        int rc = -1;
        if (!__atomic_load_n(&fb_front_writers, __ATOMIC_SEQ_CST))
            rc = native_present(screen.px, fb_w, (uint32_t)x, (uint32_t)y,
                                (uint32_t)w, (uint32_t)h);
#ifdef FB_NATIVE_PRESENT_NEGCTL_CPU_FALLBACK
        if (rc == 0) rc = -1; /* Watched mutation: overwrite a completed GPU frame. */
#endif
        if (rc < -1)
            __atomic_store_n(&fb_front_quarantined, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&fb_front_blocked, 0, __ATOMIC_SEQ_CST);
        if (rc != -1) {
            if (!rc) {
                present_px += (uint64_t)w * (uint64_t)h;
                present_calls++;
            }
            fb_graphics_unlock();
            return;
        }
    }
    present_px += (uint64_t)w * (uint64_t)h;
    present_calls++;
    if (g_par_present && h >= 128) g_par_present(x, y, w, h);   /* RAM-to-RAM now (fast) */
    else fb_copy_rect(x, y, w, h);
    if (using_gpu) virtio_gpu_flush(x, y, w, h);               /* DMA the rect to the host */
    fb_graphics_unlock();
}

/* Flush a rect that was drawn straight into fb_mem (e.g. the cursor overlay):
 * for virtio-gpu it must be DMA'd to the host; for VGA MMIO it's already live. */
void fb_flush_rect(int x, int y, int w, int h)
{
    if (using_gpu) virtio_gpu_flush(x, y, w, h);
}

/* ---- the pointer ----------------------------------------------------------
 *
 * When the display has a cursor plane the pointer is not a framebuffer object
 * at all: fb_cursor_image() hands the arrow to the display once, and every
 * subsequent move is one small command with no pixel written anywhere. Callers
 * must ask fb_cursor_hw() FIRST -- on a plain multiboot LFB there is no plane,
 * fb_cursor_move() does nothing, and the compositor has to keep drawing the
 * pointer itself or the desktop loses its cursor. */
int fb_cursor_hw(void) { return using_gpu && virtio_gpu_cursor_ready(); }

int fb_cursor_image(const uint32_t *argb, int w, int h, int hot_x, int hot_y)
{
    if (!fb_cursor_hw()) return -1;
    return virtio_gpu_cursor_define(argb, w, h, hot_x, hot_y);
}

void fb_cursor_move(int x, int y)
{
    if (fb_cursor_hw()) virtio_gpu_cursor_move(x, y);
}

/* Write one pixel straight to the framebuffer (not the back buffer): for the
 * cursor overlay, which must not contaminate the cursor-free composite. */
void fb_fb_put(int x, int y, uint32_t color)
{
    if (x < 0 || y < 0 || x >= (int)fb_w || y >= (int)fb_h) return;
    fb_graphics_lock();
    volatile uint8_t *mem = fb_front_acquire();
    if (!mem) { fb_graphics_unlock(); return; }
    volatile uint32_t *dst = (volatile uint32_t *)(mem + (uint32_t)y * fb_pitch);
    dst[x] = color;
    fb_front_release();
    fb_graphics_unlock();
}

/* Text now goes through the anti-aliased Unicode engine (kernel/text.c). These
 * stay as thin wrappers so existing callers keep working; `y` is the cell top. */
/* These three take DEVICE coordinates but draw at the SCALED default UI size --
 * they are the kernel chrome's text, and the chrome converts its own point
 * geometry with fb_pt() before calling in. Routing the size through fb_ui_px()
 * here rather than at each of the ~20 call sites is what makes the menu bar and
 * window titles get re-rasterized (sharper), not stretched. */
void fb_char(int x, int y, char ch, uint32_t color)
{
    char s[2] = { ch, 0 };
    text_draw_sz(x, y, s, fb_ui_px(), color);
}

void fb_text(int x, int y, const char *s, uint32_t color)
{
    int px = fb_ui_px();
    int x0 = x, lh = text_line_height(px);
    char line[512]; int li = 0;
    for (;;) {
        if (*s == '\n' || *s == 0) {
            line[li] = 0; text_draw_sz(x0, y, line, px, color); li = 0;
            if (*s == 0) break;
            y += lh; s++;
        } else { if (li < 511) line[li++] = *s; s++; }
    }
}

int fb_text_width(const char *s) { return text_width_sz(s, fb_ui_px()); }

/* 2026-09-13: normal drawing is owned by OpenLogit. These ABI
 * adapters preserve the existing target/clip and contain no pixel loops. */
void fb_clear(uint32_t color)
{
    ol_display_clear(fb_sdk() ,color);
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    ol_display_fill_rect(fb_sdk() ,x,y,w,h,color);
}

void fb_fill_circle(int cx, int cy, int r, uint32_t color)
{
    ol_display_fill_circle(fb_sdk() ,cx,cy,r,color);
}

void fb_round_rect(int x, int y, int w, int h, int radius, uint32_t color)
{
    ol_display_round_rect(fb_sdk() ,x,y,w,h,radius,color);
}

void fb_blit_glyph(int x, int y, const uint8_t *cov, int w, int h, uint32_t color)
{
    ol_display_blit_glyph(fb_sdk() ,x,y,cov,w,h,color);
}

void fb_blend_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    ol_display_blend_rect(fb_sdk() ,x,y,w,h,r,g,b,a);
}

void fb_blend_round_rect(int x, int y, int w, int h, int radius,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    ol_display_blend_round_rect(fb_sdk() ,x,y,w,h,radius,r,g,b,a);
}

unsigned fb_shadow_clamp_count(void)
{
    return ol_display_shadow_clamp_count(fb_sdk());
}

void fb_shadow(int x, int y, int w, int h, int radius, int dy, int blur, uint8_t alpha)
{
    ol_display_shadow(fb_sdk() ,x,y,w,h,radius,dy,blur,alpha);
}

uint32_t fb_shade(uint32_t c, int delta)
{
    return ol_display_shade(fb_sdk() ,c,delta);
}

void fb_fill_vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    ol_display_fill_vgrad(fb_sdk() ,x,y,w,h,top,bottom);
}

void fb_round_rect_vgrad(int x, int y, int w, int h, int radius, uint32_t top, uint32_t bottom)
{
    ol_display_round_rect_vgrad(fb_sdk() ,x,y,w,h,radius,top,bottom);
}

void fb_blur_rect(int x, int y, int w, int h, int radius, int corner)
{
    ol_display_blur_rect(fb_sdk() ,x,y,w,h,radius,corner);
}

void fb_liquid_glass(int x, int y, int w, int h, int radius,
                     uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta)
{
    ol_display_liquid_glass(fb_sdk() ,x,y,w,h,radius,tr,tg,tb,ta);
}

void fb_liquid_glass_cut(int x, int y, int w, int h, int radius,
                         uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta,
                         unsigned cut)
{
    ol_display_liquid_glass_cut(fb_sdk() ,x,y,w,h,radius,tr,tg,tb,ta,cut);
}

void fb_blit_rgba(int dx, int dy, int dw, int dh, const uint8_t *rgba, int sw, int sh)
{
    ol_display_blit_rgba(fb_sdk() ,dx,dy,dw,dh,rgba,sw,sh);
}

#if !__STDC_HOSTED__
#include "usercopy.h"
/* Called under the WM draw domain: T and its clip/geometry cannot change
 * while usercopy pins and resolves one row at a time. No user pointer reaches
 * a pixel loop, and scale-up reuses the same row instead of recopying it. */
int fb_blit_user_rgba(int dx, int dy, int dw, int dh,
                     const uint8_t *rgba, int sw, int sh)
{
    struct surface *t = T ? T : &screen;
    if (!rgba || !t->px || dw <= 0 || dh <= 0 || sw <= 0 || sw > 4096 || sh <= 0) return -1;
    long j0 = dy < 0 ? -(long)dy : 0, j1 = dh;
    if (j1 > (long)t->h - dy) j1 = (long)t->h - dy;
    if (t->clip_on) {
        if ((long)t->cly0 - dy > j0) j0 = (long)t->cly0 - dy;
        if ((long)t->cly1 - dy < j1) j1 = (long)t->cly1 - dy;
    }
    if (j0 >= j1) return 0;
    uint8_t *row = kmalloc((unsigned)sw * 4);
    if (!row) return -1;
    int last = -1, rc = 0;
    for (long j=j0; j<j1; j++) {
        int sy = (int)(j * sh / dh);
        if (sy != last) {
            if (user_copy_from(row, rgba + (uint64_t)sy * sw * 4, (unsigned)sw * 4) < 0) { rc=-1; break; }
            last=sy;
        }
        fb_blit_rgba(dx, (int)(dy+j), dw, 1, row, sw, 1);
    }
    kfree(row);
    return rc;
}
#endif
