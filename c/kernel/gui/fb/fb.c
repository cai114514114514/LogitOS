static int fb_lfb_wc;   /* the scanout PTE read back as PAT PA1 (WC) */
#include "../../cpu/power/pat.h"
#include "mmhost.h"
#include "kprintf.h"
#include "ktime.h"
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
#include "gfx_ctx.h"
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


#define FB_CLIP_DEPTH 16

/* PER DRAWING CONTEXT (gfx_ctx.h), because the compositor renders bands of
 * one frame on several cores at once (wm.c, wm_bands.h): each core draws with
 * its own target, its own clip -- which lives ON the target surface, so each
 * context also has its own view of the screen back buffer -- and its own
 * OpenLogit display, whose blur/glass scratch must not be shared either. The
 * back buffer the views wrap is one buffer; only the bookkeeping is per core.
 * Outside a band everything runs on context 0, exactly as before. */
struct fb_ctx {
    struct surface screen;        /* wraps the back buffer (the visible screen) */
    struct surface *T;            /* current draw target (defaults to screen) */
    int dirty;                    /* the display must rebind before drawing */
    struct ol_display disp;
    /* fb_clip_push/pop: the clip each push replaced, on the TARGET surface.
     * Per context, so band workers keep independent stacks. */
    struct { int on, x0, y0, x1, y1; } clipstk[FB_CLIP_DEPTH];
    int clipdepth;
};
static struct fb_ctx g_fbctx[GFX_NCTX];
static uint64_t sdk_draw_calls;
#define FBC (&g_fbctx[gfx_ctx()])

/* ---- big corner-mask arenas ------------------------------------------
 * gfx_mask_corner serves radii past GFX_MASK_MAX (72 device px) up to
 * GFX_MASK_BIG (512) from a per-context byte arena. Each drawing context gets
 * FB_MASK_ARENA_BYTES (1 MiB, kmalloc'd once, never freed -- four contexts is
 * 4 MiB of kernel heap; 1 MiB holds four 512x512 tiles or ~200 of 72..180 px).
 * No lock: an arena is only ever touched by its own context (gfx_ctx()), and
 * gfx_mask_flush_all merely bumps a generation. If kmalloc fails the context
 * simply has no arena and big radii are refused (counted), as before. */
#define FB_MASK_ARENA_BYTES (1024UL * 1024UL)
static void fb_mask_arena_init(void)
{
    void *buf = kmalloc(FB_MASK_ARENA_BYTES);
    if (buf) gfx_mask_set_arena(buf, FB_MASK_ARENA_BYTES);
}

#if GFX_NCTX > 1
/* WHICH CONTEXT A THREAD DRAWS WITH -- by THREAD, not by core.
 *
 * Every ordinary drawer -- the compositor thread, a GUI syscall filling an app
 * canvas -- uses context 0, serialised by graphics_lock exactly as before. The
 * compositor's band workers (wm.c) each own one context from 1 up, registered
 * here once at creation. Keying on the core instead would break the moment a
 * drawer blocked or was preempted and resumed elsewhere (graphics_lock is a
 * sleeping lock and may be held across a preemption): its target and clip
 * would silently become whatever that core's context held. */
#include "sched.h"
static struct thread *volatile g_ctx_thread[GFX_NCTX];
static int fb_gfx_ctx(void)
{
    struct thread *t = sched_current_thread();
    for (int i = 1; i < GFX_NCTX; i++) if (t && g_ctx_thread[i] == t) return i;
    return 0;
}
int (*gfx_ctx_hook)(void) = fb_gfx_ctx;
int fb_ctx_register(int ctx)
{
    if (ctx < 1 || ctx >= GFX_NCTX) return -1;
    g_ctx_thread[ctx] = sched_current_thread();
    /* AFTER the line above: only now does gfx_ctx() return `ctx`, so the arena
     * lands on this context and not on context 0. Once per context. */
    static volatile unsigned char arena_done[GFX_NCTX];
    if (!arena_done[ctx]) { arena_done[ctx] = 1; fb_mask_arena_init(); }
    return 0;
}
#else
int fb_ctx_register(int ctx) { (void)ctx; return -1; }
#endif

/* The clip rectangle lives ON THE CURRENT TARGET (struct surface), not in a
 * global -- so a clip set while drawing into one app's surface can never affect
 * a draw into another app's surface (the cross-app leak that left a freshly
 * opened Terminal white). */
void fb_set_clip(int x, int y, int w, int h)
{
    struct fb_ctx *c = FBC;
    struct surface *s = c->T ? c->T : &c->screen;
    c->dirty=1;
    s->clip_on = 1; s->clx0 = x; s->cly0 = y; s->clx1 = x + w; s->cly1 = y + h;
}
void fb_clear_clip(void) { struct fb_ctx *c = FBC; struct surface *s = c->T ? c->T : &c->screen; s->clip_on = 0; c->dirty=1; }

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
    if (ui_scale != pct) gfx_mask_flush_all();   /* big tiles are keyed by device size */
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
    { static int arena0; if (!arena0) { arena0 = 1; fb_mask_arena_init(); } }   /* ctx 0 */
    for (int i = 0; i < GFX_NCTX; i++) g_fbctx[i].dirty = 1;
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

    /* The framebuffer is high-MMIO, outside the identity map — map it.
     * Write-combining (PAT PA1: PWT=1, PCD=0) when pat_init() programmed it:
     * firmware's MTRRs leave the aperture UC, and UC turns every 4-byte store
     * into its own PCIe write -- tens of milliseconds per 1080p frame on a
     * real card. MTRR UC + PAT WC is WC (Intel SDM vol. 3A table 11-7). This
     * is the first mapping of the aperture, so there is no UC alias to retire.
     * X79-VG4 bring-up; not verified on X79-VG4 hardware. */
#ifndef FB_NEGCTL_UC_MAP
    uint64_t cache = pat_wc_available() ? VMM_PAT_WC : VMM_NOCACHE;
#else
    uint64_t cache = VMM_NOCACHE;
#endif
    vmm_map_range(fb->addr, fb->addr, bytes, VMM_WRITABLE | cache);
    {
        uint64_t *pte = vmm_pte(mm_read_cr3(), fb->addr);
        uint64_t f = pte ? *pte : 0;
        fb_lfb_wc = (f & 0x18) == 0x08;
        kprintf("[fb] lfb %x+%x mapped %s (pte pwt=%d pcd=%d)\n", (unsigned)fb->addr, (unsigned)bytes,
                (f & 0x18) == 0x08 ? "write-combining" : (f & 0x18) == 0x18 ? "uncached" : "other",
                (int)((f >> 3) & 1), (int)((f >> 4) & 1));
    }
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
static struct ol_display *fb_sdk(void)
{
    struct fb_ctx *c = FBC;
    const struct surface *surface = c->T ? c->T : &c->screen;
    __atomic_fetch_add(&sdk_draw_calls, 1, __ATOMIC_RELAXED);
    if (!c->disp.allocate) { c->disp.allocate = kmalloc; c->disp.release = kfree; }
    /* The old adapter revalidated/copy-bound the same target for EVERY pixel
     * of the fallback wallpaper (1,024,000 binds at 1280x800). Target/clip
     * changes are explicit owner operations; cache their binding so a thin
     * compatibility call does not repeat frame setup inside the pixel loop. */
    if (c->dirty || c->disp.target.px != surface->px ||
        c->disp.target.w != surface->w || c->disp.target.h != surface->h) {
        struct ol_pixel_target target = {
            .px = surface->px,
            .w = surface->w,
            .h = surface->h,
            .clip_on = surface->clip_on,
            .clx0 = surface->clx0,
            .cly0 = surface->cly0,
            .clx1 = surface->clx1,
            .cly1 = surface->cly1,
        };
#ifdef FB_BIND_UNCHECKED
        c->disp.target = target;
        ol_display_bind(&c->disp, &target, rpos, gpos, bpos);
#else
        if (ol_display_bind(&c->disp, &target, rpos, gpos, bpos) != OL_OK) {
            /* The former pre-assignment tried to avoid keeping an old target,
             * but published an unvalidated one with stale channel positions.
             * On a fresh 5K bind, ff8040 became 0000ff. Disable drawing on a
             * rejected surface; a later valid bind restores the whole batch. */
            c->disp.target = (struct ol_pixel_target){0};
            c->disp.error = OL_ARGUMENT;
        }
#endif
        c->dirty = 0;
    }
    return &c->disp;
}
uint64_t fb_openlogit_batches(void) {return sdk_draw_calls;}
struct ol_display *fb_display_cur(void) { return fb_sdk(); }

/* Clip stack on the current target, per context (struct fb_ctx). The clip
 * lives on the target surface, which fb_sdk() re-reads on every rebind, so the
 * push writes the surface (not just the bound display) and survives a rebind.
 * The stack belongs to the context, not the surface: a push/pop pair must not
 * straddle fb_target(), which asserts that in FB_DEBUG builds. */
int fb_clip_push(int x0, int y0, int x1, int y1)
{
    struct fb_ctx *c = FBC;
    struct surface *s = c->T ? c->T : &c->screen;
    if (c->clipdepth >= FB_CLIP_DEPTH) return OL_LIMIT;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
#ifdef FB_CLIP_STACK_NEGCTL_DISPLAY_ONLY
    /* Control: write only the bound display's target, as a group that never
     * touches the surface would. A rebind then forgets the clip. */
    struct ol_pixel_target *dt = &fb_sdk()->target;
    s = NULL;
    c->clipstk[c->clipdepth].on = dt->clip_on;
    c->clipstk[c->clipdepth].x0 = dt->clx0; c->clipstk[c->clipdepth].y0 = dt->cly0;
    c->clipstk[c->clipdepth].x1 = dt->clx1; c->clipstk[c->clipdepth].y1 = dt->cly1;
    c->clipdepth++;
    if (dt->clip_on) {
        if (x0 < dt->clx0) x0 = dt->clx0;
        if (y0 < dt->cly0) y0 = dt->cly0;
        if (x1 > dt->clx1) x1 = dt->clx1;
        if (y1 > dt->cly1) y1 = dt->cly1;
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    dt->clip_on = 1; dt->clx0 = x0; dt->cly0 = y0; dt->clx1 = x1; dt->cly1 = y1;
    (void)s;
    return OL_OK;
#else
    c->clipstk[c->clipdepth].on = s->clip_on;
    c->clipstk[c->clipdepth].x0 = s->clx0; c->clipstk[c->clipdepth].y0 = s->cly0;
    c->clipstk[c->clipdepth].x1 = s->clx1; c->clipstk[c->clipdepth].y1 = s->cly1;
    c->clipdepth++;
    if (s->clip_on) {                       /* set_clip semantics: intersect */
        if (x0 < s->clx0) x0 = s->clx0;
        if (y0 < s->cly0) y0 = s->cly0;
        if (x1 > s->clx1) x1 = s->clx1;
        if (y1 > s->cly1) y1 = s->cly1;
    }
    if (x1 < x0) x1 = x0;                   /* empty stays empty */
    if (y1 < y0) y1 = y0;
    s->clip_on = 1; s->clx0 = x0; s->cly0 = y0; s->clx1 = x1; s->cly1 = y1;
    c->dirty = 1;
    return OL_OK;
#endif
}
void fb_clip_pop(void)
{
    struct fb_ctx *c = FBC;
    struct surface *s = c->T ? c->T : &c->screen;
    if (c->clipdepth <= 0) return;
    c->clipdepth--;
#ifdef FB_CLIP_STACK_NEGCTL_DISPLAY_ONLY
    struct ol_pixel_target *dt = &fb_sdk()->target;
    dt->clip_on = c->clipstk[c->clipdepth].on;
    dt->clx0 = c->clipstk[c->clipdepth].x0; dt->cly0 = c->clipstk[c->clipdepth].y0;
    dt->clx1 = c->clipstk[c->clipdepth].x1; dt->cly1 = c->clipstk[c->clipdepth].y1;
    (void)s;
#else
    s->clip_on = c->clipstk[c->clipdepth].on;
    s->clx0 = c->clipstk[c->clipdepth].x0; s->cly0 = c->clipstk[c->clipdepth].y0;
    s->clx1 = c->clipstk[c->clipdepth].x1; s->cly1 = c->clipstk[c->clipdepth].y1;
    c->dirty = 1;
#endif
}
int fb_clip_depth(void) { return FBC->clipdepth; }

/* 2026-09-13: normal drawing is owned by OpenLogit. These ABI
 * adapters preserve the existing target/clip and contain no pixel loops. */
void fb_put(int x, int y, uint32_t color)
{
    ol_display_put(fb_sdk() ,x,y,color);
}

void fb_set_backbuffer(uint32_t *buf)
{
    for (int i = 0; i < GFX_NCTX; i++) {      /* every context's view of it */
        struct fb_ctx *c = &g_fbctx[i];
        c->screen.px = buf;
        c->screen.w  = (int)fb_w;
        c->screen.h  = (int)fb_h;
        c->screen.clip_on = 0;
        c->T = &c->screen;
        c->dirty = 1;
        c->clipdepth = 0;
    }
}

void fb_target(struct surface *s)
{
    struct fb_ctx *c = FBC;
    struct surface *n = s ? s : &c->screen;
#ifdef FB_DEBUG
    /* An open clip push belongs to the old target: popping it later would
     * restore a clip onto the wrong surface. */
    if (n != (c->T ? c->T : &c->screen) && c->clipdepth) __builtin_trap();
#endif
    c->T = n;
    c->dirty=1;
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
    if (!g_fbctx[0].screen.px) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) w = (int)fb_w - x;
    if (y + h > (int)fb_h) h = (int)fb_h - y;
    if (w <= 0 || h <= 0) return;
    volatile uint8_t *mem = fb_front_acquire();
    if (!mem) return;
    for (int yy = 0; yy < h; yy++) {
        volatile uint32_t *dst = (volatile uint32_t *)(mem + (uint32_t)(y + yy) * fb_pitch);
        const uint32_t *src = g_fbctx[0].screen.px + (uint32_t)(y + yy) * fb_w;
#ifndef FB_NEGCTL_PIXEL_COPY
        /* One string move per row: with the aperture write-combining, the CPU
         * merges it into full 64-byte bursts; ERMSB (Ivy Bridge and later)
         * makes rep movsb the fast form. The old per-pixel volatile loop
         * forced one bus write per 4 bytes. */
        void *d = (void *)(uintptr_t)(dst + x);
        const void *s = src + x;
        size_t n = (size_t)w * 4;
        __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
#else
        for (int xx = 0; xx < w; xx++) dst[x + xx] = src[x + xx];
#endif
    }
    fb_front_release();
}

/* When SMP is up, smp.c registers a parallel implementation that splits a tall
 * rect's rows across all CPUs via work IPIs. */
static void (*g_par_present)(int, int, int, int);
void fb_set_present_par(void (*fn)(int, int, int, int)) { g_par_present = fn; }

void fb_present(void) { fb_present_rect(0, 0, (int)fb_w, (int)fb_h); }

/* One full-screen copy, timed, before anything else draws: the number that
 * says whether the scanout path is fast on this machine. */
void fb_present_bench(void)
{
    /* the label is the mapping read back at fb_init(), not PAT availability */
    if (!g_fbctx[0].screen.px || using_gpu || native_present) return;
    uint64_t t0 = time_mono_ns();
    fb_copy_rect(0, 0, (int)fb_w, (int)fb_h);
    uint64_t ns = time_mono_ns() - t0;
    uint64_t bytes = (uint64_t)fb_w * fb_h * 4;
    unsigned us = (unsigned)(ns / 1000), mbs = ns ? (unsigned)(bytes * 1000 / ns) : 0;
    kprintf("[fb] present bench %ux%u: %u us per frame, %u MB/s (%s)\n", fb_w, fb_h, us, mbs,
            fb_lfb_wc ? "WC" : "not WC");
}

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
    if (w <= 0 || h <= 0 || !g_fbctx[0].screen.px ||
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
            rc = native_present(g_fbctx[0].screen.px, fb_w, (uint32_t)x, (uint32_t)y,
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

void fb_set_reduce_transparency(int enabled)
{
    for (int i = 0; i < GFX_NCTX; i++) g_fbctx[i].disp.reduce_transparency = enabled != 0;
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
    struct fb_ctx *c = FBC;
    struct surface *t = c->T ? c->T : &c->screen;
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
