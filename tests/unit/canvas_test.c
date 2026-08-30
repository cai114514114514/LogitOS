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

/* ------------------------------------------------- the external oracle's tap --
 *
 * WHY THIS EXISTS, and it is a hole this file had rather than a nicety.
 *
 * Every readback assertion below reaches the bytes through `atob`. That is a
 * real differential for the ALPHABET and the bit packing -- js_canvas.c's
 * b64_encode is a C table and js_platform.c's atob is a JS shim, they share no
 * line -- but it is NOT one for the PADDING, and the reason is in atob's
 * second statement: `if (s.length % 4 === 0) s = s.replace(/==?$/, '')`, then
 * a refusal only of `length % 4 === 1`. Unpadded base64 decodes there
 * perfectly. So an encoder that never emitted a '=' would keep all 68 checks
 * green while producing a data: URL that python, libpng, and the URL parser in
 * every other browser reject.
 *
 * That is this tree's own "wrong CRC polynomial computed the same way round
 * trips perfectly" shape, one layer up from where tests/pngenc.mk found it.
 * The answer is the same answer: a second oracle that shares no code with
 * either side. tests/unit/canvas_b64_ext_test.py is python3's
 * base64.b64decode(validate=True) plus binascii.crc32 and zlib, and this
 * function is how the URLs reach it.
 *
 * The dump is a corpus, not one string, and the sizes are chosen so the PNG
 * byte length lands in all three residues mod 3 -- with only n%3==0 there is
 * no padding in the file at all and the control could not fire. The python
 * side REFUSES a corpus that does not cover all three, rather than reporting
 * a happy count over a corpus that cannot fail. */
static void dump_url(const char *label, const char *jsexpr)
{
    const char *path = getenv("CANVAS_URL_DUMP");
    if (!path) return;
    char *u = evals(jsexpr);
    if (!u) { printf("FAIL: dump_url(%s) threw\n", label); failed++; return; }
    FILE *f = fopen(path, "a");
    if (!f) { printf("FAIL: dump_url cannot open %s\n", path); failed++; free(u); return; }
    fprintf(f, "%s\t%s\n", label, u);
    fclose(f);
    free(u);
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

    /* ---- readback: toDataURL / toBlob --------------------------------------
     *
     * THIS BLOCK REPLACES AN ASSERTION THAT THE METHOD THROWS, and the reason
     * the old one was right is the reason these have the shape they do. It
     * read: "A fabricated data URL is believed rather than detected -- it is
     * what every fingerprint and every format-support probe reads -- so this
     * must keep throwing for as long as there is no encoder behind it." There
     * is an encoder behind it now (rust/src/pngenc.rs, gated by make
     * test-pngenc against two oracles), so the condition attached to that
     * sentence has been met rather than waived.
     *
     * WHAT IS ASSERTED IS THE PIXELS, NOT THE SHAPE OF THE STRING. A data URL
     * of the right length with the right prefix is exactly the fabrication the
     * old note feared; the only assertion that cannot be satisfied by a
     * plausible-looking lie is one that decodes the URL and finds the colour
     * the test drew. So every check below goes through the bytes.
     */
    run("var e2 = document.createElement('canvas'); e2.width = 4; e2.height = 3;"
        "var g2 = e2.getContext('2d');"
        "g2.fillStyle = '#ff0000'; g2.fillRect(0,0,4,3);"
        "g2.fillStyle = '#0000ff'; g2.fillRect(0,0,1,1);"
        "var url = e2.toDataURL();"
        /* atob is js_platform.c's; decoding here rather than in C keeps the
         * assertion on what a PAGE can observe. */
        "var raw = atob(url.slice(url.indexOf(',') + 1));"
        "var by = []; for (var i = 0; i < raw.length; i++) by.push(raw.charCodeAt(i));");

    eq("toDataURL returns a data: URL that DECLARES image/png",
       "url.slice(0, 22)", "data:image/png;base64,");
    /* The eight-byte PNG signature, by value. A file that does not start with
     * these is not a PNG whatever its URL says. */
    eq("the decoded bytes carry the PNG signature",
       "by.slice(0,8).join(',')", "137,80,78,71,13,10,26,10");
    eq("the first chunk is IHDR",
       "String.fromCharCode(by[12],by[13],by[14],by[15])", "IHDR");
    /* THE DIMENSIONS COME FROM THE FILE, not from the element. A canvas that
     * encoded a default 300x150 while reporting 4x3 would pass every check
     * that only looks at the URL. */
    eq("IHDR width is the canvas width",
       "((by[16]<<24)|(by[17]<<16)|(by[18]<<8)|by[19])", "4");
    eq("IHDR height is the canvas height",
       "((by[20]<<24)|(by[21]<<16)|(by[22]<<8)|by[23])", "3");
    eq("IHDR is depth 8, colour type 6 (RGBA), no interlace",
       "[by[24],by[25],by[26],by[27],by[28]].join(',')", "8,6,0,0,0");
    eq("the file ends with IEND",
       "String.fromCharCode(by[by.length-8],by[by.length-7],by[by.length-6],by[by.length-5])",
       "IEND");

    /* THE PIXELS. The IDAT holds a zlib stream of STORED deflate blocks, so at
     * this size the raw scanlines are literally in the file and a page can
     * read them without an inflater: 8 signature + 25 IHDR chunk + 8 IDAT
     * length/type + 2 zlib header + 5 stored-block header = 48, then row 0 is
     * one filter byte followed by 4 RGBA pixels.
     *
     * This is the check the old refusal existed to protect: it fails for a
     * fabricated URL, for a URL of the wrong canvas, for a transposed axis and
     * for a swapped channel, and it cannot be satisfied by anything except
     * having actually encoded what was actually drawn. */
    eq("row 0 carries filter type 0 (None)", "by[48]", "0");
    eq("pixel (0,0) is the blue square that was painted over the red",
       "by.slice(49,53).join(',')", "0,0,255,255");
    eq("pixel (1,0) is the red fill",
       "by.slice(53,57).join(',')", "255,0,0,255");
    /* Row 0 is bytes 49..64 (4 px x 4 B); byte 65 is row 1's filter type;
     * row 1's first pixel is 66..69. Spelled as arithmetic on the row stride
     * rather than as a literal, so a reader can check it. */
    eq("row 1 carries its own filter byte", "by[48 + 1 + 4*4]", "0");
    eq("pixel (0,1), one row down, is red",
       "by.slice(48 + 1 + 4*4 + 1, 48 + 1 + 4*4 + 1 + 4).join(',')", "255,0,0,255");

    /* A canvas that never got a context is transparent black at its attribute
     * size -- the spec's answer, and the honest one: nothing was drawn. A
     * fingerprint probe calling toDataURL on a fresh canvas is a real shape,
     * and throwing there would put us back where this started. */
    run("var e3 = document.createElement('canvas'); e3.width = 2; e3.height = 2;"
        "var u3 = e3.toDataURL();"
        "var r3 = atob(u3.slice(u3.indexOf(',') + 1));"
        "var b3 = []; for (var i = 0; i < r3.length; i++) b3.push(r3.charCodeAt(i));");
    eq("a canvas with no context still encodes, at its own size",
       "((b3[16]<<24)|(b3[17]<<16)|(b3[18]<<8)|b3[19]) + 'x' +"
       "((b3[20]<<24)|(b3[21]<<16)|(b3[22]<<8)|b3[23])", "2x2");
    eq("and its pixels are transparent black, not garbage",
       "b3.slice(49,57).join(',')", "0,0,0,0,0,0,0,0");

    /* A zero-sized canvas is the spec's "data:," and the one case where there
     * are no pixels to be honest about. */
    eq("a zero-sized canvas returns data:,",
       "(function(){ var z = document.createElement('canvas');"
       "  z.width = 0; z.height = 0; return z.toDataURL(); })()", "data:,");

    /* THE MIME FALLBACK IS VISIBLE, WHICH IS THE WHOLE POINT. HTML says a UA
     * that cannot produce the requested type must use image/png; the lie would
     * be a `data:image/webp` prefix over PNG bytes, because that is exactly
     * what a "does this browser support webp" probe reads. Asking for webp
     * must therefore come back SAYING png. */
    eq("toDataURL('image/webp') falls back to PNG and says so in the URL",
       "e2.toDataURL('image/webp').slice(0, 22)", "data:image/png;base64,");
    eq("toDataURL('image/jpeg') too",
       "e2.toDataURL('image/jpeg').slice(0, 22)", "data:image/png;base64,");
    eq("and the fallback returns the same bytes as an explicit image/png",
       "e2.toDataURL('image/webp') === e2.toDataURL('image/png')", "true");

    /* ---- toBlob: async, and asserted to BE async ---------------------------
     *
     * Both halves, because each is a different bug. If the callback has run by
     * the time toBlob returns, this is the one asynchronous API in this
     * browser that is not, and a page doing `toBlob(cb); next();` sees the
     * wrong order. If it never runs after a pump, the work was queued
     * somewhere nothing drains. js_page_pump() is js_dom_run_jobs, the same
     * drain every promise reaction in this engine settles on -- which is the
     * claim being made: as async as a promise here, and no more. */
    eq("toBlob returns undefined and has NOT called back yet",
       "(function(){ __bl = 'pending';"
       "  var r = e2.toBlob(function (b) { __bl = b; });"
       "  return String(r) + '/' + (__bl === 'pending' ? 'deferred' : 'ran-inline'); })()",
       "undefined/deferred");
    js_page_pump();
    eq("after one pump the callback has run with a Blob",
       "(__bl && __bl.constructor && __bl.constructor.name)", "Blob");
    eq("the Blob declares image/png", "__bl.type", "image/png");
    eq("the Blob's bytes are the same PNG toDataURL produced",
       "(function(){ var b = __bl._b;"
       "  if (!b || b.length !== by.length) return 'len ' + (b && b.length) + ' vs ' + by.length;"
       "  for (var i = 0; i < b.length; i++) if (b[i] !== by[i]) return 'differ at ' + i;"
       "  return 'same'; })()", "same");
    eq("toBlob without a callback throws",
       "(function(){ try { e2.toBlob(); return 'no throw'; }"
       "  catch (x) { return x.constructor.name; } })()", "TypeError");

    /* ---- the corpus for the external oracle --------------------------------
     *
     * Written only when CANVAS_URL_DUMP names a file, so an ordinary run is
     * unchanged. The sizes are picked for their PNG LENGTHS, not their
     * pictures, because base64 only CARRIES padding when the byte count is not
     * a multiple of 3 -- a corpus of one residue could not fail the check it
     * exists for.
     *
     * A stored-block PNG of a w x h canvas is 68 + h*(1+4w) bytes: 8 signature
     * + 25 IHDR + 23 IDAT (4 length, 4 type, 2 zlib header, 5 stored-block
     * header, 4 Adler-32, 4 CRC) + 12 IEND. MEASURED, and the first draft of
     * this comment said 64 and named the wrong canvas for each residue -- it
     * had dropped the Adler-32 trailer, which is four bytes that only exist
     * because inflate.rs verifies them. The numbers below are read off
     * build/canvas_urls.txt rather than derived a second time:
     *
     *     1x1  ->  73 bytes,  73 % 3 == 1  ->  TWO '='
     *     2x1  ->  77 bytes,  77 % 3 == 2  ->  ONE '='
     *     1x2  ->  78 bytes,  78 % 3 == 0  ->  NO padding
     *
     * All three, so the padding check can fire. The 4x3 canvas is dumped as
     * well because it is the one carrying a KNOWN picture: it lets the oracle
     * assert a pixel from outside this tree, not merely a well-formed file. */
    run("var mk = function (w, h, fill) {"
        "  var e = document.createElement('canvas'); e.width = w; e.height = h;"
        "  if (fill) { var g = e.getContext('2d'); g.fillStyle = fill;"
        "              g.fillRect(0, 0, w, h); }"
        "  return e.toDataURL(); };");
    dump_url("1x1", "mk(1,1,'#00ff00')");
    dump_url("2x1", "mk(2,1,'#00ff00')");
    dump_url("1x2", "mk(1,2,'#00ff00')");
    dump_url("4x3-known", "url");

    /* ---- text: measureText / fillText / strokeText --------------------------
     *
     * Everything here runs against a SECOND canvas (t/q) so the readback
     * block's state above cannot be disturbed by font settings.
     *
     * WHAT IS AND IS NOT ASSERTED. The glyphs' antialiased EDGES are not this
     * block's subject -- c/lib/text's own suites hold the shaper to a
     * HarfBuzz differential and glyphras's flattening to a FreeType
     * differential, and re-deriving coverage here would be a third oracle
     * that can only disagree. What IS the subject is the PLUMBING: that a
     * measure and a draw agree BY CONSTRUCTION (both are shape_line at the
     * same px), that the CTM moves text where it moves a rect, that align /
     * baseline / maxWidth / strokeStyle each move ink, and that the font
     * shorthand parses or refuses honestly. The ink extents below are
     * INTEGERS -- shape_line's whole-pixel pen -- so no coverage tolerance
     * is involved anywhere.
     *
     * make test-canvas-text-negctl compiles this suite with
     * -DCANVAS_TEXT_ABSENT: every check in this block must REDDEN there,
     * because that flag is the pre-2026-08-30 build (registrations absent,
     * `typeof ctx.fillText` undefined) -- which is exactly the state the
     * dead canvas agent left this file in, bodies written, none reachable.
     */
    run("var t = document.createElement('canvas'); t.width = 64; t.height = 40;"
        "var q = t.getContext('2d');"
        /* ink(x0,x1,y0,y1): opaque-ink COUNT:FIRSTCOL:LASTCOL over the box --
         * extents, not coverage, for the reason above. p2 is this block's
         * px(), on q: the checks below draw on the SECOND canvas and must
         * not read the first one's pixels by accident. */
        "function ink(x0, x1, y0, y1) {"
        "  var n = 0, first = -1, last = -1;"
        "  for (var y = y0; y < y1; y++) for (var x = x0; x < x1; x++) {"
        "    if (q.getImageData(x, y, 1, 1).data[3] !== 0) {"
        "      n++; if (first < 0) first = x; last = x; } }"
        "  return n + ':' + first + ':' + last; }"
        "function p2(x, y) { var d = q.getImageData(x, y, 1, 1).data;"
        "  return d[0] + ',' + d[1] + ',' + d[2] + ',' + d[3]; }");

    eq("measureText returns a TextMetrics with a numeric width",
       "typeof q.measureText('W').width", "number");
    eq("a wider string measures wider",
       "q.measureText('WWWW').width > q.measureText('W').width", "true");
    eq("widths are whole user pixels (the shaper's integer pen)",
       "q.measureText('Hello, world!').width % 1", "0");
    run("q.font = '16px monospace';");
    eq("a monospace face advances identically per glyph",
       "q.measureText('iiii').width === 4 * q.measureText('i').width", "true");
    run("q.font = '20px sans-serif';");
    eq("the font string round-trips what was set",
       "(function(){ q.font = 'bold 20px monospace'; return q.font; })()",
       "bold 20px monospace");
    eq("an unparseable font keeps the previous one (no em/%/keywords)",
       "(function(){ q.font = '12em serif'; return q.font; })()",
       "bold 20px monospace");
    eq("the default font string",
       "(function(){ var f = document.createElement('canvas');"
       "  return f.getContext('2d').font; })()", "10px sans-serif");

    /* THE MEASURE/DRAW AGREEMENT, as an integer identity: the same string at
     * the same font drawn at x and at x+width starts its ink exactly `width`
     * further right. If measure ever took a different path than draw (a
     * per-character-advance measure misses ligatures and kern pairs), this
     * difference stops being the width. */
    eq("the pen advances exactly measureText's width between two draws",
       "(function(){ q.clearRect(0,0,64,40); q.font = '20px sans-serif';"
       "  q.fillText('Hi!', 4, 20);"
       "  var a = parseInt(ink(0, 30, 0, 40).split(':')[1], 10);"
       "  var w = q.measureText('Hi!').width;"
       "  q.clearRect(0,0,64,40);"
       "  q.fillText('Hi!', 4 + w, 20);"
       "  var b = parseInt(ink(0, 64, 0, 40).split(':')[1], 10);"
       "  return b - a - w; })()", "0");

    run("q.clearRect(0,0,64,40); q.fillStyle = '#ffffff'; q.fillText('WWWW', 2, 20);");
    eq("fillText paints ink", "ink(0, 40, 0, 40).split(':')[0] != '0'", "true");
    eq("fillText honours the fill colour",
       "(function(){ for (var y = 0; y < 40; y++) for (var x = 0; x < 40; x++) {"
       "  var d = q.getImageData(x, y, 1, 1).data;"
       "  if (d[0] === 255 && d[1] === 255 && d[2] === 255 && d[3] === 255) return true; }"
       "  return false; })()", "true");
    eq("fillText leaves the row above the ascendant untouched",
       "ink(0, 64, 0, 2).split(':')[0]", "0");

    /* The CTM must move text exactly where it moves a rect -- the same
     * property test-canvas-negctl's -DCANVAS_IGNORE_CTM exists for, so this
     * check reddens there too. */
    eq("a scaled CTM scales the glyphs",
       "(function(){ q.clearRect(0,0,64,40); q.font = '10px sans-serif';"
       "  q.fillText('W', 2, 10);"
       "  var c1 = parseInt(ink(0, 30, 0, 40).split(':')[0], 10);"
       "  q.clearRect(0,0,64,40);"
       "  q.save(); q.scale(2, 2); q.fillText('W', 2, 10); q.restore();"
       "  var c2 = parseInt(ink(0, 64, 0, 40).split(':')[0], 10);"
       "  return c2 > 2 * c1; })()", "true");

    eq("textAlign='right' puts the pen at the RIGHT edge",
       "(function(){ q.clearRect(0,0,64,40); q.font = '20px sans-serif';"
       "  q.textAlign = 'right'; q.fillText('WWWW', 50, 20); q.textAlign = 'left';"
       "  var e = ink(0, 64, 0, 40).split(':');"
       "  return parseInt(e[2], 10) <= 50 &&"
       "         parseInt(e[2], 10) > 50 - q.measureText('WWWW').width; })()", "true");
    eq("textAlign round-trips, start collapsing to left (no ctx.direction)",
       "(function(){ q.textAlign = 'start'; return q.textAlign; })()", "left");
    eq("textBaseline defaults to alphabetic", "q.textBaseline", "alphabetic");
    eq("textBaseline round-trips 'top'",
       "(function(){ q.textBaseline = 'top'; return q.textBaseline; })()", "top");
    eq("'hanging' is stored and read back, drawn as alphabetic",
       "(function(){ q.textBaseline = 'hanging'; var s = q.textBaseline;"
       "  q.textBaseline = 'alphabetic'; return s; })()", "hanging");
    eq("an unknown textBaseline is ignored, keeping the previous value",
       "(function(){ q.textBaseline = 'middle'; q.textBaseline = 'subscript';"
       "  return q.textBaseline; })()", "middle");
    run("q.textBaseline = 'alphabetic';");

    eq("strokeText draws with the STROKE paint and lineWidth",
       "(function(){ q.clearRect(0,0,64,40); q.font = '24px sans-serif';"
       "  q.lineWidth = 2; q.strokeStyle = '#ff0000'; q.strokeText('W', 4, 24);"
       "  var n = 0;"
       "  for (var y = 0; y < 40; y++) for (var x = 0; x < 40; x++) {"
       "    var d = q.getImageData(x, y, 1, 1).data;"
       "    if (d[3] !== 0 && d[0] > 200 && d[1] < 60) n++; }"
       "  return n > 0; })()", "true");

    eq("maxWidth squashes the run horizontally",
       "(function(){ q.clearRect(0,0,64,40); q.font = '20px sans-serif';"
       "  var w = q.measureText('WWWWWW').width;"
       "  q.fillText('WWWWWW', 2, 20, w / 2);"
       "  var e = ink(0, 64, 0, 40).split(':');"
       "  return (parseInt(e[2], 10) - parseInt(e[1], 10)) < 0.75 * w; })()", "true");
    eq("maxWidth 0 draws nothing (the squash limit, not a full-width draw)",
       "(function(){ q.clearRect(0,0,64,40); q.font = '20px sans-serif';"
       "  q.fillText('WWWW', 2, 20, 0);"
       "  return ink(0, 64, 0, 40).split(':')[0]; })()", "0");
    eq("a string over the layout scratch is refused, not measured truncated",
       "(function(){ q.font = '10px monospace';"
       "  return q.measureText(new Array(1100).join('W')).width; })()", "0");
    run("q.font = '20px sans-serif';");

    /* ---- drawImage: three arities, two sources, the transform --------------
     *
     * s1 is a 2x1 canvas, red|blue -- small enough that every mapped pixel is
     * nameable and NEAREST sampling has no interesting interior.
     *
     * The 5-argument check is the one that catches the composition-order bug
     * the dead agent's draft shipped: with the img2dev matrix multiplied
     * destination-translate FIRST (the wrong order for post-multiplying
     * helpers), a 2x-scaled draw at dx=5 lands its origin at x=10 and every
     * pixel below asserts transparent.
     */
    run("var s1 = document.createElement('canvas'); s1.width = 2; s1.height = 1;"
        "var h1 = s1.getContext('2d');"
        "h1.fillStyle = '#ff0000'; h1.fillRect(0, 0, 1, 1);"
        "h1.fillStyle = '#0000ff'; h1.fillRect(1, 0, 1, 1);"
        "q.clearRect(0, 0, 64, 40);");
    eq("q is clean before the image checks", "p2(5,5)", "0,0,0,0");

    run("q.clearRect(0,0,64,40); q.drawImage(s1, 5, 5);");
    eq("drawImage 3-arg paints the source at natural size (left)",
       "p2(5,5)", "255,0,0,255");
    eq("drawImage 3-arg paints the source at natural size (right)",
       "p2(6,5)", "0,0,255,255");
    eq("and nothing beyond it", "p2(7,5)", "0,0,0,0");

    run("q.clearRect(0,0,64,40); q.drawImage(s1, 5, 5, 4, 2);");
    eq("drawImage 5-arg scales up (left half red)",
       "p2(6,5)", "255,0,0,255");
    eq("drawImage 5-arg scales up (right half blue)",
       "p2(7,5)", "0,0,255,255");
    eq("the destination rect is where the page put it (not CTM-order-flipped)",
       "p2(4,5)", "0,0,0,0");

    run("q.clearRect(0,0,64,40); q.drawImage(s1, 1, 0, 1, 1, 5, 5, 2, 2);");
    eq("drawImage 9-arg takes only the blue sub-rect",
       "(function(){ return p2(5,5) + '|' + p2(6,6); })()", "0,0,255,255|0,0,255,255");
    eq("the 9-arg draw stays inside its destination rect",
       "p2(7,7)", "0,0,0,0");

    /* 4 arguments: dw given, dh missing. The missing dh is WebIDL's NaN, not
     * a default, so the call draws NOTHING -- pinning the rule the obvious
     * parser gets wrong (and the dead agent's draft did). */
    run("q.clearRect(0,0,64,40); q.drawImage(s1, 5, 5, 4);");
    eq("a 4-argument drawImage draws nothing (dh is NaN, not defaulted)",
       "p2(5,5)", "0,0,0,0");
    eq("6..8 arguments match no overload and throw TypeError",
       "(function(){ try { q.drawImage(s1, 0, 0, 1, 1, 0, 0); return 'no throw'; }"
       "  catch (e) { return e.constructor.name; } })()", "TypeError");

    /* SELF-DRAW. A canvas drawing itself must read a SNAPSHOT of the sampled
     * sub-rect: the composite writes destination pixels left to right, and
     * dest[x] samples src[x-1] -- a pixel one step of the same sweep has
     * already rewritten. r,b,r,b shifted right by one makes dest[2]'s answer
     * the whole distinction: the snapshot reads src[1] = BLUE; a live read
     * sees the red that was just written over x=1 and answers RED. */
    eq("a canvas drawing ITSELF reads a snapshot, not rows it is rewriting",
       "(function(){ var z = document.createElement('canvas'); z.width = 4; z.height = 1;"
       "  var zz = z.getContext('2d');"
       "  zz.fillStyle = '#ff0000'; zz.fillRect(0, 0, 1, 1); zz.fillRect(2, 0, 1, 1);"
       "  zz.fillStyle = '#0000ff'; zz.fillRect(1, 0, 1, 1); zz.fillRect(3, 0, 1, 1);"
       "  zz.drawImage(z, 0, 0, 4, 1, 1, 0, 4, 1);"
       "  return zz.getImageData(2, 0, 1, 1).data[2]; })()", "255");

    eq("drawImage from an <img> whose src is a data: URL the page built",
       "(function(){ var im = document.createElement('img');"
       "  im.setAttribute('src', s1.toDataURL());"
       "  q.clearRect(0,0,64,40); q.drawImage(im, 3, 3);"
       "  var a = q.getImageData(3, 3, 1, 1).data, b = q.getImageData(4, 3, 1, 1).data;"
       "  return a[0] + ',' + b[2]; })()", "255,255");
    eq("a canvas with no context is a valid source that paints nothing",
       "(function(){ var b = document.createElement('canvas'); b.width = 2; b.height = 1;"
       "  q.clearRect(0,0,64,40); var r = q.drawImage(b, 1, 1);"
       "  return String(r) + ',' + q.getImageData(1, 1, 1, 1).data[3]; })()", "undefined,0");
    eq("an undecodable <img> is silence, not an exception",
       "(function(){ var im = document.createElement('img');"
       "  im.setAttribute('src', 'data:text/plain;base64,QUJD');"
       "  q.clearRect(0,0,64,40); q.drawImage(im, 0, 0);"
       "  return q.getImageData(0, 0, 1, 1).data[3]; })()", "0");
    eq("a source that is not an image at all is a TypeError",
       "(function(){ try { q.drawImage({}, 0, 0); return 'no throw'; }"
       "  catch (e) { return e.constructor.name; } })()", "TypeError");

    eq("imageSmoothingEnabled tells the truth about this build: false",
       "q.imageSmoothingEnabled", "false");
    eq("setting imageSmoothingEnabled=true is accepted and stays false",
       "(function(){ q.imageSmoothingEnabled = true;"
       "  return q.imageSmoothingEnabled; })()", "false");

    js_page_close();
    if (failed) { printf("\ncanvas_test: %d/%d checks FAILED\n", failed, checks); return 1; }
    printf("\ncanvas_test: %d checks pass\n", checks);
    return 0;
}
