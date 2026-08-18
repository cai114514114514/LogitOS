/* canvas_test.c -- CanvasRenderingContext2D, checked by reading the pixels back.
 *
 * WHY THIS SHAPE. A gfx_surface is STRAIGHT (non-premultiplied) RGBA8 and so is
 * ImageData, so getImageData is a copy rather than a conversion: what these
 * assertions read is literally what the engine composited, with no stage in
 * between that could be wrong in a compensating direction. That is the whole
 * reason the gate can be "assert the byte" instead of "assert it did not
 * throw", and it is why every check below names a pixel.
 *
 * WHAT IS NOT MEASURED HERE, deliberately. The rasterizer's ACCURACY is not
 * this file's subject -- c/lib/gfx already has two independent oracles for
 * that (tests/unit/gfx_raster_test.c against a 16x16 supersampled analytic
 * predicate, tests/unit/aui_mask_test.c against a separately written
 * reference), and re-deriving coverage here would be a third reference that
 * can only disagree with them. What IS this file's subject is the PLUMBING
 * between a page's JS and that engine: whether a coordinate arrives where the
 * page put it, whether the CTM composes in the order the spec says, whether
 * fillStyle means the colour it names, whether alpha composites, whether
 * clearRect removes and putImageData replaces.
 *
 * So the assertions sit on pixel CENTRES of axis-aligned rectangles, where the
 * true coverage is exactly 0 or exactly 255 and no antialiasing tolerance is
 * involved. A wrong colour, a transposed axis, an off-by-one origin or a CTM
 * composed in the wrong order all move a centre sample; a half-percent
 * coverage difference does not, and is not this gate's business.
 *
 *   make test-canvas            this file
 *   make test-canvas-negctl     -DCANVAS_IGNORE_CTM, which drops the CTM from
 *                               point transformation -- the single most
 *                               plausible wrong implementation, and one that
 *                               draws a perfectly good picture in the wrong
 *                               place. It must FAIL here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }

static int checks, failed;

static void fail(const char *what, const char *detail)
{ failed++; printf("FAIL: %s\n      %s\n", what, detail ? detail : ""); }

static JSContext *G;

/* Evaluate `src` and return its completion value as a C string the caller
 * frees with free(). NULL if it threw -- the message is printed. */
static char *evals(const char *src)
{
    JSValue v = JS_Eval(G, src, strlen(src), "<canvas-test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(G);
        const char *m = JS_ToCString(G, e);
        printf("      threw: %s\n", m ? m : "?");
        if (m) JS_FreeCString(G, m);
        JS_FreeValue(G, e);
        JS_FreeValue(G, v);
        return NULL;
    }
    const char *s = JS_ToCString(G, v);
    char *out = s ? strdup(s) : NULL;
    if (s) JS_FreeCString(G, s);
    JS_FreeValue(G, v);
    return out;
}

static void eq(const char *label, const char *src, const char *want)
{
    checks++;
    char *got = evals(src);
    if (!got) { fail(label, "(threw)"); return; }
    if (strcmp(got, want)) {
        char buf[512];
        snprintf(buf, sizeof buf, "want \"%s\", got \"%s\"", want, got);
        fail(label, buf);
    } else printf("ok  : %s\n", label);
    free(got);
}

static void run(const char *src)
{
    char *s = evals(src);
    free(s);
}

/* The page every check runs against. One <canvas>, sized so a check can name a
 * pixel without arithmetic. */
static const char *PAGE =
    "<!doctype html><html><body><canvas id=c width=40 height=20></canvas></body></html>";

int main(void)
{
    struct node *root = dom_parse(PAGE, (int)strlen(PAGE));
    if (!root) { printf("canvas_test: the fixture page did not parse\n"); return 1; }
    if (!js_page_open(root)) { printf("canvas_test: js_page_open failed\n"); return 1; }
    G = js_page_ctx();

    /* ---- the context exists at all, and only on a canvas ------------------ */
    eq("getContext('2d') returns a context",
       "typeof document.getElementById('c').getContext('2d')", "object");
    eq("the same call returns the SAME object",
       "(function(){ var e=document.getElementById('c');"
       "  return e.getContext('2d') === e.getContext('2d'); })()", "true");
    eq("getContext('webgl') is null, not a throw",
       "String(document.getElementById('c').getContext('webgl'))", "null");
    /* The reason this file exists rather than a one-line `return null`: the
     * method must not appear on elements that are not canvases, or a probe
     * that tests `!!el.getContext` on the wrong element gets a wrong answer.
     * HTMLCanvasElement.prototype being a real link in the chain is what makes
     * that true; asserted here so a future move to Element.prototype cannot
     * pass unnoticed. */
    eq("a <div> has no getContext",
       "typeof document.createElement('div').getContext", "undefined");

    run("var e = document.getElementById('c'); var g = e.getContext('2d');"
        "function px(x,y){ var d = g.getImageData(x,y,1,1).data;"
        "  return d[0]+','+d[1]+','+d[2]+','+d[3]; }");

    /* ---- a fresh canvas is transparent black ------------------------------ */
    eq("a new canvas is transparent black", "px(0,0)", "0,0,0,0");

    /* ---- fillRect puts the named colour where the page said ---------------- */
    run("g.fillStyle = '#ff0000'; g.fillRect(2,3,4,5);");
    eq("fillRect paints inside",            "px(3,4)", "255,0,0,255");
    eq("fillRect's left edge is exclusive-1","px(1,4)", "0,0,0,0");
    eq("fillRect's right edge is exclusive", "px(6,4)", "0,0,0,0");
    eq("fillRect's top edge is exclusive-1", "px(3,2)", "0,0,0,0");
    eq("fillRect's bottom edge is exclusive","px(3,8)", "0,0,0,0");
    /* x and y are not interchangeable, and a transposed pair still paints a
     * rectangle -- which is why this asks a NON-SQUARE one about a pixel that
     * is inside one orientation and outside the other. */
    eq("the rect is not transposed",         "px(5,7)", "255,0,0,255");

    /* ---- fillStyle parses the colour syntaxes a page actually writes ------- */
    run("g.fillStyle='#0f0'; g.fillRect(10,0,2,2);");
    eq("#rgb shorthand",   "px(10,0)", "0,255,0,255");
    run("g.fillStyle='rgb(0,0,255)'; g.fillRect(12,0,2,2);");
    eq("rgb()",            "px(12,0)", "0,0,255,255");
    run("g.fillStyle='blue'; g.fillRect(14,0,2,2);");
    eq("a named colour",   "px(14,0)", "0,0,255,255");
    /* An unparseable value is IGNORED by the spec, keeping the previous one.
     * Going black instead is how a chart loses its series colours with nothing
     * to find, so the wrong behaviour here is silent and worth pinning. */
    run("g.fillStyle='not-a-colour'; g.fillRect(16,0,2,2);");
    eq("an unparseable fillStyle keeps the previous colour", "px(16,0)", "0,0,255,255");
    eq("fillStyle serializes back as #rrggbb",
       "(function(){ g.fillStyle = '#123456'; return g.fillStyle; })()", "#123456");
    /* The property that matters is that the serialization ROUND TRIPS through
     * the parser -- `ctx.fillStyle = ctx.fillStyle` is a real idiom, and an
     * output the input side cannot read loses the colour silently. Asserted as
     * a round trip rather than as a string measured from another browser,
     * because that is the property, and because a string nobody here measured
     * would be a remembered expectation dressed up as a reference. */
    eq("a translucent fillStyle serializes as rgba() with spaces",
       "(function(){ g.fillStyle = 'rgba(1,2,3,0.5)'; return g.fillStyle; })()",
       "rgba(1, 2, 3, 0.502)");
    eq("the serialization round-trips through the parser",
       "(function(){ g.fillStyle = 'rgba(10,20,30,0.25)'; var a = g.fillStyle;"
       "  g.fillStyle = a; return g.fillStyle === a; })()", "true");
    eq("an opaque colour round-trips too",
       "(function(){ g.fillStyle = '#0a141e'; var a = g.fillStyle;"
       "  g.fillStyle = a; return g.fillStyle === a && a === '#0a141e'; })()", "true");

    /* ---- globalAlpha composites rather than being remembered -------------- */
    run("g.fillStyle='#000000'; g.clearRect(0,0,40,20);"
        "g.fillStyle='#ffffff'; g.fillRect(0,0,4,4);"
        "g.globalAlpha=0.5; g.fillStyle='#000000'; g.fillRect(0,0,4,4);"
        "g.globalAlpha=1;");
    /* Black at 50% over opaque white: the destination stays opaque and the
     * channels land mid-range. The exact value is the engine's rounding, so
     * this asserts the ALPHA is still 255 and the colour actually moved --
     * a globalAlpha that was stored and ignored leaves 0,0,0,255. */
    eq("globalAlpha is applied, not merely stored",
       "(function(){ var d = g.getImageData(1,1,1,1).data;"
       "  return (d[3] === 255) && (d[0] > 100 && d[0] < 160); })()", "true");

    /* ---- the CTM -- and this is what the negative control removes ---------- */
    run("g.globalAlpha=1; g.clearRect(0,0,40,20);"
        "g.save(); g.translate(20,10); g.fillStyle='#ff00ff'; g.fillRect(0,0,3,3); g.restore();");
    eq("translate moves the shape",        "px(21,11)", "255,0,255,255");
    eq("translate really moved it",        "px(1,1)",   "0,0,0,0");
    eq("restore undoes the translate",
       "(function(){ g.fillStyle='#00ffff'; g.fillRect(0,0,2,2); return px(0,0); })()",
       "0,255,255,255");

    run("g.clearRect(0,0,40,20); g.save(); g.scale(2,2);"
        "g.fillStyle='#ffff00'; g.fillRect(1,1,2,2); g.restore();");
    eq("scale scales the geometry",        "px(3,3)", "255,255,0,255");
    eq("scale is not a no-op",             "px(1,1)", "0,0,0,0");

    /* Composition ORDER. translate-then-scale and scale-then-translate put the
     * same rect in different places, and an implementation that multiplies on
     * the wrong side draws a perfectly plausible picture in the wrong one. */
    run("g.clearRect(0,0,40,20); g.save(); g.translate(10,4); g.scale(2,2);"
        "g.fillStyle='#ff8800'; g.fillRect(1,1,1,1); g.restore();");
    eq("translate then scale composes in that order", "px(12,6)", "255,136,0,255");
    eq("and not in the other order",                  "px(21,9)", "0,0,0,0");

    /* ---- paths go through the same fill as fillRect ------------------------ */
    run("g.clearRect(0,0,40,20); g.beginPath(); g.moveTo(2,2); g.lineTo(8,2);"
        "g.lineTo(8,8); g.lineTo(2,8); g.closePath(); g.fillStyle='#00ff00'; g.fill();");
    eq("a closed path fills",              "px(5,5)", "0,255,0,255");
    eq("outside the path is untouched",    "px(9,5)", "0,0,0,0");

    /* fillRect must not disturb the page's current path -- the spec is explicit
     * and a shared path object is the obvious wrong implementation. */
    run("g.clearRect(0,0,40,20); g.beginPath(); g.rect(2,2,6,6);"
        "g.fillStyle='#ff0000'; g.fillRect(20,2,4,4);"
        "g.fillStyle='#0000ff'; g.fill();");
    eq("fillRect leaves the current path alone", "px(5,5)", "0,0,255,255");
    eq("and painted its own rectangle too",      "px(21,3)", "255,0,0,255");

    /* ---- clearRect removes, and only where it says ------------------------- */
    run("g.clearRect(0,0,40,20); g.fillStyle='#ffffff'; g.fillRect(0,0,10,10);"
        "g.clearRect(2,2,3,3);");
    eq("clearRect clears",                 "px(3,3)", "0,0,0,0");
    eq("clearRect leaves its neighbour",   "px(6,3)", "255,255,255,255");

    /* ---- ImageData in both directions -------------------------------------- */
    eq("createImageData is transparent black and the right size",
       "(function(){ var d = g.createImageData(3,2);"
       "  return d.width + ',' + d.height + ',' + d.data.length + ',' + d.data[0]; })()",
       "3,2,24,0");
    eq("putImageData replaces pixels and ignores globalAlpha",
       "(function(){ g.clearRect(0,0,40,20);"
       "  var d = g.createImageData(2,2);"
       "  for (var i = 0; i < 16; i += 4) { d.data[i]=10; d.data[i+1]=20;"
       "    d.data[i+2]=30; d.data[i+3]=255; }"
       "  g.globalAlpha = 0.25; g.putImageData(d, 5, 5); g.globalAlpha = 1;"
       "  return px(5,5) + '|' + px(6,6) + '|' + px(7,5); })()",
       "10,20,30,255|10,20,30,255|0,0,0,0");
    /* Outside the canvas the spec returns transparent black rather than
     * clamping the rect, so the returned ImageData is always exactly w x h and
     * a caller indexing it cannot walk off the end. */
    eq("getImageData past the edge pads instead of shrinking",
       "(function(){ var d = g.getImageData(38, 0, 4, 1);"
       "  return d.width + ',' + d.data.length; })()", "4,16");

    /* ---- width/height reset the canvas, which is how every page clears one -- */
    run("g.clearRect(0,0,40,20); g.fillStyle='#ffffff'; g.fillRect(0,0,10,10);");
    eq("setting width wipes the canvas",
       "(function(){ e.width = 40; return px(1,1); })()", "0,0,0,0");
    eq("width reads back through the content attribute",
       "e.getAttribute('width')", "40");
    eq("an absent width reads as the spec's 300",
       "(function(){ var n = document.createElement('canvas'); return n.width; })()", "300");
    eq("an absent height reads as the spec's 150",
       "(function(){ var n = document.createElement('canvas'); return n.height; })()", "150");

    /* ---- gradients -------------------------------------------------------- */
    run("g.clearRect(0,0,40,20);"
        "var lg = g.createLinearGradient(0,0,20,0);"
        "lg.addColorStop(0,'#ff0000'); lg.addColorStop(1,'#0000ff');"
        "g.fillStyle = lg; g.fillRect(0,0,20,4);");
    eq("a linear gradient starts at its first stop",
       "(function(){ var d = g.getImageData(0,1,1,1).data;"
       "  return d[0] > 200 && d[2] < 60; })()", "true");
    eq("and ends at its last",
       "(function(){ var d = g.getImageData(19,1,1,1).data;"
       "  return d[2] > 200 && d[0] < 60; })()", "true");
    eq("and is not flat in between",
       "(function(){ var a = g.getImageData(2,1,1,1).data[0];"
       "  var b = g.getImageData(17,1,1,1).data[0];"
       "  return a - b > 100; })()", "true");
    eq("addColorStop refuses an offset outside 0..1",
       "(function(){ try { lg.addColorStop(2,'#000'); return 'no throw'; }"
       "  catch (e) { return e.constructor.name; } })()", "RangeError");
    eq("addColorStop refuses an unparseable colour",
       "(function(){ try { lg.addColorStop(0.5,'zzz'); return 'no throw'; }"
       "  catch (e) { return e.constructor.name; } })()", "SyntaxError");

    /* ---- what is refused, by name ------------------------------------------ */
    /* A fabricated data URL is believed rather than detected -- it is what
     * every fingerprint and every format-support probe reads -- so this must
     * keep throwing for as long as there is no encoder behind it. */
    eq("toDataURL throws rather than fabricating",
       "(function(){ try { e.toDataURL(); return 'no throw'; }"
       "  catch (x) { return x.constructor.name; } })()", "TypeError");

    js_page_close();
    if (failed) { printf("\ncanvas_test: %d/%d checks FAILED\n", failed, checks); return 1; }
    printf("\ncanvas_test: %d checks pass\n", checks);
    return 0;
}
