/* js_canvas.c -- CanvasRenderingContext2D, on Open Logit.
 *
 * WHY THIS FILE EXISTS, and the number is the whole argument. The instrument
 * that names a failed callee (third_party/quickjs/quickjs.c, marker
 * LOGIT-NAME-CALLEE) was pointed at the site scoreboard the day it landed, and
 * one name came back ahead of everything else by an order of magnitude:
 *
 *     33  getContext        qq 25, stripe 8, anthropic 2
 *      1  write
 *      1  appendChild
 *
 * Before that instrument those 33 lines all read `TypeError: not a function`
 * and named nothing, which is why this had never been ranked.
 *
 * ---------------------------------------------------------------------------
 * WHY NOT `getContext() { return null; }`, WHICH IS ONE LINE AND LOOKS SAFE
 * ---------------------------------------------------------------------------
 * Returning null IS how the spec refuses a context type, so the return value
 * would be honest. The corpus says the CALLERS are not written for it --
 * tests/fixtures/jsperf/baidu-async-search.js:
 *
 *     var o = a.getContext === i ? !1 : a.getContext("2d");
 *     if (o === !1) return !1;
 *     a.width = a.height = 10; ...
 *
 * `i` is undefined and the guard compares STRICTLY against false. With
 * getContext absent the probe returns false and the page takes its fallback
 * cleanly; with a getContext that returns null the guard does not fire and the
 * page walks on holding null. The one-line fix converts a clean fallback into
 * a crash further from its cause. So the exit is a real context, and this is
 * it.
 *
 * ---------------------------------------------------------------------------
 * IT IS THE CONSUMER c/lib/gfx DID NOT HAVE
 * ---------------------------------------------------------------------------
 * The engine's own reconnaissance recorded that `gfx_fill`, `gfx_paint_linear`,
 * `gfx_paint_radial`, `gfx_surface_init` and the whole `gfx_m_*` affine layer
 * had NO PRODUCTION CALLER anywhere in the tree -- two unit tests and a bench.
 * Canvas 2D is exactly their shape: paths in user coordinates, an affine CTM,
 * nonzero and evenodd fills, stroking, gradient paints, straight-alpha RGBA8
 * surfaces. Nothing here is a new rasterizer and the one-rasterizer invariant
 * survives: this file makes gfx calls and owns no scanline loop.
 *
 * Two engine properties decided the shape of the code rather than the reverse:
 *
 *   - A gfx_surface is STRAIGHT (non-premultiplied) RGBA8, which is byte for
 *     byte what ImageData is. getImageData is a copy, not a conversion -- and
 *     that is why it can be the primary gate: what a test reads back is
 *     literally what the engine composited, with nothing in between to be
 *     wrong in a compensating direction.
 *
 *   - gfx_path_matrix() REFUSES to be called once the path holds a point (it
 *     latches `overflow`), because points already recorded were flattened
 *     under the old matrix. Canvas requires the opposite: the CTM in force
 *     when a point is added transforms it, and a later translate() must not
 *     move points already in the path. So the path's own matrix stays IDENTITY
 *     here and this file transforms every point itself with gfx_m_apply. That
 *     is not a workaround, it is the only reading of the two contracts that is
 *     true to both.
 *
 * ---------------------------------------------------------------------------
 * THE SEAM
 * ---------------------------------------------------------------------------
 * Nothing here edits js_dom.c. It installs onto `HTMLCanvasElement.prototype`
 * BY NAME -- the seam js_semantics.c, js_media.c, js_forms.c and js_select.c
 * all use, and by name rather than by walking up from a created element, which
 * is the trap js_select.c's header documents. That prototype is a real link in
 * a <canvas>'s chain (js_dom_iface.inc:214), which settles a question the old
 * absence note worried about at length: `getContext` lands on canvases and on
 * NOTHING else, so `div.getContext` stays undefined and a probe that tests the
 * wrong element still gets the right answer.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS REFUSED, AND WHY BY NAME RATHER THAN STUBBED
 * ---------------------------------------------------------------------------
 * `toDataURL` and `toBlob` THROW. This tree decodes PNG and does not encode
 * it, and a fabricated data URL is the single most load-bearing lie a canvas
 * can tell: it is what every fingerprint and every "does this browser support
 * webp" probe reads, and a wrong one is believed rather than detected.
 *
 * `getContext` of anything but "2d" returns null. For webgl that is not a
 * refusal, it is the truth.
 *
 * NOT HERE YET, and deliberately left to throw so that the same instrument
 * which chose this file chooses what comes next: drawImage, fillText /
 * strokeText / measureText, clip(), and the composite operations beyond
 * source-over.
 */
#include <string.h>
#include <stdlib.h>
#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "gfx.h"
#include "img.h"

int printf(const char *, ...);

/* Point budget for one path. gfx latches overflow rather than truncating, so
 * this is a number that gets REPORTED when a page exceeds it, not one that
 * quietly changes the picture. 4096 points is a very long path for the charts
 * and sparklines canvas is mostly used for on a page. */
#define CV_PTS   4096
#define CV_SUBS   256
#define CV_STACK   32
#define CV_GRADS   32

/* Largest backing store, in pixels: 4 MPx x 4 B = 16 MiB. A bigger canvas is
 * refused by returning null from getContext, which the spec permits and which
 * is what a browser out of video memory does. */
#define CV_MAXPX (4 * 1024 * 1024)

struct cv_state {
    struct gfx_matrix m;
    unsigned char fill[4], stroke[4];
    int galpha;                    /* 0..255 */
    int lw;                        /* user units, 24.8 */
    int cap, join;
    int miter;                     /* 16.16 */
    int fill_grad, stroke_grad;    /* index into grads[], -1 = solid colour */
};

struct cv_grad {
    int kind;                      /* GFX_LINEAR / GFX_RADIAL */
    int x0, y0, x1, y1, r;         /* USER units 24.8; transformed at paint
                                    * time, because a canvas gradient is
                                    * placed by the CTM in force when the
                                    * drawing operation runs, not when the
                                    * gradient was created */
    struct gfx_stop stop[GFX_MAX_STOPS];
    int nstop;
    int used;
};

struct canvas2d {
    struct node *el;
    unsigned char *px;
    int w, h;
    struct cv_state st;
    struct cv_state stack[CV_STACK];
    int nstack;
    int pts[CV_PTS * 2];
    int subs[CV_SUBS];
    struct gfx_path path;
    int has_pt;                    /* the current subpath has a start point */
    int startx, starty;            /* USER 24.8, for closePath */
    int curx, cury;                /* USER 24.8 */
    struct cv_grad grads[CV_GRADS];
    JSValue elval;                 /* keeps the element wrapper alive */
    struct canvas2d *next;         /* g_all, so the painter can find one by node */
};

static JSClassID cv_class_id;
static JSClassID cvgrad_class_id;

/* ------------------------------------------------------------------ misc -- */

static int fx(double v)                       /* double -> 24.8, saturating */
{
    if (!(v > -4000000.0)) return -1024000000;
    if (!(v <  4000000.0)) return  1024000000;
    return (int)(v * 256.0);
}
static double unfx(int v) { return (double)v / 256.0; }

/* Read one number argument as 24.8. Returns -1 when the value is NaN or not
 * convertible, which the spec treats as "ignore this call" for the geometry
 * entry points -- not as an error to throw. */
static int arg_fx(JSContext *ctx, JSValueConst v, int *out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v)) return -1;
    if (d != d) return -1;
    *out = fx(d);
    return 0;
}

static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* ------------------------------------------------------------------ state -- */

static void st_reset(struct cv_state *s)
{
    gfx_m_identity(&s->m);
    s->fill[0] = s->fill[1] = s->fill[2] = 0; s->fill[3] = 255;
    s->stroke[0] = s->stroke[1] = s->stroke[2] = 0; s->stroke[3] = 255;
    s->galpha = 255;
    s->lw = 256;
    s->cap = GFX_CAP_BUTT;
    s->join = GFX_JOIN_MITER;
    s->miter = 10 * 65536;
    s->fill_grad = s->stroke_grad = -1;
}

static struct canvas2d *cv_of(JSValueConst v)
{ return (struct canvas2d *)JS_GetOpaque(v, cv_class_id); }

/* A LIST, not a fixed table. It was eight slots for one run, and qq.com opened
 * more than eight canvases on its front page -- so the ninth printed "will not
 * reach the screen" and did not, honestly and uselessly. There is no natural
 * number here: a page decides how many canvases it has. The link costs one
 * pointer inside a struct that is malloc'd anyway, and removes the cap rather
 * than raising it to the next number that a page will exceed. */
static struct canvas2d *g_all;

static void cv_finalizer(JSRuntime *rt, JSValue val)
{
    struct canvas2d *c = (struct canvas2d *)JS_GetOpaque(val, cv_class_id);
    if (!c) return;
    /* Off the list FIRST: browser_paint.c walks it every frame, and a link to
     * freed memory would be read before anything noticed the context was
     * gone. */
    for (struct canvas2d **pp = &g_all; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    if (c->px) free(c->px);
    JS_FreeValueRT(rt, c->elval);
    free(c);
}

/* element --__ctx2d--> context --elval--> element is a CYCLE, and QuickJS can
 * only collect one it can WALK. Without this mark the pair is unreachable and
 * uncollectable at the same time, and JS_FreeRuntime asserts on a non-empty
 * gc_obj_list -- which is how it was found: the suite passed and then aborted
 * on the way out. A leak that only shows up at teardown is exactly the kind a
 * browser never notices, because a browser does not tear the runtime down. */
static void cv_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    struct canvas2d *c = (struct canvas2d *)JS_GetOpaque(val, cv_class_id);
    if (c) JS_MarkValue(rt, c->elval, mark_func);
}

static JSClassDef cv_class     = { "CanvasRenderingContext2D", cv_finalizer, cv_gc_mark };
static JSClassDef cvgrad_class = { "CanvasGradient", NULL };

/* ------------------------------------------------------------------- path -- */

/* The path holds DEVICE coordinates and its own matrix stays identity -- see
 * the header. Every entry point below transforms through the CTM here. */
static void dev(struct canvas2d *c, int ux, int uy, int *dx, int *dy)
{
#ifdef CANVAS_IGNORE_CTM
    /* THE NEGATIVE CONTROL (tests/canvas.mk). Every point goes to the device
     * unchanged, which is the natural wrong implementation: gfx_path carries a
     * matrix of its own, so "the path will handle it" is what a reader assumes
     * until gfx_path_matrix refuses the mid-build call. It still draws a
     * perfectly good picture -- in the wrong place -- so every colour, every
     * edge and every ImageData check passes and only the transform checks
     * redden. That is what makes those the ones measuring the transform. */
    (void)c; *dx = ux; *dy = uy;
#else
    gfx_m_apply(&c->st.m, ux, uy, dx, dy);
#endif
}

static void path_reset(struct canvas2d *c)
{
    gfx_path_init(&c->path, c->pts, CV_PTS, c->subs, CV_SUBS);
    c->has_pt = 0;
}

/* The rect entry points need a path of their OWN, not c->path with its counts
 * saved and restored: gfx_path_init points a path at the CALLER'S buffers, so
 * re-initialising c->path over c->pts[] and drawing into it overwrites the
 * page's points while leaving the struct restorable -- the counts come back and
 * the geometry does not. That is exactly what "fillRect must not disturb the
 * current path" turned out to mean here, and the test found it. */
static void rect_into(struct canvas2d *c, struct gfx_path *p, int x, int y, int w, int h)
{
    int a, b;
    dev(c, x, y, &a, &b);         gfx_move_to(p, a, b);
    dev(c, x + w, y, &a, &b);     gfx_line_to(p, a, b);
    dev(c, x + w, y + h, &a, &b); gfx_line_to(p, a, b);
    dev(c, x, y + h, &a, &b);     gfx_line_to(p, a, b);
    gfx_close(p);
}

static void emit_move(struct canvas2d *c, int ux, int uy)
{
    int dx, dy; dev(c, ux, uy, &dx, &dy);
    gfx_move_to(&c->path, dx, dy);
    c->startx = ux; c->starty = uy; c->curx = ux; c->cury = uy;
    c->has_pt = 1;
}
static void emit_line(struct canvas2d *c, int ux, int uy)
{
    if (!c->has_pt) { emit_move(c, ux, uy); return; }
    int dx, dy; dev(c, ux, uy, &dx, &dy);
    gfx_line_to(&c->path, dx, dy);
    c->curx = ux; c->cury = uy;
}

/* gfx has a whole ellipse but no SWEEP, which is what canvas needs. Flattened
 * here in the engine's own integer trigonometry, with the step count taken
 * from the DEVICE radius so a magnified arc does not become polygonal -- the
 * same criterion gfx_path's curve flattening uses, and the reason this is not
 * a fixed segment count. */
static void arc_emit(struct canvas2d *c, int cx, int cy, int rx, int ry,
                     int a0, int a1, int ccw)
{
    int rmax = rx > ry ? rx : ry;
    int scale = gfx_m_scale_of(&c->st.m);
    int rdev = (int)(((long long)rmax * scale) >> 16);
    if (rdev < 256) rdev = 256;
    int steps = (int)gfx_isqrt((unsigned long long)rdev * 8);
    if (steps < 8) steps = 8;
    if (steps > 512) steps = 512;

    int full = 360 * 256;
    int span = a1 - a0;
    if (ccw) { while (span > 0) span -= full; if (span < -full) span = -full; }
    else     { while (span < 0) span += full; if (span >  full) span =  full; }

    for (int i = 0; i <= steps; i++) {
        int a = a0 + (int)(((long long)span * i) / steps);
        int x = cx + (int)(((long long)rx * gfx_cos(a)) >> 16);
        int y = cy + (int)(((long long)ry * gfx_sin(a)) >> 16);
        if (i == 0 && !c->has_pt) emit_move(c, x, y);
        else emit_line(c, x, y);
    }
}

/* ------------------------------------------------------------------ paint -- */

/* Build the gfx paint for a fill or a stroke. Gradient stops are already
 * colour+alpha; the state's globalAlpha multiplies the lot, which is what
 * gfx_paint's own global_alpha field is for. */
static void make_paint(struct canvas2d *c, int is_stroke, struct gfx_paint *p)
{
    const unsigned char *col = is_stroke ? c->st.stroke : c->st.fill;
    int gi = is_stroke ? c->st.stroke_grad : c->st.fill_grad;
    if (gi >= 0 && gi < CV_GRADS && c->grads[gi].used) {
        struct cv_grad *g = &c->grads[gi];
        int x0, y0, x1, y1;
        dev(c, g->x0, g->y0, &x0, &y0);
        dev(c, g->x1, g->y1, &x1, &y1);
        if (g->kind == GFX_RADIAL) {
            int scale = gfx_m_scale_of(&c->st.m);
            int rd = (int)(((long long)g->r * scale) >> 16);
            gfx_paint_radial(p, x1, y1, rd);
        } else {
            gfx_paint_linear(p, x0, y0, x1, y1);
        }
        for (int i = 0; i < g->nstop; i++)
            gfx_paint_stop(p, g->stop[i].t, g->stop[i].color, g->stop[i].alpha);
    } else {
        gfx_paint_solid(p, GFX_RGB(col[0], col[1], col[2]), col[3]);
    }
    p->global_alpha = c->st.galpha;
}

static void surf_of(struct canvas2d *c, struct gfx_surface *s)
{ gfx_surface_init(s, c->px, c->w, c->h, c->w * 4); }

/* One place reports a refused path, so a page that draws something too long
 * finds out which canvas and how long rather than seeing a missing shape. */
static int path_ok(struct canvas2d *c, const struct gfx_path *p, const char *what)
{
    if (!p->overflow) return 1;
    printf("[canvas] %s refused: path exceeded %d points or %d subpaths "
           "(this canvas is %dx%d)\n", what, CV_PTS, CV_SUBS, c->w, c->h);
    return 0;
}

static void do_fill(struct canvas2d *c, struct gfx_path *path, int rule)
{
    if (!c->px || !path_ok(c, path, "fill")) return;
    struct gfx_surface s; surf_of(c, &s);
    struct gfx_paint p;   make_paint(c, 0, &p);
    struct gfx_rect clip = { 0, 0, c->w, c->h };
    gfx_fill(&s, path, rule, &p, &clip);
}

static void do_stroke(struct canvas2d *c, struct gfx_path *path)
{
    if (!c->px || !path_ok(c, path, "stroke")) return;
    /* lineWidth is in user units; the stroker wants device. A non-uniform CTM
     * cannot be expressed as one width -- a real implementation strokes in
     * user space and transforms the outline -- so this uses gfx's approximate
     * uniform scale and is exact for the uniform case, which is every canvas
     * that has not called scale() with two different factors. */
    int scale = gfx_m_scale_of(&c->st.m);
    struct gfx_stroke sk;
    memset(&sk, 0, sizeof sk);
    sk.width = (int)(((long long)c->st.lw * scale) >> 16);
    if (sk.width <= 0) return;
    sk.cap = c->st.cap;
    sk.join = c->st.join;
    sk.miter_limit = c->st.miter;

    static int opts[CV_PTS * 6];
    static int osub[CV_SUBS * 4];
    struct gfx_path out;
    gfx_path_init(&out, opts, CV_PTS * 3, osub, CV_SUBS * 4);
    if (!gfx_stroke_path(&out, &c->path, &sk)) {
        printf("[canvas] stroke refused: the outline did not fit %d points "
               "(source path has %d)\n", CV_PTS * 3, c->path.npt);
        return;
    }
    struct gfx_surface s; surf_of(c, &s);
    struct gfx_paint p;   make_paint(c, 1, &p);
    struct gfx_rect clip = { 0, 0, c->w, c->h };
    gfx_fill(&s, &out, GFX_NONZERO, &p, &clip);
}

/* ------------------------------------------------------- context methods -- */

#define CV_THIS struct canvas2d *c = cv_of(t); if (!c) return JS_UNDEFINED

static JSValue cv_save(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv; CV_THIS;
    if (c->nstack >= CV_STACK) {
        printf("[canvas] save() ignored: state stack is full (%d deep)\n", CV_STACK);
        return JS_UNDEFINED;
    }
    c->stack[c->nstack++] = c->st;
    return JS_UNDEFINED;
}
static JSValue cv_restore(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv; CV_THIS;
    if (c->nstack > 0) c->st = c->stack[--c->nstack];
    return JS_UNDEFINED;
}

static JSValue cv_translate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS; int x, y;
    if (argc < 2 || arg_fx(ctx, argv[0], &x) || arg_fx(ctx, argv[1], &y)) return JS_UNDEFINED;
    gfx_m_translate(&c->st.m, x, y);
    return JS_UNDEFINED;
}
static JSValue cv_scale(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS;
    double sx, sy;
    if (argc < 2 || JS_ToFloat64(ctx, &sx, argv[0]) || JS_ToFloat64(ctx, &sy, argv[1]))
        return JS_UNDEFINED;
    if (sx != sx || sy != sy) return JS_UNDEFINED;
    gfx_m_scale(&c->st.m, (int)(sx * 65536.0), (int)(sy * 65536.0));
    return JS_UNDEFINED;
}
static JSValue cv_rotate(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS;
    double r;
    if (argc < 1 || JS_ToFloat64(ctx, &r, argv[0]) || r != r) return JS_UNDEFINED;
    /* gfx takes 24.8 DEGREES; canvas gives radians. 180/pi to 24.8 is
     * 14667.7..., and the constant is rounded once here rather than at each
     * call site. */
    gfx_m_rotate(&c->st.m, (int)(r * 14667.719));
    return JS_UNDEFINED;
}
static JSValue cv_transform(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS;
    double v[6];
    if (argc < 6) return JS_UNDEFINED;
    for (int i = 0; i < 6; i++)
        if (JS_ToFloat64(ctx, &v[i], argv[i]) || v[i] != v[i]) return JS_UNDEFINED;
    struct gfx_matrix n, o;
    gfx_m_set(&n, (int)(v[0] * 65536.0), (int)(v[1] * 65536.0),
                  (int)(v[2] * 65536.0), (int)(v[3] * 65536.0),
                  fx(v[4]), fx(v[5]));
    gfx_m_mul(&o, &c->st.m, &n);
    c->st.m = o;
    return JS_UNDEFINED;
}
static JSValue cv_setTransform(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS;
    if (argc < 6) { gfx_m_identity(&c->st.m); return JS_UNDEFINED; }
    double v[6];
    for (int i = 0; i < 6; i++)
        if (JS_ToFloat64(ctx, &v[i], argv[i]) || v[i] != v[i]) return JS_UNDEFINED;
    gfx_m_set(&c->st.m, (int)(v[0] * 65536.0), (int)(v[1] * 65536.0),
                        (int)(v[2] * 65536.0), (int)(v[3] * 65536.0),
                        fx(v[4]), fx(v[5]));
    return JS_UNDEFINED;
}
static JSValue cv_resetTransform(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv; CV_THIS;
    gfx_m_identity(&c->st.m);
    return JS_UNDEFINED;
}

static JSValue cv_beginPath(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv; CV_THIS;
    path_reset(c);
    return JS_UNDEFINED;
}
static JSValue cv_closePath(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv; CV_THIS;
    if (c->has_pt) {
        gfx_close(&c->path);
        c->curx = c->startx; c->cury = c->starty;
        /* The spec: after closePath the current point is the start point and a
         * NEW subpath begins there. gfx_close ends the subpath, so the next
         * lineTo must start one -- has_pt going false is exactly that. */
        c->has_pt = 0;
    }
    return JS_UNDEFINED;
}
static JSValue cv_moveTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS; int x, y;
    if (argc < 2 || arg_fx(ctx, argv[0], &x) || arg_fx(ctx, argv[1], &y)) return JS_UNDEFINED;
    emit_move(c, x, y);
    return JS_UNDEFINED;
}
static JSValue cv_lineTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS; int x, y;
    if (argc < 2 || arg_fx(ctx, argv[0], &x) || arg_fx(ctx, argv[1], &y)) return JS_UNDEFINED;
    emit_line(c, x, y);
    return JS_UNDEFINED;
}
static JSValue cv_quadTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS; int cx, cy, x, y;
    if (argc < 4 || arg_fx(ctx, argv[0], &cx) || arg_fx(ctx, argv[1], &cy) ||
        arg_fx(ctx, argv[2], &x) || arg_fx(ctx, argv[3], &y)) return JS_UNDEFINED;
    if (!c->has_pt) emit_move(c, cx, cy);
    int dcx, dcy, dx, dy;
    dev(c, cx, cy, &dcx, &dcy); dev(c, x, y, &dx, &dy);
    gfx_quad_to(&c->path, dcx, dcy, dx, dy);
    c->curx = x; c->cury = y;
    return JS_UNDEFINED;
}
static JSValue cv_bezierTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS; int a[6];
    if (argc < 6) return JS_UNDEFINED;
    for (int i = 0; i < 6; i++) if (arg_fx(ctx, argv[i], &a[i])) return JS_UNDEFINED;
    if (!c->has_pt) emit_move(c, a[0], a[1]);
    int d[6];
    dev(c, a[0], a[1], &d[0], &d[1]);
    dev(c, a[2], a[3], &d[2], &d[3]);
    dev(c, a[4], a[5], &d[4], &d[5]);
    gfx_cubic_to(&c->path, d[0], d[1], d[2], d[3], d[4], d[5]);
    c->curx = a[4]; c->cury = a[5];
    return JS_UNDEFINED;
}
static JSValue cv_rectpath(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS; int x, y, w, h;
    if (argc < 4 || arg_fx(ctx, argv[0], &x) || arg_fx(ctx, argv[1], &y) ||
        arg_fx(ctx, argv[2], &w) || arg_fx(ctx, argv[3], &h)) return JS_UNDEFINED;
    emit_move(c, x, y);
    emit_line(c, x + w, y);
    emit_line(c, x + w, y + h);
    emit_line(c, x, y + h);
    gfx_close(&c->path);
    c->has_pt = 0;
    c->curx = x; c->cury = y;
    return JS_UNDEFINED;
}
static JSValue cv_arc(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS;
    int cx, cy, r;
    double a0, a1;
    if (argc < 5 || arg_fx(ctx, argv[0], &cx) || arg_fx(ctx, argv[1], &cy) ||
        arg_fx(ctx, argv[2], &r)) return JS_UNDEFINED;
    if (JS_ToFloat64(ctx, &a0, argv[3]) || JS_ToFloat64(ctx, &a1, argv[4])) return JS_UNDEFINED;
    if (a0 != a0 || a1 != a1) return JS_UNDEFINED;
    int ccw = argc > 5 && JS_ToBool(ctx, argv[5]);
    arc_emit(c, cx, cy, r, r, (int)(a0 * 14667.719), (int)(a1 * 14667.719), ccw);
    return JS_UNDEFINED;
}
static JSValue cv_ellipse(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS;
    int cx, cy, rx, ry;
    double rot, a0, a1;
    if (argc < 7 || arg_fx(ctx, argv[0], &cx) || arg_fx(ctx, argv[1], &cy) ||
        arg_fx(ctx, argv[2], &rx) || arg_fx(ctx, argv[3], &ry)) return JS_UNDEFINED;
    if (JS_ToFloat64(ctx, &rot, argv[4]) || JS_ToFloat64(ctx, &a0, argv[5]) ||
        JS_ToFloat64(ctx, &a1, argv[6])) return JS_UNDEFINED;
    if (rot != rot || a0 != a0 || a1 != a1) return JS_UNDEFINED;
    int ccw = argc > 7 && JS_ToBool(ctx, argv[7]);
    /* An ellipse rotation is a CTM change around the centre, so it composes
     * rather than needing its own code path -- and it is restored exactly,
     * not approximately, because the saved matrix is the same struct. */
    struct gfx_matrix saved = c->st.m;
    if (rot != 0.0) {
        gfx_m_translate(&c->st.m, cx, cy);
        gfx_m_rotate(&c->st.m, (int)(rot * 14667.719));
        gfx_m_translate(&c->st.m, -cx, -cy);
    }
    arc_emit(c, cx, cy, rx, ry, (int)(a0 * 14667.719), (int)(a1 * 14667.719), ccw);
    c->st.m = saved;
    return JS_UNDEFINED;
}

static JSValue cv_fill(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS;
    int rule = GFX_NONZERO;
    if (argc > 0 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s && !strcmp(s, "evenodd")) rule = GFX_EVENODD;
        if (s) JS_FreeCString(ctx, s);
    }
    do_fill(c, &c->path, rule);
    return JS_UNDEFINED;
}
static JSValue cv_stroke(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv; CV_THIS;
    do_stroke(c, &c->path);
    return JS_UNDEFINED;
}

/* fillRect/strokeRect build a throwaway path so they go through exactly the
 * same fill as fill() does -- one code path, so a rect and a rect-shaped path
 * cannot come out different. The page's own path is saved and restored, which
 * the spec requires: these do not disturb the current path. */
static JSValue cv_fillRect(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS; int x, y, w, h;
    if (argc < 4 || arg_fx(ctx, argv[0], &x) || arg_fx(ctx, argv[1], &y) ||
        arg_fx(ctx, argv[2], &w) || arg_fx(ctx, argv[3], &h)) return JS_UNDEFINED;
    if (w == 0 || h == 0) return JS_UNDEFINED;
    int tp[16], ts[4];
    struct gfx_path r;
    gfx_path_init(&r, tp, 8, ts, 4);
    rect_into(c, &r, x, y, w, h);
    do_fill(c, &r, GFX_NONZERO);
    return JS_UNDEFINED;
}
static JSValue cv_strokeRect(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS; int x, y, w, h;
    if (argc < 4 || arg_fx(ctx, argv[0], &x) || arg_fx(ctx, argv[1], &y) ||
        arg_fx(ctx, argv[2], &w) || arg_fx(ctx, argv[3], &h)) return JS_UNDEFINED;
    int tp[16], ts[4];
    struct gfx_path r;
    gfx_path_init(&r, tp, 8, ts, 4);
    rect_into(c, &r, x, y, w, h);
    do_stroke(c, &r);
    return JS_UNDEFINED;
}

/* clearRect is destination-out, which gfx has no operator for, so it is done
 * here over a coverage mask -- the mask IS the shape, so a rotated CTM clears
 * a parallelogram and not its bounding box. The axis-aligned case (b and c
 * zero, which is every canvas that never called rotate) skips the mask
 * entirely because there the shape and its bounds are the same rectangle. */
static JSValue cv_clearRect(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS; int x, y, w, h;
    if (!c->px) return JS_UNDEFINED;
    if (argc < 4 || arg_fx(ctx, argv[0], &x) || arg_fx(ctx, argv[1], &y) ||
        arg_fx(ctx, argv[2], &w) || arg_fx(ctx, argv[3], &h)) return JS_UNDEFINED;
    if (w == 0 || h == 0) return JS_UNDEFINED;

    if (c->st.m.b == 0 && c->st.m.c == 0) {
        int x0, y0, x1, y1;
        dev(c, x, y, &x0, &y0);
        dev(c, x + w, y + h, &x1, &y1);
        if (x1 < x0) { int s = x0; x0 = x1; x1 = s; }
        if (y1 < y0) { int s = y0; y0 = y1; y1 = s; }
        int px0 = (x0 + 128) >> 8, py0 = (y0 + 128) >> 8;
        int px1 = (x1 + 128) >> 8, py1 = (y1 + 128) >> 8;
        if (px0 < 0) px0 = 0; if (py0 < 0) py0 = 0;
        if (px1 > c->w) px1 = c->w; if (py1 > c->h) py1 = c->h;
        for (int yy = py0; yy < py1; yy++)
            memset(c->px + ((size_t)yy * c->w + px0) * 4, 0, (size_t)(px1 - px0) * 4);
        return JS_UNDEFINED;
    }

    int tp[16], ts[4];
    struct gfx_path r;
    gfx_path_init(&r, tp, 8, ts, 4);
    rect_into(c, &r, x, y, w, h);
    int bx0, by0, bx1, by1;
    if (gfx_path_bounds(&r, &bx0, &by0, &bx1, &by1) && !r.overflow) {
        if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0;
        if (bx1 > c->w) bx1 = c->w; if (by1 > c->h) by1 = c->h;
        int mw = bx1 - bx0, mh = by1 - by0;
        if (mw > 0 && mh > 0 && mw <= GFX_MAX_W) {
            unsigned char *cov = (unsigned char *)malloc((size_t)mw * mh);
            if (cov) {
                memset(cov, 0, (size_t)mw * mh);
                gfx_fill_mask(&r, GFX_NONZERO, cov, mw, mh, bx0, by0);
                for (int yy = 0; yy < mh; yy++)
                    for (int xx = 0; xx < mw; xx++) {
                        int a = cov[(size_t)yy * mw + xx];
                        if (!a) continue;
                        unsigned char *d = c->px + (((size_t)(by0 + yy) * c->w) + bx0 + xx) * 4;
                        d[3] = (unsigned char)((d[3] * (255 - a) + 127) / 255);
                        if (d[3] == 0) { d[0] = d[1] = d[2] = 0; }
                    }
                free(cov);
            }
        }
    }
    return JS_UNDEFINED;
}

/* ---------------------------------------------------------- image data -- */

static JSValue make_imagedata(JSContext *ctx, int w, int h, const unsigned char *src,
                              int src_stride)
{
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return obj;
    size_t n = (size_t)w * h * 4;
    JSValue buf = JS_NewArrayBuffer(ctx, NULL, 0, NULL, NULL, 0);
    JS_FreeValue(ctx, buf);
    /* Uint8ClampedArray is built through the JS side because QuickJS's C API
     * has no constructor for it; going through the global keeps one
     * implementation of "what an ImageData's data is". */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "Uint8ClampedArray");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, ctor)) { JS_FreeValue(ctx, ctor); JS_FreeValue(ctx, obj);
                                     return JS_ThrowTypeError(ctx, "Uint8ClampedArray is missing"); }
    JSValue len = JS_NewInt64(ctx, (int64_t)n);
    JSValue arr = JS_CallConstructor(ctx, ctor, 1, (JSValueConst *)&len);
    JS_FreeValue(ctx, len);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(arr)) { JS_FreeValue(ctx, obj); return arr; }

    if (src) {
        size_t abytes; JSValue ab = JS_GetTypedArrayBuffer(ctx, arr, NULL, NULL, NULL);
        uint8_t *p = JS_GetArrayBuffer(ctx, &abytes, ab);
        if (p && abytes >= n)
            for (int y = 0; y < h; y++)
                memcpy(p + (size_t)y * w * 4, src + (size_t)y * src_stride, (size_t)w * 4);
        JS_FreeValue(ctx, ab);
    }
    JS_SetPropertyStr(ctx, obj, "data", arr);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, h));
    return obj;
}

static JSValue cv_getImageData(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct canvas2d *c = cv_of(t);
    if (!c || !c->px) return JS_ThrowTypeError(ctx, "not a 2d context");
    int32_t x = 0, y = 0, w = 0, h = 0;
    if (argc < 4) return JS_ThrowTypeError(ctx, "getImageData needs 4 arguments");
    JS_ToInt32(ctx, &x, argv[0]); JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &w, argv[2]); JS_ToInt32(ctx, &h, argv[3]);
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if (w <= 0 || h <= 0) return JS_ThrowRangeError(ctx, "getImageData: zero-sized rect");
    /* The spec returns TRANSPARENT BLACK for pixels outside the canvas rather
     * than clamping the rect, so the returned ImageData is always exactly
     * w x h and a caller indexing it cannot walk off the end. */
    unsigned char *tmp = (unsigned char *)malloc((size_t)w * h * 4);
    if (!tmp) return JS_ThrowOutOfMemory(ctx);
    memset(tmp, 0, (size_t)w * h * 4);
    for (int yy = 0; yy < h; yy++) {
        int sy = y + yy;
        if (sy < 0 || sy >= c->h) continue;
        for (int xx = 0; xx < w; xx++) {
            int sx = x + xx;
            if (sx < 0 || sx >= c->w) continue;
            memcpy(tmp + ((size_t)yy * w + xx) * 4,
                   c->px + ((size_t)sy * c->w + sx) * 4, 4);
        }
    }
    JSValue r = make_imagedata(ctx, w, h, tmp, w * 4);
    free(tmp);
    return r;
}

static JSValue cv_createImageData(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int32_t w = 0, h = 0;
    if (argc < 2) return JS_ThrowTypeError(ctx, "createImageData needs 2 arguments");
    JS_ToInt32(ctx, &w, argv[0]); JS_ToInt32(ctx, &h, argv[1]);
    if (w < 0) w = -w;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0) return JS_ThrowRangeError(ctx, "createImageData: zero size");
    if ((long long)w * h > CV_MAXPX) return JS_ThrowRangeError(ctx, "createImageData: too large");
    return make_imagedata(ctx, w, h, NULL, 0);
}

/* putImageData REPLACES pixels: no compositing, no globalAlpha, and NOT
 * transformed by the CTM. That is the spec and it is also why this is a copy
 * loop rather than a call into the engine -- routing it through a paint would
 * quietly make it obey state it must ignore. */
static JSValue cv_putImageData(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct canvas2d *c = cv_of(t);
    if (!c || !c->px) return JS_ThrowTypeError(ctx, "not a 2d context");
    if (argc < 3) return JS_ThrowTypeError(ctx, "putImageData needs 3 arguments");
    JSValue dv = JS_GetPropertyStr(ctx, argv[0], "data");
    JSValue wv = JS_GetPropertyStr(ctx, argv[0], "width");
    JSValue hv = JS_GetPropertyStr(ctx, argv[0], "height");
    int32_t iw = 0, ih = 0, dx = 0, dy = 0;
    JS_ToInt32(ctx, &iw, wv); JS_ToInt32(ctx, &ih, hv);
    JS_ToInt32(ctx, &dx, argv[1]); JS_ToInt32(ctx, &dy, argv[2]);
    JS_FreeValue(ctx, wv); JS_FreeValue(ctx, hv);

    size_t abytes = 0; uint8_t *p = NULL;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, dv, NULL, NULL, NULL);
    if (!JS_IsException(ab)) p = JS_GetArrayBuffer(ctx, &abytes, ab);
    if (p && iw > 0 && ih > 0 && abytes >= (size_t)iw * ih * 4) {
        for (int yy = 0; yy < ih; yy++) {
            int ty = dy + yy;
            if (ty < 0 || ty >= c->h) continue;
            for (int xx = 0; xx < iw; xx++) {
                int tx = dx + xx;
                if (tx < 0 || tx >= c->w) continue;
                memcpy(c->px + ((size_t)ty * c->w + tx) * 4,
                       p + ((size_t)yy * iw + xx) * 4, 4);
            }
        }
    }
    JS_FreeValue(ctx, ab);
    JS_FreeValue(ctx, dv);
    return JS_UNDEFINED;
}

/* ---------------------------------------------------------- gradients -- */

struct grad_ref { struct canvas2d *c; int idx; };

static JSValue cv_grad_addstop(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct grad_ref *gr = (struct grad_ref *)JS_GetOpaque(t, cvgrad_class_id);
    if (!gr || !gr->c) return JS_UNDEFINED;
    struct cv_grad *g = &gr->c->grads[gr->idx];
    if (argc < 2) return JS_UNDEFINED;
    double off;
    if (JS_ToFloat64(ctx, &off, argv[0]) || off != off)
        return JS_ThrowRangeError(ctx, "addColorStop: offset is not a number");
    if (off < 0 || off > 1)
        return JS_ThrowRangeError(ctx, "addColorStop: offset outside 0..1");
    if (g->nstop >= GFX_MAX_STOPS) {
        printf("[canvas] addColorStop ignored: the engine holds %d stops\n", GFX_MAX_STOPS);
        return JS_UNDEFINED;
    }
    const char *s = JS_ToCString(ctx, argv[1]);
    unsigned char rgba[4] = { 0, 0, 0, 255 };
    int ok = s ? img_css_color(s, (int)strlen(s), rgba) : 0;
    if (s) JS_FreeCString(ctx, s);
    if (!ok) return JS_ThrowSyntaxError(ctx, "addColorStop: unparseable colour");
    g->stop[g->nstop].t = (int)(off * 65536.0);
    g->stop[g->nstop].color = GFX_RGB(rgba[0], rgba[1], rgba[2]);
    g->stop[g->nstop].alpha = rgba[3];
    g->nstop++;
    return JS_UNDEFINED;
}

static JSClassDef *cvgrad_classdef(void) { return &cvgrad_class; }

static void cvgrad_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    struct grad_ref *gr = (struct grad_ref *)JS_GetOpaque(val, cvgrad_class_id);
    if (gr) free(gr);
}

static JSValue new_gradient(JSContext *ctx, struct canvas2d *c, int kind,
                            int x0, int y0, int x1, int y1, int r)
{
    int idx = -1;
    for (int i = 0; i < CV_GRADS; i++) if (!c->grads[i].used) { idx = i; break; }
    if (idx < 0) {
        /* The slot table is per-context and small on purpose: a page that
         * makes a gradient per frame would grow it without bound, and a
         * gradient is state the fill reads, not an object the fill copies.
         * Reusing slot 0 would silently repaint someone else's gradient, so
         * this refuses out loud instead. */
        return JS_ThrowRangeError(ctx, "this context already holds %d gradients", CV_GRADS);
    }
    struct cv_grad *g = &c->grads[idx];
    memset(g, 0, sizeof *g);
    g->kind = kind; g->x0 = x0; g->y0 = y0; g->x1 = x1; g->y1 = y1; g->r = r;
    g->used = 1;

    JSValue o = JS_NewObjectClass(ctx, cvgrad_class_id);
    if (JS_IsException(o)) { g->used = 0; return o; }
    struct grad_ref *gr = (struct grad_ref *)malloc(sizeof *gr);
    if (!gr) { g->used = 0; JS_FreeValue(ctx, o); return JS_ThrowOutOfMemory(ctx); }
    gr->c = c; gr->idx = idx;
    JS_SetOpaque(o, gr);
    JS_SetPropertyStr(ctx, o, "addColorStop",
                      JS_NewCFunction(ctx, cv_grad_addstop, "addColorStop", 2));
    JS_SetPropertyStr(ctx, o, "__gradIndex", JS_NewInt32(ctx, idx));
    return o;
}

static JSValue cv_createLinear(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct canvas2d *c = cv_of(t);
    if (!c) return JS_ThrowTypeError(ctx, "not a 2d context");
    int a[4];
    if (argc < 4) return JS_ThrowTypeError(ctx, "createLinearGradient needs 4 arguments");
    for (int i = 0; i < 4; i++) if (arg_fx(ctx, argv[i], &a[i])) return JS_UNDEFINED;
    return new_gradient(ctx, c, GFX_LINEAR, a[0], a[1], a[2], a[3], 0);
}
/* gfx's radial paint is ONE circle; canvas names two. The end circle is used,
 * which is exact for createRadialGradient(x,y,0,x,y,r) -- concentric with a
 * degenerate start, and that is the form essentially every page writes. A
 * genuinely offset inner circle renders as concentric, and that is a stated
 * approximation rather than a silent one. */
static JSValue cv_createRadial(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct canvas2d *c = cv_of(t);
    if (!c) return JS_ThrowTypeError(ctx, "not a 2d context");
    int a[6];
    if (argc < 6) return JS_ThrowTypeError(ctx, "createRadialGradient needs 6 arguments");
    for (int i = 0; i < 6; i++) if (arg_fx(ctx, argv[i], &a[i])) return JS_UNDEFINED;
    return new_gradient(ctx, c, GFX_RADIAL, a[0], a[1], a[3], a[4], a[5]);
}

/* ---------------------------------------------------------- properties -- */

static JSValue cv_get_canvas(JSContext *ctx, JSValueConst t)
{
    struct canvas2d *c = cv_of(t);
    if (!c) return JS_UNDEFINED;
    return JS_DupValue(ctx, c->elval);
}

/* HTML's "serialize a colour": #rrggbb when the alpha is 1, otherwise
 * `rgba(r, g, b, a)` -- with the spaces, which are part of the grammar CSS
 * serializes to and which a page comparing two serializations will see.
 *
 * Getting this wrong is not cosmetic. `ctx.fillStyle = ctx.fillStyle` is a
 * real idiom, so the output has to be something the INPUT parser can read
 * back; the gate asserts that round trip rather than a string measured from
 * another browser, because the property that matters here is "we do not lose
 * the colour" and that is checkable without one.
 *
 * The alpha is printed to at most three decimals with trailing zeros trimmed,
 * which is what CSS's "shortest that round-trips" comes to for a value that
 * started life as one of 256 steps. */
static JSValue style_get(JSContext *ctx, const unsigned char *col)
{
    char buf[48];
    static const char *hex = "0123456789abcdef";
    if (col[3] == 255) {
        buf[0] = '#';
        buf[1] = hex[col[0] >> 4]; buf[2] = hex[col[0] & 15];
        buf[3] = hex[col[1] >> 4]; buf[4] = hex[col[1] & 15];
        buf[5] = hex[col[2] >> 4]; buf[6] = hex[col[2] & 15];
        buf[7] = 0;
        return JS_NewString(ctx, buf);
    }
    int n = 0;
    const char *pre = "rgba(";
    while (*pre) buf[n++] = *pre++;
    for (int k = 0; k < 3; k++) {
        int v = col[k];
        if (v >= 100) buf[n++] = (char)('0' + v / 100);
        if (v >= 10)  buf[n++] = (char)('0' + (v / 10) % 10);
        buf[n++] = (char)('0' + v % 10);
        buf[n++] = ','; buf[n++] = ' ';
    }
    int a1000 = (col[3] * 1000 + 127) / 255;
    if (a1000 >= 1000) { buf[n++] = '1'; }
    else {
        int d0 = a1000 / 100, d1 = (a1000 / 10) % 10, d2 = a1000 % 10;
        buf[n++] = '0'; buf[n++] = '.';
        buf[n++] = (char)('0' + d0);
        if (d1 || d2) buf[n++] = (char)('0' + d1);
        if (d2)       buf[n++] = (char)('0' + d2);
    }
    buf[n++] = ')';
    buf[n] = 0;
    return JS_NewString(ctx, buf);
}

static int style_set(JSContext *ctx, JSValueConst v, unsigned char *col, int *grad)
{
    if (JS_GetOpaque(v, cvgrad_class_id)) {
        struct grad_ref *gr = (struct grad_ref *)JS_GetOpaque(v, cvgrad_class_id);
        *grad = gr ? gr->idx : -1;
        return 1;
    }
    const char *s = JS_ToCString(ctx, v);
    if (!s) return 0;
    unsigned char rgba[4];
    int ok = img_css_color(s, (int)strlen(s), rgba);
    JS_FreeCString(ctx, s);
    /* The spec: an unparseable value is IGNORED, leaving the previous one.
     * Not an exception, and not black -- silently going black is how a chart
     * loses its series colours without anything to find. */
    if (!ok) return 0;
    col[0] = rgba[0]; col[1] = rgba[1]; col[2] = rgba[2]; col[3] = rgba[3];
    *grad = -1;
    return 1;
}

static JSValue cv_get_fill(JSContext *ctx, JSValueConst t)
{ struct canvas2d *c = cv_of(t); return c ? style_get(ctx, c->st.fill) : JS_UNDEFINED; }
static JSValue cv_set_fill(JSContext *ctx, JSValueConst t, JSValueConst v)
{ struct canvas2d *c = cv_of(t); if (c) style_set(ctx, v, c->st.fill, &c->st.fill_grad);
  return JS_UNDEFINED; }
static JSValue cv_get_strokes(JSContext *ctx, JSValueConst t)
{ struct canvas2d *c = cv_of(t); return c ? style_get(ctx, c->st.stroke) : JS_UNDEFINED; }
static JSValue cv_set_strokes(JSContext *ctx, JSValueConst t, JSValueConst v)
{ struct canvas2d *c = cv_of(t); if (c) style_set(ctx, v, c->st.stroke, &c->st.stroke_grad);
  return JS_UNDEFINED; }

static JSValue cv_get_alpha(JSContext *ctx, JSValueConst t)
{ struct canvas2d *c = cv_of(t); return c ? JS_NewFloat64(ctx, c->st.galpha / 255.0) : JS_UNDEFINED; }
static JSValue cv_set_alpha(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct canvas2d *c = cv_of(t); double d;
    if (!c || JS_ToFloat64(ctx, &d, v) || d != d) return JS_UNDEFINED;
    if (d < 0 || d > 1) return JS_UNDEFINED;      /* spec: out of range is ignored */
    c->st.galpha = clamp255((int)(d * 255.0 + 0.5));
    return JS_UNDEFINED;
}
static JSValue cv_get_lw(JSContext *ctx, JSValueConst t)
{ struct canvas2d *c = cv_of(t); return c ? JS_NewFloat64(ctx, unfx(c->st.lw)) : JS_UNDEFINED; }
static JSValue cv_set_lw(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct canvas2d *c = cv_of(t); double d;
    if (!c || JS_ToFloat64(ctx, &d, v) || d != d || d <= 0) return JS_UNDEFINED;
    c->st.lw = fx(d);
    return JS_UNDEFINED;
}
static JSValue cv_get_cap(JSContext *ctx, JSValueConst t)
{
    struct canvas2d *c = cv_of(t);
    if (!c) return JS_UNDEFINED;
    return JS_NewString(ctx, c->st.cap == GFX_CAP_ROUND ? "round" :
                             c->st.cap == GFX_CAP_SQUARE ? "square" : "butt");
}
static JSValue cv_set_cap(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct canvas2d *c = cv_of(t);
    const char *s = c ? JS_ToCString(ctx, v) : NULL;
    if (!s) return JS_UNDEFINED;
    if (!strcmp(s, "round")) c->st.cap = GFX_CAP_ROUND;
    else if (!strcmp(s, "square")) c->st.cap = GFX_CAP_SQUARE;
    else if (!strcmp(s, "butt")) c->st.cap = GFX_CAP_BUTT;
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
static JSValue cv_get_join(JSContext *ctx, JSValueConst t)
{
    struct canvas2d *c = cv_of(t);
    if (!c) return JS_UNDEFINED;
    return JS_NewString(ctx, c->st.join == GFX_JOIN_ROUND ? "round" :
                             c->st.join == GFX_JOIN_BEVEL ? "bevel" : "miter");
}
static JSValue cv_set_join(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct canvas2d *c = cv_of(t);
    const char *s = c ? JS_ToCString(ctx, v) : NULL;
    if (!s) return JS_UNDEFINED;
    if (!strcmp(s, "round")) c->st.join = GFX_JOIN_ROUND;
    else if (!strcmp(s, "bevel")) c->st.join = GFX_JOIN_BEVEL;
    else if (!strcmp(s, "miter")) c->st.join = GFX_JOIN_MITER;
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
static JSValue cv_get_miter(JSContext *ctx, JSValueConst t)
{ struct canvas2d *c = cv_of(t); return c ? JS_NewFloat64(ctx, c->st.miter / 65536.0) : JS_UNDEFINED; }
static JSValue cv_set_miter(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct canvas2d *c = cv_of(t); double d;
    if (!c || JS_ToFloat64(ctx, &d, v) || d != d || d <= 0) return JS_UNDEFINED;
    c->st.miter = (int)(d * 65536.0);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry cv_proto_funcs[] = {
    JS_CGETSET_DEF("canvas", cv_get_canvas, NULL),
    JS_CGETSET_DEF("fillStyle", cv_get_fill, cv_set_fill),
    JS_CGETSET_DEF("strokeStyle", cv_get_strokes, cv_set_strokes),
    JS_CGETSET_DEF("globalAlpha", cv_get_alpha, cv_set_alpha),
    JS_CGETSET_DEF("lineWidth", cv_get_lw, cv_set_lw),
    JS_CGETSET_DEF("lineCap", cv_get_cap, cv_set_cap),
    JS_CGETSET_DEF("lineJoin", cv_get_join, cv_set_join),
    JS_CGETSET_DEF("miterLimit", cv_get_miter, cv_set_miter),
    JS_CFUNC_DEF("save", 0, cv_save),
    JS_CFUNC_DEF("restore", 0, cv_restore),
    JS_CFUNC_DEF("translate", 2, cv_translate),
    JS_CFUNC_DEF("scale", 2, cv_scale),
    JS_CFUNC_DEF("rotate", 1, cv_rotate),
    JS_CFUNC_DEF("transform", 6, cv_transform),
    JS_CFUNC_DEF("setTransform", 6, cv_setTransform),
    JS_CFUNC_DEF("resetTransform", 0, cv_resetTransform),
    JS_CFUNC_DEF("beginPath", 0, cv_beginPath),
    JS_CFUNC_DEF("closePath", 0, cv_closePath),
    JS_CFUNC_DEF("moveTo", 2, cv_moveTo),
    JS_CFUNC_DEF("lineTo", 2, cv_lineTo),
    JS_CFUNC_DEF("quadraticCurveTo", 4, cv_quadTo),
    JS_CFUNC_DEF("bezierCurveTo", 6, cv_bezierTo),
    JS_CFUNC_DEF("rect", 4, cv_rectpath),
    JS_CFUNC_DEF("arc", 5, cv_arc),
    JS_CFUNC_DEF("ellipse", 7, cv_ellipse),
    JS_CFUNC_DEF("fill", 0, cv_fill),
    JS_CFUNC_DEF("stroke", 0, cv_stroke),
    JS_CFUNC_DEF("fillRect", 4, cv_fillRect),
    JS_CFUNC_DEF("strokeRect", 4, cv_strokeRect),
    JS_CFUNC_DEF("clearRect", 4, cv_clearRect),
    JS_CFUNC_DEF("getImageData", 4, cv_getImageData),
    JS_CFUNC_DEF("putImageData", 3, cv_putImageData),
    JS_CFUNC_DEF("createImageData", 2, cv_createImageData),
    JS_CFUNC_DEF("createLinearGradient", 4, cv_createLinear),
    JS_CFUNC_DEF("createRadialGradient", 6, cv_createRadial),
};

/* ------------------------------------------------------ the element side -- */

static int attr_int(struct node *n, const char *name, int dflt)
{
    int len = 0;
    const char *v = js_dom_attr_len(n, name, &len);
    if (!v || len <= 0) return dflt;
    int acc = 0, i = 0, any = 0;
    while (i < len && (v[i] == ' ' || v[i] == '\t')) i++;
    for (; i < len && v[i] >= '0' && v[i] <= '9'; i++) {
        acc = acc * 10 + (v[i] - '0'); any = 1;
        if (acc > 1 << 20) break;
    }
    return any ? acc : dflt;
}

/* Allocate (or reallocate) the backing store. The spec: setting width or
 * height RESETS the canvas -- pixels, state stack and CTM all go -- even when
 * the value is unchanged. That is not a quirk to work around; it is how every
 * page in existence clears a canvas. */
static int cv_realloc(struct canvas2d *c, int w, int h)
{
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if ((long long)w * h > CV_MAXPX) return 0;
    unsigned char *p = (unsigned char *)malloc((size_t)w * h * 4);
    if (!p) return 0;
    memset(p, 0, (size_t)w * h * 4);
    if (c->px) free(c->px);
    c->px = p; c->w = w; c->h = h;
    c->nstack = 0;
    st_reset(&c->st);
    path_reset(c);
    for (int i = 0; i < CV_GRADS; i++) c->grads[i].used = 0;
    return 1;
}

static JSValue el_getContext(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = js_dom_node_from(t);
    if (!n) return JS_NULL;
    const char *type = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    int is2d = type && (!strcmp(type, "2d") || !strcmp(type, "2D"));
    if (type) JS_FreeCString(ctx, type);
    /* Anything else -- webgl, webgl2, bitmaprenderer -- is null, which is not
     * a refusal but the truth: this browser has no such context. */
    if (!is2d) return JS_NULL;

    /* One context per canvas, as the spec requires: two getContext('2d') calls
     * return the SAME object, and a page that keeps a reference across a
     * re-query must see its own state. */
    JSValue have = JS_GetPropertyStr(ctx, t, "__ctx2d");
    if (!JS_IsUndefined(have) && !JS_IsNull(have)) return have;
    JS_FreeValue(ctx, have);

    struct canvas2d *c = (struct canvas2d *)malloc(sizeof *c);
    if (!c) return JS_NULL;
    memset(c, 0, sizeof *c);
    c->el = n;
    c->elval = JS_DupValue(ctx, t);
    if (!cv_realloc(c, attr_int(n, "width", 300), attr_int(n, "height", 150))) {
        JS_FreeValue(ctx, c->elval);
        free(c);
        printf("[canvas] getContext('2d') refused: %dx%d exceeds %d pixels\n",
               attr_int(n, "width", 300), attr_int(n, "height", 150), CV_MAXPX);
        return JS_NULL;
    }

    JSValue obj = JS_NewObjectClass(ctx, cv_class_id);
    if (JS_IsException(obj)) { JS_FreeValue(ctx, c->elval); free(c->px); free(c); return obj; }
    JS_SetOpaque(obj, c);
    JS_SetPropertyStr(ctx, t, "__ctx2d", JS_DupValue(ctx, obj));
    c->next = g_all; g_all = c;
    return obj;
}

/* width/height are the IDL attributes: they read the content attribute and
 * write it back, and writing resets the surface. Reading falls back to the
 * spec's 300x150 when the attribute is absent or not a valid non-negative
 * integer, which is also what a page that never set them expects. */
static JSValue el_get_w(JSContext *ctx, JSValueConst t)
{
    struct node *n = js_dom_node_from(t);
    return JS_NewInt32(ctx, n ? attr_int(n, "width", 300) : 300);
}
static JSValue el_get_h(JSContext *ctx, JSValueConst t)
{
    struct node *n = js_dom_node_from(t);
    return JS_NewInt32(ctx, n ? attr_int(n, "height", 150) : 150);
}
static void el_set_dim(JSContext *ctx, JSValueConst t, JSValueConst v, const char *name)
{
    struct node *n = js_dom_node_from(t);
    if (!n) return;
    int32_t iv = 0;
    if (JS_ToInt32(ctx, &iv, v)) return;
    if (iv < 0) iv = 0;
    char buf[16]; int k = 0;
    if (iv == 0) buf[k++] = '0';
    else { char tmp[16]; int m = 0; int x = iv;
           while (x) { tmp[m++] = (char)('0' + x % 10); x /= 10; }
           while (m) buf[k++] = tmp[--m]; }
    buf[k] = 0;
    js_dom_attr_write(ctx, n, name, buf, k);
    JSValue have = JS_GetPropertyStr(ctx, t, "__ctx2d");
    struct canvas2d *c = cv_of(have);
    if (c) cv_realloc(c, attr_int(n, "width", 300), attr_int(n, "height", 150));
    JS_FreeValue(ctx, have);
}
static JSValue el_set_w(JSContext *ctx, JSValueConst t, JSValueConst v)
{ el_set_dim(ctx, t, v, "width"); return JS_UNDEFINED; }
static JSValue el_set_h(JSContext *ctx, JSValueConst t, JSValueConst v)
{ el_set_dim(ctx, t, v, "height"); return JS_UNDEFINED; }

static JSValue el_toDataURL(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    /* Refused by name. This tree decodes PNG and does not encode it, and a
     * fabricated data URL is believed rather than detected -- it is what every
     * fingerprint and every format-support probe reads. */
    return JS_ThrowTypeError(ctx,
        "canvas.toDataURL is not implemented: this browser has no image encoder, "
        "and a fabricated data URL would be believed rather than detected");
}

static const JSCFunctionListEntry canvas_el_funcs[] = {
    JS_CGETSET_DEF("width", el_get_w, el_set_w),
    JS_CGETSET_DEF("height", el_get_h, el_set_h),
    JS_CFUNC_DEF("getContext", 1, el_getContext),
    JS_CFUNC_DEF("toDataURL", 0, el_toDataURL),
    JS_CFUNC_DEF("toBlob", 1, el_toDataURL),
};

/* ------------------------------------------------------- reaching the screen --
 *
 * layout.c reserves an IT_CANVAS box and browser_paint.c asks for the pixels
 * here, weakly -- the same split IT_VIDEO and IT_CONTROL use, and for the same
 * reason: a script can repaint a canvas between two frames without layout
 * running at all, so the bitmap cannot be layout's to own.
 *
 * The painter has a `struct node *` and the context is reachable only from the
 * element's JS wrapper, so a small registry closes the gap. It is an ARRAY and
 * not a hash because a page with more than a handful of canvases is not the
 * case this is sized for, and a linear scan over eight entries costs less than
 * the hash would; the cap is stated and a canvas past it simply does not
 * reach the screen, which is visible rather than silent.
 *
 * The backing store is straight RGBA8 -- what SYS_GUI_BLIT consumes -- so this
 * is one blit and no conversion, the same property that made getImageData a
 * copy. The bitmap is drawn at its OWN size into the box CSS gave it; when the
 * two differ the compositor's nearest-neighbour rescale applies, which is the
 * spec's behaviour and is why the two sizes are separate quantities. */
/* Returns the backing store, or NULL when that element has no context yet --
 * the ordinary case for a canvas the page has not drawn into.
 *
 * It HANDS OVER the pixels rather than blitting them, and that is what keeps
 * this file host-linkable: gui_blit is a static inline over int 0x80 in
 * c/apps/logit.h, and pulling that in would make js_canvas.c a ring-3-only TU
 * that tests/canvas.mk could not build. The painter already blits IT_IMAGE, so
 * the call site exists there anyway. */
const unsigned char *canvas_pixels(struct node *n, int *w, int *h)
{
    for (struct canvas2d *c = g_all; c; c = c->next)
        if (c->el == n && c->px) { *w = c->w; *h = c->h; return c->px; }
    return 0;
}

/* --------------------------------------------------------------- install -- */

void js_canvas_install(JSContext *ctx)
{
    JS_NewClassID(&cv_class_id);
    JS_NewClass(JS_GetRuntime(ctx), cv_class_id, &cv_class);
    cvgrad_class.finalizer = cvgrad_finalizer;
    JS_NewClassID(&cvgrad_class_id);
    JS_NewClass(JS_GetRuntime(ctx), cvgrad_class_id, cvgrad_classdef());

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, cv_proto_funcs,
                               (int)(sizeof cv_proto_funcs / sizeof cv_proto_funcs[0]));
    JS_SetClassProto(ctx, cv_class_id, proto);

    JSValue g = JS_GetGlobalObject(ctx);
    /* By NAME, not by walking up from a created element -- js_select.c's
     * header documents why the walk lands a member on the wrong interface. */
    JSValue ce = JS_GetPropertyStr(ctx, g, "HTMLCanvasElement");
    if (JS_IsObject(ce)) {
        JSValue cp = JS_GetPropertyStr(ctx, ce, "prototype");
        if (JS_IsObject(cp))
            JS_SetPropertyFunctionList(ctx, cp, canvas_el_funcs,
                                       (int)(sizeof canvas_el_funcs / sizeof canvas_el_funcs[0]));
        JS_FreeValue(ctx, cp);
    } else {
        /* No interface objects in this build (a host harness that links
         * js_dom.c without js_platform.c). Saying so beats installing nothing
         * silently, because the symptom one layer up is `getContext is not a
         * function` -- the very message this file exists to remove. */
        printf("[canvas] HTMLCanvasElement is absent; the 2d context is not reachable\n");
    }
    JS_FreeValue(ctx, ce);
    JS_FreeValue(ctx, g);
}
