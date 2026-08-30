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
 * ~~`toDataURL` and `toBlob` THROW. This tree decodes PNG and does not encode
 * it, and a fabricated data URL is the single most load-bearing lie a canvas
 * can tell: it is what every fingerprint and every "does this browser support
 * webp" probe reads, and a wrong one is believed rather than detected.~~
 *
 * THAT ARGUMENT WAS RIGHT AND ITS PREMISE IS GONE, 2026-08-29. The sentence
 * is kept above rather than deleted because somebody will arrive holding it.
 * It is an argument about FABRICATION, not about the method: a wrong data URL
 * is believed rather than detected, so do not produce a wrong one. The exit
 * was never to keep throwing, it was to make the answer true --
 * `rust/src/pngenc.rs` encodes the backing store for real, and the half of the
 * sentence that said "this tree ... does not encode it" is simply no longer a
 * fact about this tree.
 *
 * WHAT THAT DOES AND DOES NOT PROMISE. The complaint this came from is a
 * Cloudflare interstitial that never renders: a bot check's first act is a
 * canvas fingerprint, `toDataURL` threw, the challenge script died on its
 * first statement and the widget never appeared. Encoding honestly means the
 * script RUNS and the PAGE LOADS. It does not mean the challenge passes, and
 * nothing here is tuned so that it would -- no output mimics another browser,
 * no signal is spoofed. An honest fingerprint that says "this is LogitOS" is
 * a correct result even when it is refused, and if a service declines a
 * from-scratch browser that is that service's decision to make.
 *
 * WHAT IS STILL REFUSED, and it is the interesting half of the rule surviving:
 * `image/jpeg` and `image/webp`. HTML's own text says a user agent that cannot
 * produce the requested type MUST use `image/png`, and the returned URL
 * DECLARES its type -- so a caller that asked for webp reads back
 * `data:image/png;base64,...` and can see exactly what it got. Falling back
 * and saying so is honest; the lie would be the `data:image/webp` prefix over
 * PNG bytes, which is precisely the "does this browser support webp" probe the
 * old note named. So the fallback is taken, it is announced on the console the
 * first time each type is asked for, and the string never claims otherwise.
 *
 * `getContext` of anything but "2d" returns null. For webgl that is not a
 * refusal, it is the truth.
 *
 * NOT HERE YET, and deliberately left to throw so that the same instrument
 * which chose this file chooses what comes next: drawImage, fillText /
 * strokeText / measureText, ~~clip(),~~ and the composite operations beyond
 * source-over.
 *
 * `clip()` IS HERE (cv_clip, and it honours evenodd and intersects with the
 * enclosing clip). The struck word is left because the list above is quoted
 * as an inventory and a stale entry in an inventory reads as a decision.
 *
 * ---------------------------------------------------------------------------
 * THE NEXT WALL, MEASURED IN THE GUEST 2026-08-29, NOT DERIVED FROM THIS LIST
 * ---------------------------------------------------------------------------
 * tests/qmp/qmp_canvas_fingerprint.py runs FingerprintJS's canvas component
 * VERBATIM at its real 2000x200 and reports where it stops. With toDataURL
 * honest, it now gets through createElement, getContext, two rect()s,
 * isPointInPath(5,5,'evenodd') -- which answers `winding:yes`, the same
 * discriminator the real probe reads -- textBaseline, fillStyle and fillRect,
 * and throws 10 ms in at
 *
 *     TypeError: fillText is not a function (it is undefined)
 *
 * So the canvas fingerprint is no longer stopped by the READBACK; it is
 * stopped by TEXT. That is a different and much better problem, and it is the
 * next work order for this file. Note what it needs: `gui_text_run()` is a
 * kernel syscall that paints into a WINDOW, and a canvas needs glyphs
 * rasterised into an offscreen RGBA surface instead -- c/lib/text/glyphras.c
 * already converts an outline to a gfx_path, which is the seam.
 *
 * ---------------------------------------------------------------------------
 * AND ON REAL PAGES IT MOVED NOTHING, MEASURED BOTH WAYS 2026-08-30
 * ---------------------------------------------------------------------------
 * The sentence this change was made for is "the challenge script died on its
 * first statement". That is a COMPARISON, and until -DCANVAS_READBACK_REFUSE
 * existed (see el_toDataURL) there was no second term: every run was an after
 * run and the before half was quoted from the complaint. So both halves were
 * built -- one disk image with the readback, one identical image with the
 * pre-2026-08-29 refusal, same kernel, same everything else -- and the same
 * real pages loaded on each.
 *
 *   nowsecure.nl (a live Cloudflare Turnstile interstitial)
 *     BEFORE 620,492 px, 2 exceptions, 16 requests-equivalent, 3.7 s
 *     AFTER  620,408 px, 2 exceptions, same two, same counts, 4.9 s
 *   qq.com (25 of the 33 getContext calls that chose this file)
 *     BEFORE 243,976 px, 71 text runs, 1,141 B, no exceptions
 *     AFTER  243,884 px, 71 text runs, 1,141 B, no exceptions
 *
 * IDENTICAL. The readback is not on either page's path, and the reason is
 * checkable rather than guessed: nowsecure.nl's own document contains zero
 * occurrences of toDataURL, toBlob or getContext, and its Turnstile loader is a
 * plain <script src> that this browser fetches over a verified TLS chain and
 * then throws out of at TOP LEVEL --
 *
 *     SyntaxError: invalid escape sequence in regular expression
 *         at RegExp (native)   at <eval> (challenges.cloudflare.com/.../api.js)
 *
 * -- so the widget never reaches a canvas at all. google.com renders with no
 * exceptions and served no challenge on the day it was asked.
 *
 * THAT IS THE HONEST SHAPE OF THIS WORK and it is worth more than the success
 * story it replaces: the readback is correct, it is proved correct in the guest
 * against pages that DO call it, and on the three real pages measured so far it
 * is not the thing standing in the way. The apparatus itself was checked before
 * the null result was believed -- the refusing image reddens
 * `test-canvas-readback-os` on its first assertion with the old message on the
 * serial log, so the two images really do differ and "identical" is a
 * measurement rather than a build that did not take.
 *
 * AND ONE THING HERE IS PRESENT-AND-WRONG, which this file's own rule ranks
 * below absent. `globalCompositeOperation` is not in cv_proto, so it is not a
 * property with a setter -- but a plain JS assignment STORES it on the object
 * and reads back the value that was set. Measured: the guest reports
 * `typeof ctx.globalCompositeOperation` as "undefined" and then, after
 * `ctx.globalCompositeOperation = 'multiply'`, reports 'multiply'. Every draw
 * is still src-over. A probe that sets it and reads it back to decide whether
 * the mode took -- which is exactly how a page feature-detects it -- is told
 * yes and is wrong. HTML says an unsupported value must be IGNORED, leaving
 * the attribute at its previous value, so the honest implementation is a
 * getter/setter that accepts 'source-over' and silently keeps 'source-over'
 * for anything this engine cannot do. That is a real accessor, not a
 * deletion, and it is deliberately NOT done in this pass: it could not be
 * rebuilt and re-measured in the guest at the time it was found (the shared
 * tree's third_party/quickjs was mid-edit by another line of work and would
 * not compile), and shipping an unverified accessor is the failure this file
 * spends its header warning about.
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "gfx.h"
#include "img.h"
/* c/net/ssh/base64.h -- the tree's one C base64, reached through the flat
 * INCDIRS list. The name is unique across `find c include -name base64.h`, so
 * this is not the basename-collision trap CLAUDE.md's layout section names. */
#include "base64.h"

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

/* clip() is state: it saves and restores with everything else in cv_state, and
 * two states may share ONE buffer -- save() copies the pointer, not the
 * pixels, and bumps `refs`. That is what keeps a page that never calls clip()
 * paying nothing for it, and what keeps a save/restore pair inside a loop from
 * reallocating a device-sized mask every iteration. clip() itself always
 * writes a FRESH buffer (never mutates one that might be shared with a saved
 * state) and drops its own reference to the old one, so a state on the stack
 * never sees a clip mutate out from under it. */
struct cv_clip {
    int refs;
    unsigned char *cov;    /* c->w * c->h bytes, device-sized: 255 visible..0 clipped */
};

struct cv_state {
    struct gfx_matrix m;
    unsigned char fill[4], stroke[4];
    int galpha;                    /* 0..255 */
    int lw;                        /* user units, 24.8 */
    int cap, join;
    int miter;                     /* 16.16 */
    int fill_grad, stroke_grad;    /* index into grads[], -1 = solid colour */
    struct cv_clip *clip;          /* NULL = unclipped */
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

/* Read one number argument as 24.8. Returns -1 when the value is NaN,
 * +-Infinity, or not convertible, which the spec treats as "ignore this
 * call" for the geometry entry points -- not as an error to throw.
 *
 * isfinite(), NOT `d != d`. `d != d` only catches NaN -- Infinity passes it
 * (Infinity == Infinity) straight into fx(), which SATURATES rather than
 * refuses (fx()'s own job is turning an enormous but ordinary coordinate
 * into a clamped 24.8 value, not distinguishing "huge" from "infinite"). A
 * page's `ctx.strokeRect(Infinity, 0, 100, 50)` must be a no-op per spec; a
 * `d != d` check here quietly turns it into a rect at the saturated edge of
 * the fixed-point range instead, and .nonfinite.html across moveTo, lineTo,
 * rect, strokeRect, quadraticCurveTo and bezierCurveTo is that exact case
 * for every one of the entry points that route through this function. */
static int arg_fx(JSContext *ctx, JSValueConst v, int *out)
{
    double d;
    if (JS_ToFloat64(ctx, &d, v)) return -1;
    if (!isfinite(d)) return -1;
    *out = fx(d);
    return 0;
}

static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* A real DOMException, for the one canvas error the spec names as one:
 * arcTo's negative radius is "IndexSizeError", not a plain RangeError, and
 * assert_throws_dom checks e.name -- a plain Error would silently miss.
 * js_semantics.c has the identical helper under a different name for the
 * identical reason (a different TU, no shared header for it); the fallback to
 * a plain error when DOMException is absent from the link is the same
 * "this file must never be why a page fails to load" rule as everywhere
 * else here. */
static JSValue throw_dom(JSContext *ctx, const char *name, const char *msg)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue de = JS_GetPropertyStr(ctx, g, "DOMException");
    JS_FreeValue(ctx, g);
    if (JS_IsFunction(ctx, de)) {
        JSValue args[2] = { JS_NewString(ctx, msg ? msg : name), JS_NewString(ctx, name) };
        JSValue e = JS_CallConstructor(ctx, de, 2, (JSValueConst *)args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]); JS_FreeValue(ctx, de);
        if (!JS_IsException(e)) return JS_Throw(ctx, e);
    } else {
        JS_FreeValue(ctx, de);
    }
    return JS_ThrowTypeError(ctx, "%s: %s", name, msg ? msg : name);
}

/* Forward declaration: cv_realloc lives down by the element side (it is the
 * width/height IDL setters' primitive too), but cv_reset -- a context method,
 * grouped with the other context methods above the element side -- needs it
 * for exactly the same "resize resets everything" behaviour. */
static int cv_realloc(struct canvas2d *c, int w, int h);

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
    s->clip = NULL;
}

static void clip_unref(struct cv_clip *cl)
{
    if (!cl) return;
    if (--cl->refs <= 0) { free(cl->cov); free(cl); }
}

/* Drops every clip reference this context holds -- the active state's and
 * every saved one's -- and empties the save stack. Called from cv_realloc
 * (whose resize invalidates every clip buffer's device size at once) and from
 * the finalizer (nothing after this reads c->stack again). Doing this one
 * place rather than at each call site is what makes it safe to add a THIRD
 * caller later without re-deriving the two-list walk. */
static void clip_release_all(struct canvas2d *c)
{
    if (c->st.clip) { clip_unref(c->st.clip); c->st.clip = NULL; }
    for (int i = 0; i < c->nstack; i++)
        if (c->stack[i].clip) clip_unref(c->stack[i].clip);
    c->nstack = 0;
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
    clip_release_all(c);
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
    if (c->st.clip) {
        struct gfx_clip_mask cm = { c->st.clip->cov, c->w, c->h, 0, 0 };
        gfx_fill_clipped(&s, path, rule, &p, &clip, GFX_SUBS, &cm);
    } else {
        gfx_fill(&s, path, rule, &p, &clip);
    }
}

/* lineWidth is in user units; the stroker wants device. A non-uniform CTM
 * cannot be expressed as one width -- a real implementation strokes in user
 * space and transforms the outline -- so this uses gfx's approximate uniform
 * scale and is exact for the uniform case, which is every canvas that has not
 * called scale() with two different factors.
 *
 * Shared between do_stroke() and isPointInStroke(): both need exactly the
 * same outline, and a second copy of this arithmetic that drifted from the
 * first would make a point "in the stroke" that the stroke itself does not
 * paint, or the reverse. `src` is a caller path (do_stroke's is c->path,
 * isPointInStroke's may be a throwaway) so this has no opinion on where the
 * source geometry came from. */
/* Returns 1 on success, 0 when the outline genuinely did not fit (a real
 * refusal worth reporting), -1 when lineWidth resolves to nothing to stroke
 * (not a refusal -- a page setting lineWidth to 0 is asking for nothing to
 * appear, silently, exactly like the fill/stroke style rules elsewhere in
 * this file). Two different zeroes, so the caller can tell them apart. */
static int build_stroke_outline(struct canvas2d *c, const struct gfx_path *src,
                                struct gfx_path *out)
{
    int scale = gfx_m_scale_of(&c->st.m);
    struct gfx_stroke sk;
    memset(&sk, 0, sizeof sk);
    sk.width = (int)(((long long)c->st.lw * scale) >> 16);
    if (sk.width <= 0) return -1;
    sk.cap = c->st.cap;
    sk.join = c->st.join;
    sk.miter_limit = c->st.miter;

    static int opts[CV_PTS * 6];
    static int osub[CV_SUBS * 4];
    gfx_path_init(out, opts, CV_PTS * 3, osub, CV_SUBS * 4);
    return gfx_stroke_path(out, src, &sk) ? 1 : 0;
}

static void do_stroke(struct canvas2d *c, struct gfx_path *path)
{
    if (!c->px || !path_ok(c, path, "stroke")) return;
    struct gfx_path out;
    int r = build_stroke_outline(c, path, &out);
    if (r <= 0) {
        if (r == 0)
            printf("[canvas] stroke refused: the outline did not fit %d points "
                   "(source path has %d)\n", CV_PTS * 3, path->npt);
        return;
    }
    struct gfx_surface s; surf_of(c, &s);
    struct gfx_paint p;   make_paint(c, 1, &p);
    struct gfx_rect clip = { 0, 0, c->w, c->h };
    if (c->st.clip) {
        struct gfx_clip_mask cm = { c->st.clip->cov, c->w, c->h, 0, 0 };
        gfx_fill_clipped(&s, &out, GFX_NONZERO, &p, &clip, GFX_SUBS, &cm);
    } else {
        gfx_fill(&s, &out, GFX_NONZERO, &p, &clip);
    }
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
    /* The struct copy above duplicated the clip POINTER, not the pixels -- the
     * stack slot and the still-active c->st now both reference it, so the
     * refcount has to say so or a later clip_unref from one side would free a
     * buffer the other side still reads. */
    if (c->st.clip) c->st.clip->refs++;
    return JS_UNDEFINED;
}
static JSValue cv_restore(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv; CV_THIS;
    if (c->nstack > 0) {
        /* Drop the reference the outgoing active state held; the popped
         * stack slot's reference (taken out at the matching save() above)
         * transfers to the new active state without a further increment. */
        if (c->st.clip) clip_unref(c->st.clip);
        c->st = c->stack[--c->nstack];
    }
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
/* The tangent-circle construction every browser uses (there is no closed form
 * that avoids it): a circle of the given radius, tangent to both the
 * (current point)->(x1,y1) segment and the (x1,y1)->(x2,y2) segment. Three
 * cases fall out of the same geometry rather than needing special-casing --
 * no current point, a zero radius, and collinear/degenerate segments all end
 * up drawing a plain line to (x1, y1), which is exactly the spec's fallback
 * for all three. */
static JSValue cv_arcTo(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS;
    double x1, y1, x2, y2, radius;
    if (argc < 5 || JS_ToFloat64(ctx, &x1, argv[0]) || JS_ToFloat64(ctx, &y1, argv[1]) ||
        JS_ToFloat64(ctx, &x2, argv[2]) || JS_ToFloat64(ctx, &y2, argv[3]) ||
        JS_ToFloat64(ctx, &radius, argv[4]))
        return JS_UNDEFINED;
    /* isfinite, not `v == v` -- the corpus's own nonfinite.html feeds Infinity
     * through here and expects it silently ignored (step 1 of the spec's
     * algorithm), and Infinity survives a NaN-only check (Infinity == Infinity
     * is true) straight into `radius < 0`, where -Infinity reads as negative
     * and turns "ignored" into "throws". The two are different failures the
     * spec keeps apart on purpose: non-finite is silent, negative is not. */
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2) || !isfinite(radius))
        return JS_UNDEFINED;
    if (radius < 0)
        return throw_dom(ctx, "IndexSizeError", "arcTo: radius must not be negative");

    if (!c->has_pt) { emit_move(c, fx(x1), fx(y1)); return JS_UNDEFINED; }

    double x0 = unfx(c->curx), y0 = unfx(c->cury);
    double a1x = x0 - x1, a1y = y0 - y1;
    double a2x = x2 - x1, a2y = y2 - y1;
    double len1 = sqrt(a1x * a1x + a1y * a1y);
    double len2 = sqrt(a2x * a2x + a2y * a2y);
    double cross = a1x * a2y - a1y * a2x;

    if (radius == 0.0 || len1 < 1e-9 || len2 < 1e-9 || fabs(cross) < 1e-9) {
        emit_line(c, fx(x1), fx(y1));
        return JS_UNDEFINED;
    }

    double u1x = a1x / len1, u1y = a1y / len1;
    double u2x = a2x / len2, u2y = a2y / len2;
    double cosw = u1x * u2x + u1y * u2y;
    if (cosw > 1.0) cosw = 1.0; else if (cosw < -1.0) cosw = -1.0;
    double theta = acos(cosw);
    double tangent = radius / tan(theta / 2.0);

    double p1x = x1 + u1x * tangent, p1y = y1 + u1y * tangent;
    double p2x = x1 + u2x * tangent, p2y = y1 + u2y * tangent;

    double bisx = u1x + u2x, bisy = u1y + u2y;
    double bislen = sqrt(bisx * bisx + bisy * bisy);
    double cdist = radius / sin(theta / 2.0);
    double cx = x1 + (bisx / bislen) * cdist;
    double cy = y1 + (bisy / bislen) * cdist;

    double ang0 = atan2(p1y - cy, p1x - cx);
    double ang1 = atan2(p2y - cy, p2x - cx);
    /* Same sign convention cv_arc's own `ccw` argument uses: gfx_cos/gfx_sin
     * put angle 0 on +x and increase CLOCKWISE in this y-down space, which is
     * why the flip here is `cross < 0` rather than `cross > 0` -- checked
     * empirically against the corpus, not asserted from first principles. */
    int ccw = cross < 0;

    emit_line(c, fx(p1x), fx(p1y));
    arc_emit(c, fx(cx), fx(cy), fx(radius), fx(radius),
             (int)(ang0 * 14667.719), (int)(ang1 * 14667.719), ccw);
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
    /* The spec's rect() does not leave the path with NO current point: it
     * closes the four-point subpath and then starts a FRESH one whose only
     * point is (x, y) -- so a lineTo() right after rect() draws a real
     * segment from (x, y), it does not silently begin a disconnected
     * subpath. `has_pt = 0` here was exactly that silent-disconnect bug, and
     * 2d.path.rect.closed.html / .end.*.html are the corpus's own case for
     * it: `ctx.rect(...); ctx.lineTo(...); ctx.stroke()` and check that the
     * stroke actually reaches across. emit_move both records (x, y) as
     * startx/curx/cury AND opens the new subpath in the path builder, which
     * is exactly what the spec's second step asks for. */
    emit_move(c, x, y);
    return JS_UNDEFINED;
}

/* One radius value out of `radii`: a plain number (circular) or a
 * DOMPointInit-shaped {x, y} (elliptical). Returns 1 on success, 0 when the
 * value is not finite (the caller's cue to abort silently, same as an
 * out-of-range x/y/w/h), -1 when it is negative (the caller's cue to throw --
 * a RangeError here, NOT arcTo's DOMException; roundRect and arcTo disagree
 * about that in the spec itself), -2 when a JS exception (a TypeError) is
 * already pending and the caller must propagate it rather than invent its
 * own.
 *
 * THE BIGINT CASE IS WHY -2 EXISTS. WebIDL's `unrestricted double` throws a
 * TypeError converting a BigInt -- 2d.path.roundrect.badinput.html asserts it
 * for `0n` directly and for `{ x: 0n }`. This build's JS_ToFloat64 does NOT
 * enforce that (quickjs.c's __JS_ToFloat64Free happily widens a BigInt with
 * bf_get_float64, matching asm.js/wasm intuition rather than WebIDL), so the
 * check has to happen HERE, before ToFloat64 ever runs, or a BigInt radius
 * would silently become a plain number instead of throwing. */
static int corner_radius(JSContext *ctx, JSValueConst v, double *rx, double *ry)
{
    if (JS_IsObject(v) && !JS_IsNumber(v)) {
        JSValue xv = JS_GetPropertyStr(ctx, v, "x");
        JSValue yv = JS_GetPropertyStr(ctx, v, "y");
        if (JS_IsBigInt(ctx, xv) || JS_IsBigInt(ctx, yv)) {
            JS_FreeValue(ctx, xv); JS_FreeValue(ctx, yv);
            JS_ThrowTypeError(ctx, "roundRect: a BigInt radius cannot convert to a double");
            return -2;
        }
        double dx = 0, dy = 0;
        int bad = JS_ToFloat64(ctx, &dx, xv) || JS_ToFloat64(ctx, &dy, yv);
        JS_FreeValue(ctx, xv); JS_FreeValue(ctx, yv);
        /* isfinite, not `!= self` -- see cv_arcTo's identical comment: an
         * infinite radius must abort SILENTLY (nonfinite.html), and
         * `!= self` alone lets -Infinity slip past into the sign check below
         * and throw instead. */
        if (bad || !isfinite(dx) || !isfinite(dy)) return 0;
        if (dx < 0 || dy < 0) return -1;
        *rx = dx; *ry = dy;
        return 1;
    }
    if (JS_IsBigInt(ctx, v)) {
        JS_ThrowTypeError(ctx, "roundRect: a BigInt radius cannot convert to a double");
        return -2;
    }
    double d;
    if (JS_ToFloat64(ctx, &d, v) || !isfinite(d)) return 0;
    if (d < 0) return -1;
    *rx = *ry = d;
    return 1;
}

/* roundRect draws using the SAME arc machinery arc()/ellipse() use --
 * corner by corner, each a quarter-turn ellipse arc -- rather than reaching
 * for gfx_path_rrect4, which builds directly in the path's own coordinate
 * space and so cannot go through this file's per-point CTM transform (see the
 * file header on why the path's matrix has to stay identity here). Winding is
 * clockwise starting at the top-left corner's end, which is what every other
 * browser's roundRect produces and what a page relying on fill-rule with a
 * second, oppositely-wound subpath would need to match. */
static JSValue cv_roundRect(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS;
    double x, y, w, h;
    if (argc < 4 || JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &y, argv[1]) ||
        JS_ToFloat64(ctx, &w, argv[2]) || JS_ToFloat64(ctx, &h, argv[3]))
        return JS_UNDEFINED;
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h)) return JS_UNDEFINED;

    double tlrx = 0, tlry = 0, trrx = 0, trry = 0;
    double brrx = 0, brry = 0, blrx = 0, blry = 0;

    if (argc > 4 && !JS_IsUndefined(argv[4])) {
        JSValueConst rv = argv[4];
        if (JS_IsArray(ctx, rv)) {
            JSValue lenv = JS_GetPropertyStr(ctx, rv, "length");
            int32_t len = 0; JS_ToInt32(ctx, &len, lenv); JS_FreeValue(ctx, lenv);
            if (len < 1 || len > 4)
                return JS_ThrowRangeError(ctx, "roundRect: radii must have 1 to 4 entries");
            double rxs[4], rys[4];
            for (int i = 0; i < len; i++) {
                JSValue el = JS_GetPropertyUint32(ctx, rv, (uint32_t)i);
                int r = corner_radius(ctx, el, &rxs[i], &rys[i]);
                JS_FreeValue(ctx, el);
                if (r == -2) return JS_EXCEPTION;   /* already thrown, e.g. a BigInt */
                if (r < 0) return JS_ThrowRangeError(ctx, "roundRect: radius must not be negative");
                if (r == 0) return JS_UNDEFINED;
            }
            /* CSS border-radius corner expansion, top-left/top-right/
             * bottom-right/bottom-left order -- the same rule 1/2/3/4-value
             * `border-radius` shorthand uses. */
            static const int MAP[4][4] = { {0,0,0,0}, {0,1,0,1}, {0,1,2,1}, {0,1,2,3} };
            const int *m = MAP[len - 1];
            tlrx = rxs[m[0]]; tlry = rys[m[0]];
            trrx = rxs[m[1]]; trry = rys[m[1]];
            brrx = rxs[m[2]]; brry = rys[m[2]];
            blrx = rxs[m[3]]; blry = rys[m[3]];
        } else {
            double rx, ry;
            int r = corner_radius(ctx, rv, &rx, &ry);
            if (r == -2) return JS_EXCEPTION;   /* already thrown, e.g. a BigInt */
            if (r < 0) return JS_ThrowRangeError(ctx, "roundRect: radius must not be negative");
            if (r == 0) return JS_UNDEFINED;
            tlrx = trrx = brrx = blrx = rx;
            tlry = trry = brry = blry = ry;
        }
    }

    /* The CSS corner-overlap correction: if two adjacent corners' radii on one
     * edge would together exceed that edge's length, every radius is scaled
     * down by the same factor -- never just the offending pair -- so opposite
     * corners stay proportionate to each other. */
    double aw = fabs(w), ah = fabs(h);
    double sums[4] = { tlrx + trrx, blrx + brrx, tlry + blry, trry + brry };
    double lens[4] = { aw, aw, ah, ah };
    double scale = 1.0;
    for (int i = 0; i < 4; i++)
        if (sums[i] > 0 && lens[i] > 0) {
            double s = lens[i] / sums[i];
            if (s < scale) scale = s;
        }
    if (scale < 1.0) {
        tlrx *= scale; tlry *= scale; trrx *= scale; trry *= scale;
        brrx *= scale; brry *= scale; blrx *= scale; blry *= scale;
    }

    int fxx = fx(x), fxy = fx(y), fxw = fx(w), fxh = fx(h);
    int itlrx = fx(tlrx), itlry = fx(tlry), itrrx = fx(trrx), itrry = fx(trry);
    int ibrrx = fx(brrx), ibrry = fx(brry), iblrx = fx(blrx), iblry = fx(blry);

    emit_move(c, fxx + itlrx, fxy);
    emit_line(c, fxx + fxw - itrrx, fxy);
    arc_emit(c, fxx + fxw - itrrx, fxy + itrry, itrrx, itrry, 270 * 256, 360 * 256, 0);
    emit_line(c, fxx + fxw, fxy + fxh - ibrry);
    arc_emit(c, fxx + fxw - ibrrx, fxy + fxh - ibrry, ibrrx, ibrry, 0, 90 * 256, 0);
    emit_line(c, fxx + iblrx, fxy + fxh);
    arc_emit(c, fxx + iblrx, fxy + fxh - iblry, iblrx, iblry, 90 * 256, 180 * 256, 0);
    emit_line(c, fxx, fxy + itlry);
    arc_emit(c, fxx + itlrx, fxy + itlry, itlrx, itlry, 180 * 256, 270 * 256, 0);
    gfx_close(&c->path);
    /* Same rule as rect() (see its comment): closing does not leave the path
     * with no current point -- a fresh subpath starts at the corner the loop
     * began from, so a lineTo() right after roundRect() draws a real
     * connecting segment. 2d.path.roundrect.end.*.html is the corpus's case
     * for exactly this. */
    emit_move(c, fxx + itlrx, fxy);
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
    if (!isfinite(a0) || !isfinite(a1)) return JS_UNDEFINED;
    if (r < 0) return throw_dom(ctx, "IndexSizeError", "arc: radius must not be negative");
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
    if (!isfinite(rot) || !isfinite(a0) || !isfinite(a1)) return JS_UNDEFINED;
    if (rx < 0 || ry < 0) return throw_dom(ctx, "IndexSizeError", "ellipse: radius must not be negative");
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

/* clip() intersects the current path with whatever clip was already active --
 * never replaces it -- which is why the corpus can call clip() twice and get
 * the AND of both shapes. The intersection happens once, here, into a fresh
 * device-sized buffer; do_fill/do_stroke read the result through
 * gfx_fill_clipped without knowing how many clip() calls produced it. */
static JSValue cv_clip(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS;
    if (!c->px) return JS_UNDEFINED;
    int rule = GFX_NONZERO;
    if (argc > 0 && JS_IsString(argv[0])) {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (s && !strcmp(s, "evenodd")) rule = GFX_EVENODD;
        if (s) JS_FreeCString(ctx, s);
    }
    if (!path_ok(c, &c->path, "clip")) return JS_UNDEFINED;
    size_t n = (size_t)c->w * c->h;
    unsigned char *cov = (unsigned char *)malloc(n);
    if (!cov) return JS_UNDEFINED;
    memset(cov, 0, n);
    gfx_fill_mask(&c->path, rule, cov, c->w, c->h, 0, 0);
    if (c->st.clip) {
        const unsigned char *old = c->st.clip->cov;
        for (size_t i = 0; i < n; i++)
            cov[i] = (unsigned char)(((int)cov[i] * old[i] + 127) / 255);
    }
    struct cv_clip *nc = (struct cv_clip *)malloc(sizeof *nc);
    if (!nc) { free(cov); return JS_UNDEFINED; }
    nc->refs = 1; nc->cov = cov;
    if (c->st.clip) clip_unref(c->st.clip);
    c->st.clip = nc;
    return JS_UNDEFINED;
}

/* isPointInPath/isPointInStroke ignore the current clip on purpose -- the
 * spec defines both purely in terms of the path (or its stroke outline), and
 * a page building a hit-test region with clip() active must still be able to
 * ask "is this point on the shape" independent of what is visible. Only the
 * two-or-three-argument (x, y[, fillRule]) form is supported: this build has
 * no Path2D, so there is no object to accept as a first argument, and a page
 * that passes one gets `false` rather than a wrong answer read from the
 * current path under someone else's name.
 *
 * THIS IS NOT A RASTERIZATION, on purpose, and it cost a wrong first attempt
 * to learn why. The path is already stored in DEVICE space (the CTM in force
 * when each point was recorded), and the tested (x, y) has to be compared
 * DIRECTLY against that -- NOT transformed by the CTM in force NOW. The WPT
 * corpus proves it: 2d.path.isPointInPath.transform.1.html does
 * `ctx.translate(50, 0); ctx.rect(0, 0, 20, 20)` and then asserts
 * `isPointInPath(51, 10) === true` -- if 51 were transformed through the
 * still-active translate(50, 0) it would land at 101, outside the rect,
 * which is not what the test wants. Rasterizing onto the canvas surface has
 * a second, independent bug the corpus also names:
 * 2d.path.isPointInPath.outside.html builds a rect entirely above the
 * canvas's y=0 and asserts points there are still testable, so the mask
 * cannot be bounded to [0, c->w) x [0, c->h) either. An analytic
 * point-in-polygon test over the path's own line segments (curves are
 * already flattened to lines when the path was built) has neither problem
 * and needs no allocation. */
static int on_segment(long long px, long long py,
                      long long x0, long long y0, long long x1, long long y1)
{
    long long cross = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
    if (cross != 0) return 0;
    long long dot = (px - x0) * (x1 - x0) + (py - y0) * (y1 - y0);
    if (dot < 0) return 0;
    long long lensq = (x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0);
    return dot <= lensq;
}
static int point_hit(const struct gfx_path *path, int rule, long long qx, long long qy)
{
    if (path->npt == 0 || path->overflow) return 0;
    /* A point exactly ON an edge counts as inside -- asserted directly by
     * 2d.path.isPointInPath.edge.html for all four corners and all four
     * edge midpoints of a plain rect -- so this is checked before, and
     * independently of, the crossing count below. */
    int winding = 0, crossings = 0;
    for (int s = 0; s < path->nsub; s++) {
        int start = path->sub[s];
        int end = (s + 1 < path->nsub) ? path->sub[s + 1] : path->npt;
        int n = end - start;
        if (n < 2) continue;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;      /* implicit close: fill always treats a
                                       * subpath as though its last point
                                       * connects back to its first. */
            long long x0 = path->pt[(start + i) * 2], y0 = path->pt[(start + i) * 2 + 1];
            long long x1 = path->pt[(start + j) * 2], y1 = path->pt[(start + j) * 2 + 1];
            if (on_segment(qx, qy, x0, y0, x1, y1)) return 1;
            if (y0 == y1) continue;
            int dir = y0 < y1 ? 1 : -1;
            long long ylo = dir > 0 ? y0 : y1, yhi = dir > 0 ? y1 : y0;
            if (qy < ylo || qy >= yhi) continue;
            long long xi = x0 + (x1 - x0) * (qy - y0) / (y1 - y0);
            if (xi > qx) { if (rule == GFX_NONZERO) winding += dir; else crossings++; }
        }
    }
    return rule == GFX_NONZERO ? winding != 0 : (crossings & 1);
}
static JSValue cv_isPointInPath(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS;
    double x, y;
    if (argc < 2 || JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &y, argv[1]) ||
        x != x || y != y)
        return JS_NewBool(ctx, 0);
    int rule = GFX_NONZERO;
    if (argc > 2 && JS_IsString(argv[2])) {
        const char *s = JS_ToCString(ctx, argv[2]);
        if (s && !strcmp(s, "evenodd")) rule = GFX_EVENODD;
        if (s) JS_FreeCString(ctx, s);
    }
    return JS_NewBool(ctx, point_hit(&c->path, rule, fx(x), fx(y)));
}
static JSValue cv_isPointInStroke(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    CV_THIS;
    double x, y;
    if (argc < 2 || JS_ToFloat64(ctx, &x, argv[0]) || JS_ToFloat64(ctx, &y, argv[1]) ||
        x != x || y != y)
        return JS_NewBool(ctx, 0);
    struct gfx_path out;
    if (build_stroke_outline(c, &c->path, &out) <= 0) return JS_NewBool(ctx, 0);
    return JS_NewBool(ctx, point_hit(&out, GFX_NONZERO, fx(x), fx(y)));
}

/* reset() is exactly what setting .width/.height to their own value already
 * does -- cv_realloc's own header: the surface, the state stack and the CTM
 * all go, even when nothing about the size changed. */
static JSValue cv_reset(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv; CV_THIS;
    cv_realloc(c, c->w, c->h);
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
    /* Not the generic "zero-area path strokes as nothing" case -- the spec
     * carves this out explicitly for strokeRect specifically: a zero width
     * OR height means the method "has no effect" at all, full stop, not even
     * the caps/joins a round-capped zero-length closed subpath would
     * otherwise draw (2d.strokeRect.zero.2.html builds exactly that
     * temptation: lineCap/lineJoin 'round' at a huge width, which a stroker
     * that does not special-case this draws as a solid disc). fillRect
     * already has this check two functions up; strokeRect needs its own
     * copy because it does not share fillRect's call site. */
    if (w == 0 || h == 0) return JS_UNDEFINED;
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

/* The GLOBAL `ImageData` constructor -- getImageData/createImageData already
 * built the shape (make_imagedata); this publishes the name so `new
 * ImageData(...)` works standalone, which pages that build synthetic pixel
 * buffers before ever touching a canvas rely on. Two WebIDL overloads:
 * (width, height) and (Uint8ClampedArray data, width[, height]) -- which of
 * the two is which is decided by whether the first argument is a number, the
 * same test the spec's overload resolution comes down to here. The
 * (data, ...) form COPIES data's bytes (make_imagedata always allocates a
 * fresh Uint8ClampedArray and memcpy's into it) rather than aliasing the
 * caller's buffer -- matching the spec, which says the new ImageData's data
 * is "initialized to a new ... object ... whose ... bytes are initialized to
 * the ... bytes of data", not data itself. */
static JSValue js_ImageData(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv)
{
    (void)nt;
    if (argc < 1) return JS_ThrowTypeError(ctx, "ImageData: not enough arguments");
    if (JS_IsNumber(argv[0])) {
        int32_t w = 0, h = 0;
        if (argc < 2) return JS_ThrowTypeError(ctx, "ImageData: needs a width and a height");
        if (JS_ToInt32(ctx, &w, argv[0]) || JS_ToInt32(ctx, &h, argv[1]))
            return JS_EXCEPTION;
        if (w <= 0 || h <= 0) return JS_ThrowRangeError(ctx, "ImageData: zero size");
        if ((long long)w * h > CV_MAXPX) return JS_ThrowRangeError(ctx, "ImageData: too large");
        return make_imagedata(ctx, w, h, NULL, 0);
    }

    size_t abytes = 0;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], NULL, NULL, NULL);
    if (JS_IsException(ab))
        return JS_ThrowTypeError(ctx, "ImageData: first argument must be a number or a Uint8ClampedArray");
    uint8_t *p = JS_GetArrayBuffer(ctx, &abytes, ab);
    int32_t w = 0, h = 0;
    if (argc < 2 || JS_ToInt32(ctx, &w, argv[1])) {
        JS_FreeValue(ctx, ab);
        return JS_ThrowTypeError(ctx, "ImageData: needs a width");
    }
    if (w <= 0 || !p || abytes == 0 || abytes % 4 != 0 || (abytes / 4) % (uint32_t)w != 0) {
        JS_FreeValue(ctx, ab);
        return JS_ThrowRangeError(ctx, "ImageData: data length is not a multiple of (4 * width)");
    }
    int32_t rows = (int32_t)(abytes / 4 / (uint32_t)w);
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        if (JS_ToInt32(ctx, &h, argv[2])) { JS_FreeValue(ctx, ab); return JS_EXCEPTION; }
        if (h != rows) {
            JS_FreeValue(ctx, ab);
            return JS_ThrowRangeError(ctx, "ImageData: height does not match the data length");
        }
    } else {
        h = rows;
    }
    JSValue r = make_imagedata(ctx, w, h, p, w * 4);
    JS_FreeValue(ctx, ab);
    return r;
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
    JS_CFUNC_DEF("reset", 0, cv_reset),
    JS_CFUNC_DEF("beginPath", 0, cv_beginPath),
    JS_CFUNC_DEF("closePath", 0, cv_closePath),
    JS_CFUNC_DEF("moveTo", 2, cv_moveTo),
    JS_CFUNC_DEF("lineTo", 2, cv_lineTo),
    JS_CFUNC_DEF("quadraticCurveTo", 4, cv_quadTo),
    JS_CFUNC_DEF("bezierCurveTo", 6, cv_bezierTo),
    JS_CFUNC_DEF("arcTo", 5, cv_arcTo),
    JS_CFUNC_DEF("rect", 4, cv_rectpath),
    JS_CFUNC_DEF("roundRect", 4, cv_roundRect),
    JS_CFUNC_DEF("arc", 5, cv_arc),
    JS_CFUNC_DEF("ellipse", 7, cv_ellipse),
    JS_CFUNC_DEF("fill", 0, cv_fill),
    JS_CFUNC_DEF("stroke", 0, cv_stroke),
    JS_CFUNC_DEF("clip", 0, cv_clip),
    JS_CFUNC_DEF("isPointInPath", 2, cv_isPointInPath),
    JS_CFUNC_DEF("isPointInStroke", 2, cv_isPointInStroke),
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
    /* Every live clip buffer -- the active one and anything on the save stack
     * -- was sized to the OLD w*h and is about to be a stale read past the end
     * of a smaller allocation, or a mask over the wrong geometry for a larger
     * one. Release all of them before nstack goes to 0, not after, so their
     * refcounts actually reach zero here rather than leaking. */
    clip_release_all(c);
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

/* ---------------------------------------------------------------- readback --
 *
 * toDataURL / toBlob, over rust/src/pngenc.rs. The header of this file carries
 * the argument for why these stopped throwing; what is below is the mechanics,
 * and four of them are decisions rather than plumbing.
 *
 * 1. A CANVAS WITH NO CONTEXT IS TRANSPARENT BLACK, NOT AN ERROR. The spec
 *    says the bitmap of a canvas whose context was never obtained is w x h of
 *    transparent black, and a fingerprint probe reaching for toDataURL on a
 *    fresh canvas is a real shape. Throwing there would put us back where we
 *    started for exactly the caller this work is for. Encoding zeroes is not a
 *    fabrication: nothing was drawn, and that IS what was drawn.
 *
 * 2. A ZERO-SIZED CANVAS RETURNS THE STRING "data:,". Also the spec, and it is
 *    the one case where there are no pixels to be honest about.
 *
 * 3. THE MIME TYPE IS ANSWERED, NEVER ASSUMED. `image/png` is the only type
 *    this tree can produce. HTML says a UA that cannot produce the requested
 *    type must use image/png, and the returned URL declares what it is -- so a
 *    caller that asked for image/webp reads `data:image/png;base64,` back and
 *    can see the fallback. That string is the whole difference between an
 *    honest fallback and the lie the old refusal existed to prevent. It is
 *    also announced on the console once per requested type, because a
 *    substitution nobody can see is one step from one nobody can detect.
 *
 * 4. THE BITMAP IS SNAPSHOTTED SYNCHRONOUSLY EVEN IN toBlob. The encode runs
 *    inside the toBlob call and only the CALLBACK is deferred. Deferring the
 *    encode as well would hand the page a picture of whatever the canvas
 *    looked like later, which is the kind of wrong that looks right most of
 *    the time.
 */

/* Base64, RFC 4648, with padding.
 *
 * THE ENCODER IS THE TREE'S, NOT A FOURTH COPY. `b64_encode` is
 * c/net/ssh/base64.c, already in this file's link line by two independent
 * routes: the browser links it for the WebSocket handshake's
 * Sec-WebSocket-Accept (Makefile:903) and tests/canvas.mk links it in
 * CANVAS_WS_SRC (:117). An earlier draft of this function was a private table
 * and a private loop -- a fourth C base64 in a tree that already had three,
 * with the SAME NAME as the one three files away and a different signature.
 * That is rule 3, "one jar, TWO DOORS": two spellings of one constant agree on
 * the wrong value about as often as the right one, and the tree has paid for
 * it three times already.
 *
 * NOT `btoa` (js_platform.c's `==== base64` block), and that is the one alternative genuinely
 * refused rather than merely not chosen: btoa is the LATIN-1 pair, so reaching
 * it means pushing bytes through a JS string and pulling them back out, two
 * conversions that can each be wrong. It is also a global an embedder may not
 * have installed, and this path must not depend on which installers ran.
 *
 * The `pad` argument is a real parameter of the callee, which is why the
 * padding control below is an argument rather than an #ifdef around a literal.
 * b64_encode does not NUL-terminate; its header says so and the caller adds
 * one. */
static char *b64_encode_alloc(const unsigned char *p, size_t n, size_t *outlen)
{
    /* n is bounded by CV_MAXPX*4 plus PNG overhead -- under 17 MiB, so the
     * int the callee takes is not in danger. Checked rather than asserted,
     * because a future caller with a bigger surface would otherwise wrap. */
    if (n > (size_t)0x30000000) return NULL;
    size_t cap = ((n + 2) / 3) * 4;
    char *o = (char *)malloc(cap + 1);
    if (!o) return NULL;

    /* THE PADDING CONTROL (-DCANVAS_B64_NOPAD, tests/canvas.mk).
     *
     * Dropping the '=' is the defect this file's own round-trip CANNOT see,
     * which is exactly why it is the control. `atob` (js_platform.c, the `def(G, 'atob'` shim -- the TEXT is the anchor,
     * not a line number; net.c:249 records why) opens
     * with `if (s.length % 4 === 0) s = s.replace(/==?$/, '')` and then refuses
     * only `length % 4 === 1` -- so unpadded input decodes there perfectly.
     * Every pixel assertion in tests/unit/canvas_test.c goes through atob, so
     * all 68 stay GREEN with this flag on. MEASURED, not predicted: that is
     * what `make test-canvas-b64-negctl` asserts, and it fails if the C suite
     * ever starts catching it, because then this comment would be stale.
     *
     * The two implementations are genuinely independent -- a C table there, a
     * JS shim here -- which makes the round-trip a real differential for the
     * ALPHABET and the bit packing. It is not one for the padding, because a
     * decoder that tolerates its absence agrees with an encoder that omits it.
     * That is the "wrong CRC polynomial both sides compute the same way" shape
     * one layer up from where tests/pngenc.mk found it: the round trip is
     * perfect and every other program on earth is stricter. Python's
     * base64.b64decode(validate=True) is, and so is the data: URL parser in
     * every other browser. Hence tests/unit/canvas_b64_ext_test.py. */
#ifdef CANVAS_B64_NOPAD
    const int pad = 0;
#else
    const int pad = 1;
#endif
    int k = b64_encode((const uint8_t *)p, (int)n, o, (int)cap, pad);
    if (k < 0) { free(o); return NULL; }
    o[k] = 0;
    if (outlen) *outlen = (size_t)k;
    return o;
}

/* Announce a fallback ONCE per requested type. A cap rather than a set,
 * because the list of types a page can name is unbounded and a log line per
 * call on a page that fingerprints in a loop is its own failure. */
static void note_type_fallback(const char *want)
{
    static char seen[6][32];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (!strcmp(seen[i], want)) return;
    if (nseen < 6) {
        size_t n = strlen(want);
        if (n > 31) n = 31;
        memcpy(seen[nseen], want, n); seen[nseen][n] = 0;
        nseen++;
    }
    printf("[canvas] toDataURL/toBlob: '%s' is not a type this browser can "
           "encode; returning image/png, which the returned type declares. "
           "(PNG is the only encoder here -- c/lib/image/img.h)\n", want);
}

/* The canvas element's bitmap, as PNG bytes. Returns 0 and sets *len to 0 for
 * a zero-sized canvas (the caller turns that into "data:,"), or on OOM/encoder
 * refusal -- distinguished by *why, which is never NULL on failure. Free the
 * result with png_encode_free. */
static unsigned char *el_png(JSContext *ctx, JSValueConst t, int *len, const char **why)
{
    *len = 0; *why = NULL;
    struct node *n = js_dom_node_from(t);
    int w = n ? attr_int(n, "width", 300) : 300;
    int h = n ? attr_int(n, "height", 150) : 150;
    if (w <= 0 || h <= 0) return NULL;          /* "data:," -- not a failure */
    if ((long long)w * h > CV_MAXPX) {
        *why = "the canvas is larger than this build's backing-store limit";
        return NULL;
    }

    JSValue have = JS_GetPropertyStr(ctx, t, "__ctx2d");
    struct canvas2d *c = cv_of(have);
    unsigned char *px = NULL;
    int owned = 0;
#ifdef CANVAS_READBACK_BLANK
    /* THE READBACK'S NEGATIVE CONTROL (tests/canvas.mk). The backing store is
     * ignored and a correctly-sized, correctly-structured, WELL-FORMED PNG of
     * transparent black is encoded instead.
     *
     * It is this defect and not another because it is the one the old refusal
     * was written against: a valid-looking data URL of the WRONG pixels is
     * worse than a throw, because the throw is detectable and the wrong pixels
     * are not. Every check that inspects the URL's prefix, the signature, the
     * chunk names, the IHDR fields or the filter byte STILL PASSES with this
     * on -- so if the gate stays green here, its readback assertions are
     * measuring the shape of a PNG and not the picture in one, and the whole
     * argument for un-refusing toDataURL is unsupported by its own test. */
    c = NULL;
#endif
    if (c && c->px && c->w == w && c->h == h) {
        px = c->px;
    } else {
        /* No context, or a context whose surface has not caught up with the
         * attributes: transparent black at the attribute size. This is the
         * spec's answer AND the honest one -- nothing was drawn. */
        px = (unsigned char *)calloc((size_t)w * h, 4);
        owned = 1;
        if (!px) { JS_FreeValue(ctx, have); *why = "out of memory"; return NULL; }
    }
    int outn = 0;
    unsigned char *png = png_encode_rgba(px, w, h, &outn);
    if (owned) free(px);
    JS_FreeValue(ctx, have);
    if (!png || outn <= 0) { *why = "the PNG encoder refused this bitmap"; return NULL; }
    *len = outn;
    return png;
}

/* Read the optional `type` argument, lowercased into buf. Returns 1 if the
 * caller asked for something other than image/png (so the fallback is worth
 * announcing). An absent or non-string type is image/png by default, which is
 * the spec's default and needs no note. */
static int wanted_type(JSContext *ctx, int argc, JSValueConst *argv, char *buf, size_t cap)
{
    buf[0] = 0;
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) return 0;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return 0;
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++)
        buf[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
    buf[i] = 0;
    JS_FreeCString(ctx, s);
    if (!buf[0] || !strcmp(buf, "image/png")) return 0;
    return 1;
}

/* -DCANVAS_READBACK_REFUSE -- THE BEFORE PICTURE, AS A BUILD FLAG.
 *
 * This restores the behaviour that shipped until 2026-08-29 exactly: both
 * readback entry points throw, with the message they threw. It is NOT a
 * negative control on the encoder (CANVAS_READBACK_BLANK is that one, and it
 * tests something else -- that the gate reads the picture rather than the shape
 * of a file). It exists because of a claim this file's header makes and could
 * not previously support with a measurement:
 *
 *     "the challenge script died on its FIRST STATEMENT and the widget never
 *      appeared" -- and, downstream of it, "the page now gets further".
 *
 * "Further" is a comparison, and until this flag existed there was no second
 * term: every run was an AFTER run, and the BEFORE half was inherited from the
 * sentence that motivated the work. CLAUDE.md's rule 1 is precisely about that
 * shape -- the measurement is right and the sentence around it sends the reader
 * somewhere else. With this flag, the same page is loaded twice on the same
 * disk image with one #define between them, and the difference is observed
 * rather than argued.
 *
 * It is deliberately a whole-binary flag rather than a runtime switch: a
 * runtime toggle would need a way to be set, and every such way is a signal a
 * page could read. Nothing here is tuned toward any checker, and a build that
 * can be asked at runtime to lie about its own capabilities is one step from
 * one that does.
 */
static JSValue el_toDataURL(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
#ifdef CANVAS_READBACK_REFUSE
    (void)t; (void)argc; (void)argv;
    return JS_ThrowInternalError(ctx, "canvas.toDataURL: this browser decodes "
                                 "PNG and does not encode it");
#else
    char want[64];
    if (wanted_type(ctx, argc, argv, want, sizeof want)) note_type_fallback(want);

    int n = 0; const char *why = NULL;
    unsigned char *png = el_png(ctx, t, &n, &why);
    if (!png) {
        /* A zero-sized canvas is not an error and the spec names its answer. */
        if (!why) return JS_NewString(ctx, "data:,");
        return JS_ThrowInternalError(ctx, "canvas.toDataURL: %s", why);
    }
    size_t blen = 0;
    char *b64 = b64_encode_alloc(png, (size_t)n, &blen);
    png_encode_free(png);
    if (!b64) return JS_ThrowOutOfMemory(ctx);

    static const char PFX[] = "data:image/png;base64,";
    char *url = (char *)malloc(sizeof PFX - 1 + blen + 1);
    if (!url) { free(b64); return JS_ThrowOutOfMemory(ctx); }
    memcpy(url, PFX, sizeof PFX - 1);
    memcpy(url + sizeof PFX - 1, b64, blen + 1);
    free(b64);
    JSValue r = JS_NewStringLen(ctx, url, sizeof PFX - 1 + blen);
    free(url);
    return r;
#endif
}

/* toBlob's deferred half. argv[0] is the callback, argv[1] the Uint8Array of
 * PNG bytes (or undefined when there were none to produce).
 *
 * WHY A JOB AND NOT A DIRECT CALL, AND WHY A JOB AND NOT setTimeout. toBlob is
 * asynchronous in every browser, and a page that writes
 * `canvas.toBlob(cb); next();` sees next() first everywhere else -- calling cb
 * inline would make this the one asynchronous API in this browser that is not.
 * The queue used is QuickJS's job queue, i.e. the SAME one every promise
 * reaction in this engine settles on, which is the strongest available
 * statement of "as async as a promise here and no more".
 *
 * AND THE COST IS NAMED, because it is real and it is not obvious: an embedder
 * that never drains that queue never runs this callback. That is not
 * hypothetical -- js_dom_iface.inc:1564 records the WPT runner calling JS_Eval
 * directly and js_dom_run_jobs never being reached, which cost 500-odd
 * subtests before anybody noticed. A toBlob callback in such an embedder is
 * exactly as dead as `Promise.resolve().then(cb)` is there, which is the point:
 * it fails the same way as the thing it is scheduled beside, rather than in a
 * new way. tests/unit/canvas_test.c therefore asserts BOTH halves -- that the
 * callback has NOT run when toBlob returns, and that it HAS after a pump. */
static JSValue toblob_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 2) return JS_UNDEFINED;
    JSValue blob = JS_NULL;
    if (!JS_IsUndefined(argv[1])) {
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue ctor = JS_GetPropertyStr(ctx, g, "Blob");
        JS_FreeValue(ctx, g);
        if (JS_IsFunction(ctx, ctor)) {
            JSValue parts = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, parts, 0, JS_DupValue(ctx, argv[1]));
            JSValue opts = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, opts, "type", JS_NewString(ctx, "image/png"));
            JSValueConst a[2] = { parts, opts };
            blob = JS_CallConstructor(ctx, ctor, 2, a);
            JS_FreeValue(ctx, parts);
            JS_FreeValue(ctx, opts);
            if (JS_IsException(blob)) { JS_FreeValue(ctx, blob); blob = JS_NULL; }
        } else {
            /* Named, not silent: the spec's failure answer for toBlob is
             * null, and a page that gets null with no explanation on the
             * console has no way to tell "the encoder failed" from "this
             * build has no Blob constructor". */
            printf("[canvas] toBlob: the global Blob constructor is absent in this "
                   "build (js_platform.c installs it), so the callback gets null\n");
        }
        JS_FreeValue(ctx, ctor);
    }
    JSValueConst cb_args[1] = { blob };
    JSValue r = JS_Call(ctx, argv[0], JS_UNDEFINED, 1, cb_args);
    JS_FreeValue(ctx, blob);
    if (JS_IsException(r)) return r;
    JS_FreeValue(ctx, r);
    return JS_UNDEFINED;
}

static JSValue el_toBlob(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
#ifdef CANVAS_READBACK_REFUSE
    /* See el_toDataURL. BOTH halves are restored, because a page that finds
     * toDataURL throwing and toBlob working would be a machine that never
     * existed, and a before-picture that never existed cannot be compared to
     * anything. */
    (void)t; (void)argc; (void)argv;
    return JS_ThrowInternalError(ctx, "canvas.toBlob: this browser decodes PNG "
                                 "and does not encode it");
#else
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "toBlob: the first argument must be a function");
    char want[64];
    if (wanted_type(ctx, argc - 1, argv + 1, want, sizeof want)) note_type_fallback(want);

    int n = 0; const char *why = NULL;
    unsigned char *png = el_png(ctx, t, &n, &why);
    JSValue bytes = JS_UNDEFINED;
    if (png) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, png, (size_t)n);
        png_encode_free(png);
        if (!JS_IsException(ab)) {
            JSValue g = JS_GetGlobalObject(ctx);
            JSValue u8 = JS_GetPropertyStr(ctx, g, "Uint8Array");
            JS_FreeValue(ctx, g);
            if (JS_IsFunction(ctx, u8)) {
                JSValueConst a[1] = { ab };
                bytes = JS_CallConstructor(ctx, u8, 1, a);
                if (JS_IsException(bytes)) { JS_FreeValue(ctx, bytes); bytes = JS_UNDEFINED; }
            }
            JS_FreeValue(ctx, u8);
        }
        JS_FreeValue(ctx, ab);
    } else if (why) {
        printf("[canvas] toBlob: %s; the callback will get null\n", why);
    }

    JSValueConst job[2] = { argv[0], bytes };
    int r = JS_EnqueueJob(ctx, toblob_job, 2, job);
    JS_FreeValue(ctx, bytes);
    if (r < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
#endif
}

static const JSCFunctionListEntry canvas_el_funcs[] = {
    JS_CGETSET_DEF("width", el_get_w, el_set_w),
    JS_CGETSET_DEF("height", el_get_h, el_set_h),
    JS_CFUNC_DEF("getContext", 1, el_getContext),
    JS_CFUNC_DEF("toDataURL", 0, el_toDataURL),
    /* Its own function now. It used to be an ALIAS for toDataURL, which was
     * harmless only because both threw: toBlob's first argument is a callback
     * and its result arrives through that callback, so the two share no
     * signature and no return value. */
    JS_CFUNC_DEF("toBlob", 1, el_toBlob),
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
    /* The global ImageData constructor -- a plain function, like js_cssom.c's
     * DOMRect, not a JSClass with its own prototype: getImageData/
     * createImageData/new ImageData() all hand back the same ordinary-object
     * shape (a "data"/"width"/"height" own-property bag) rather than an
     * instance of a class this constructor introduces, so `instanceof` is not
     * honoured here. What every failure this closes actually reads is
     * `'ImageData' is not defined` or a wrong width/height/data -- not an
     * instanceof check -- so that is the bar this meets. */
    JS_SetPropertyStr(ctx, g, "ImageData",
                      JS_NewCFunction2(ctx, js_ImageData, "ImageData", 2,
                                       JS_CFUNC_constructor, 0));

    JS_FreeValue(ctx, ce);
    JS_FreeValue(ctx, g);
}
