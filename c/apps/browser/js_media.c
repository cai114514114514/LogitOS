/* Media Source Extensions: the JavaScript surface.
 *
 * js_media_src.c owns the bytes, the demuxer, the decoders and the clock and
 * knows nothing about JS. This file owns the objects a player library actually
 * touches -- MediaSource, SourceBuffer, TimeRanges, URL.createObjectURL and the
 * HTMLMediaElement members on a <video> -- and the events they fire.
 *
 * TWO THINGS ARE WORTH KNOWING BEFORE READING IT.
 *
 * 1. HOW A <video> IS IDENTIFIED. js_dom.c owns the Element wrapper and exports
 *    no way to get the `struct node *` out of a JSValue, and browser_paint.c
 *    hands the painter a `struct node *` with no way back to a JSValue. Both
 *    files belong to other lines. So the element carries an integer key of its
 *    own, in a `data-logit-mediaid` attribute: the JS shim stamps it with
 *    setAttribute, every native call passes the integer, and media_paint_box()
 *    below reads it back off the node with dom_attr(). Public surface on both
 *    sides, stable under DOM mutation. `struct node *js_dom_node_of(JSValueConst)`
 *    exported from js_dom.c would delete the whole mechanism; it is an ask, not
 *    an edit.
 *
 * 2. WHAT DRIVES THE PICTURE. The browser's main loop calls js_page_run_due()
 *    whenever timers are pending, so a playing element keeps itself alive with a
 *    setTimeout chain started from C. Each tick steps media_pump(), which
 *    decodes at most one due picture and blits it straight into the box
 *    browser_paint.c last reported -- the video rect only, not a page repaint.
 *    That is deliberate: re-laying out a document thirty times a second to move
 *    a rectangle of pixels is the cost that makes video impossible.
 *
 * THE SHIM IS PART OF THE IMPLEMENTATION, not decoration. Everything that is
 * plumbing rather than policy -- the EventTarget mixin, the on* properties,
 * TimeRanges, the HTMLMediaElement accessors -- is written in JavaScript at the
 * bottom of this file, the same way js_webapi.c and js_platform.c do it. The C
 * is the part that has to reach the engine.
 */
#include "quickjs.h"
#include "dom.h"
#include "js_media.h"
#include "js_dom.h"
#include "logit.h"
#include <string.h>
#include <stdlib.h>

int printf(const char *, ...);

/* ============================================== the OS platform ========= */
/* The engine asks for these five drawing calls and five sound calls; here they
 * are the syscalls. On the host test they are recorders. Nothing in
 * js_media_src.c knows which. */
static unsigned long long os_now(void) { return monotonic_ns(); }
static void os_blit(int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh)
{ gui_blit(x, y, w, h, rgba, sw, sh); }
static void os_fill(int x, int y, int w, int h, unsigned rgb)
{ gui_rect(x, y, w, h, rgb); }
static void os_clip(int x, int y, int w, int h) { gui_clip(x, y, w, h); }
static void os_flush(void) { gui_flush(); }
static int  os_snd_open(int rate, int ch)
{
    struct logit_sndinfo si;
    if (snd_info(&si) != 1) return -1;          /* no card: play silent, in time */
    /* NON-BLOCKING, and that is not a preference. A blocking snd_write PARKS
     * the thread when the ring is full (500 ms backstop, see snd_stream_write
     * in c/kernel/audio/mixer.c) -- and this thread is the one that decodes the
     * next picture, runs the page's timers and answers the mouse. A player that
     * can be parked by its own sound card is a browser that freezes while a
     * video plays. The pump asks snd_avail() how much room there is and writes
     * that much; a short write is a normal answer here, not an error. */
    struct logit_sndfmt f;
    f.rate = (unsigned)rate;
    f.channels = (unsigned short)ch;
    f.format = SND_FMT_S16;
    f.buffer_ms = 0;
    f.flags = SND_F_NONBLOCK;
    return snd_open(&f);
}
static int  os_snd_write(int h, const void *b, int n) { return snd_write(h, b, n); }
static int  os_snd_avail(int h) { return snd_avail(h); }
static long long os_snd_played(int h)
{
    struct logit_sndstate st;
    if (snd_state(h, &st) != 0) return -1;
    return (long long)st.frames_played;
}
static void os_snd_close(int h, int drain) { snd_close(h, drain); }

static const struct media_platform g_osplat = {
    os_now, os_blit, os_fill, os_clip, os_flush,
    os_snd_open, os_snd_write, os_snd_avail, os_snd_played, os_snd_close
};

/* ---- node -> element key ---------------------------------------------------
 * The only place in this feature that touches the DOM. */
#define MEDIA_ID_ATTR "data-logit-mediaid"

static int node_media_key(struct node *n)
{
    if (!n) return 0;
    const char *v = dom_attr(n, MEDIA_ID_ATTR);
    if (!v) return 0;
    int k = 0;
    for (const char *p = v; *p >= '0' && *p <= '9'; p++) {
        k = k * 10 + (*p - '0');
        if (k > 1000000) return 0;
    }
    return k;
}

void media_paint_box(struct node *node, int x, int y, int w, int h,
                     int clip_x, int clip_y, int clip_w, int clip_h)
{
    media_paint_key(node_media_key(node), x, y, w, h, clip_x, clip_y, clip_w, clip_h);
}

/* =============================================== MediaSource class ====== */
static JSClassID g_ms_cid, g_sb_cid;
static JSContext *g_ctx;
static int g_pump_armed;                 /* a setTimeout tick is outstanding */

/* Every live MediaSource/SourceBuffer wrapper, so the pump can fire events on
 * them without the engine holding a JSValue. Bounded, like everything else. */
#define MAXWRAP 16
static struct { msource *ms; JSValue obj; } g_mswrap[MAXWRAP];
static struct { sbuf *sb; JSValue obj; int pending_end; int last_state; } g_sbwrap[MAXWRAP];
static struct { int key; JSValue obj; } g_elwrap[MAXWRAP];

static void wrap_ms(msource *ms, JSValue obj)
{
    for (int i = 0; i < MAXWRAP; i++)
        if (!g_mswrap[i].ms) { g_mswrap[i].ms = ms; g_mswrap[i].obj = obj; return; }
    JS_FreeValue(g_ctx, obj);
}
static JSValue ms_obj(msource *ms)
{
    for (int i = 0; i < MAXWRAP; i++) if (g_mswrap[i].ms == ms) return g_mswrap[i].obj;
    return JS_UNDEFINED;
}
static void wrap_sb(sbuf *sb, JSValue obj)
{
    for (int i = 0; i < MAXWRAP; i++)
        if (!g_sbwrap[i].sb) { g_sbwrap[i].sb = sb; g_sbwrap[i].obj = obj;
                               g_sbwrap[i].pending_end = 0; return; }
    JS_FreeValue(g_ctx, obj);
}
static void unwrap_sb(sbuf *sb)
{
    for (int i = 0; i < MAXWRAP; i++)
        if (g_sbwrap[i].sb == sb) { JS_FreeValue(g_ctx, g_sbwrap[i].obj);
                                    g_sbwrap[i].sb = 0; g_sbwrap[i].obj = JS_UNDEFINED; }
}

/* Fire an event on one of our objects by calling the shim's __fire, which owns
 * the listener list and the on* property. Doing the dispatch in JS rather than
 * in C is what keeps this file's event code to four lines. */
static void fire(JSValueConst obj, const char *type)
{
    if (!g_ctx || JS_IsUndefined(obj)) return;
    JSValue f = JS_GetPropertyStr(g_ctx, obj, "__fire");
    if (JS_IsFunction(g_ctx, f)) {
        JSValue a = JS_NewString(g_ctx, type);
        JSValue r = JS_Call(g_ctx, f, obj, 1, (JSValueConst *)&a);
        if (JS_IsException(r)) JS_FreeValue(g_ctx, JS_GetException(g_ctx));
        JS_FreeValue(g_ctx, r);
        JS_FreeValue(g_ctx, a);
    }
    JS_FreeValue(g_ctx, f);
}

static JSValue throw_mse(JSContext *ctx, int err)
{
    switch (err) {
    case MSE_E_NOTSUPPORTED:
        return JS_ThrowTypeError(ctx, "NotSupportedError: this browser cannot "
                                      "decode that type -- MediaSource.isTypeSupported "
                                      "says so, and it is telling the truth");
    case MSE_E_INVALIDSTATE: return JS_ThrowTypeError(ctx, "InvalidStateError");
    case MSE_E_QUOTA:        return JS_ThrowRangeError(ctx, "QuotaExceededError");
    case MSE_E_OOM:          return JS_ThrowOutOfMemory(ctx);
    default:                 return JS_ThrowTypeError(ctx, "MediaSource error %d", err);
    }
}

/* ---- MediaSource ---- */
static void ms_finalizer(JSRuntime *rt, JSValue val)
{
    msource *ms = JS_GetOpaque(val, g_ms_cid);
    (void)rt;
    if (!ms) return;
    for (int i = 0; i < MAXWRAP; i++) if (g_mswrap[i].ms == ms) g_mswrap[i].ms = 0;
    mse_free(ms);
}
static JSClassDef g_ms_class = { "MediaSource", .finalizer = ms_finalizer };

static msource *ms_of(JSContext *ctx, JSValueConst v)
{
    return JS_GetOpaque2(ctx, v, g_ms_cid);
}

static JSValue js_ms_ctor(JSContext *ctx, JSValueConst nt, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    JSValue proto = JS_GetPropertyStr(ctx, nt, "prototype");
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, g_ms_cid);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) return obj;
    msource *ms = mse_new();
    if (!ms) { JS_FreeValue(ctx, obj); return JS_ThrowOutOfMemory(ctx); }
    JS_SetOpaque(obj, ms);
    wrap_ms(ms, JS_DupValue(ctx, obj));
    JS_SetPropertyStr(ctx, obj, "sourceBuffers", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, obj, "activeSourceBuffers", JS_NewArray(ctx));
    return obj;
}

static JSValue js_ms_isTypeSupported(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_FALSE;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (!s) return JS_FALSE;
    int ok = mse_type_supported(s);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

static JSValue js_ms_addSourceBuffer(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    msource *ms = ms_of(ctx, t);
    if (!ms) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "addSourceBuffer needs a type");
    const char *type = JS_ToCString(ctx, argv[0]);
    if (!type) return JS_EXCEPTION;
    int err = 0;
    sbuf *sb = mse_add_source_buffer(ms, type, &err);
    JS_FreeCString(ctx, type);
    if (!sb) return throw_mse(ctx, err);

    JSValue proto = JS_GetClassProto(ctx, g_sb_cid);
    JSValue obj = JS_NewObjectProtoClass(ctx, proto, g_sb_cid);
    JS_FreeValue(ctx, proto);
    JS_SetOpaque(obj, sb);
    wrap_sb(sb, JS_DupValue(ctx, obj));
    /* Wire the shim's EventTarget onto it, and publish it in sourceBuffers. */
    JSValue init = JS_GetPropertyStr(ctx, t, "__initSB");
    if (JS_IsFunction(ctx, init)) {
        JSValueConst a[1] = { obj };
        JSValue r = JS_Call(ctx, init, t, 1, a);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, init);
    return obj;
}

static JSValue js_ms_removeSourceBuffer(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    msource *ms = ms_of(ctx, t);
    if (!ms || argc < 1) return JS_EXCEPTION;
    sbuf *sb = JS_GetOpaque2(ctx, argv[0], g_sb_cid);
    if (!sb) return JS_EXCEPTION;
    JS_SetOpaque((JSValue)argv[0], 0);
    unwrap_sb(sb);
    int rc = mse_remove_source_buffer(ms, sb);
    if (rc != MSE_OK) return throw_mse(ctx, rc);
    return JS_UNDEFINED;
}

static JSValue js_ms_endOfStream(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    msource *ms = ms_of(ctx, t);
    if (!ms) return JS_EXCEPTION;
    const char *why = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(ctx, argv[0]) : 0;
    int rc = mse_end_of_stream(ms, why);
    if (why) JS_FreeCString(ctx, why);
    if (rc != MSE_OK) return throw_mse(ctx, rc);
    fire(t, "sourceended");
    return JS_UNDEFINED;
}

static JSValue js_ms_get_readyState(JSContext *ctx, JSValueConst t)
{
    msource *ms = ms_of(ctx, t);
    if (!ms) return JS_EXCEPTION;
    int s = mse_state(ms);
    return JS_NewString(ctx, s == MSE_OPEN ? "open" : s == MSE_ENDED ? "ended" : "closed");
}
static JSValue js_ms_get_duration(JSContext *ctx, JSValueConst t)
{
    msource *ms = ms_of(ctx, t);
    if (!ms) return JS_EXCEPTION;
    double d = mse_duration(ms);
    /* An unset duration is NaN in the spec, not zero -- and a player that reads
     * it to size a scrub bar behaves very differently for the two. */
    return JS_NewFloat64(ctx, d < 0 ? (0.0 / 0.0) : d);
}
static JSValue js_ms_set_duration(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    msource *ms = ms_of(ctx, t);
    if (!ms) return JS_EXCEPTION;
    double d = 0;
    if (JS_ToFloat64(ctx, &d, v)) return JS_EXCEPTION;
    int rc = mse_set_duration(ms, d);
    if (rc != MSE_OK) return throw_mse(ctx, rc);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry g_ms_proto[] = {
    JS_CFUNC_DEF("addSourceBuffer", 1, js_ms_addSourceBuffer),
    JS_CFUNC_DEF("removeSourceBuffer", 1, js_ms_removeSourceBuffer),
    JS_CFUNC_DEF("endOfStream", 0, js_ms_endOfStream),
    JS_CGETSET_DEF("readyState", js_ms_get_readyState, 0),
    JS_CGETSET_DEF("duration", js_ms_get_duration, js_ms_set_duration),
};

/* ---- SourceBuffer ---- */
static void sb_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    sbuf *sb = JS_GetOpaque(val, g_sb_cid);
    if (!sb) return;
    for (int i = 0; i < MAXWRAP; i++) if (g_sbwrap[i].sb == sb) g_sbwrap[i].sb = 0;
    /* The buffer belongs to its MediaSource, which frees it. */
}
static JSClassDef g_sb_class = { "SourceBuffer", .finalizer = sb_finalizer };

static void arm_pump(void);

static JSValue js_sb_appendBuffer(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    sbuf *sb = JS_GetOpaque2(ctx, t, g_sb_cid);
    if (!sb) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "appendBuffer needs a BufferSource");

    /* BufferSource is an ArrayBuffer OR any view onto one, and a real player
     * hands over whichever its fetch produced. Both are unwrapped to the same
     * bytes here rather than making the page convert. */
    size_t len = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &len, argv[0]);
    JSValue held = JS_UNDEFINED;
    if (!p) {
        size_t off = 0, blen = 0, bpe = 0;
        held = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &blen, &bpe);
        if (JS_IsException(held)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return JS_ThrowTypeError(ctx, "appendBuffer wants an ArrayBuffer or a view");
        }
        size_t whole = 0;
        uint8_t *base = JS_GetArrayBuffer(ctx, &whole, held);
        if (!base) { JS_FreeValue(ctx, held); return JS_ThrowTypeError(ctx, "detached buffer"); }
        p = base + off;
        len = blen;
    }
    int rc = sb_append(sb, p, (long)len);
    JS_FreeValue(ctx, held);
    if (rc != MSE_OK) {
        fire(t, "error");
        return throw_mse(ctx, rc);
    }
    /* The spec's append is asynchronous: updating goes true here and the
     * update/updateend pair fires from the event loop. Making that real (rather
     * than firing inline) is what lets a player's updateend handler append the
     * NEXT segment without recursing into the middle of this one. */
    JS_SetPropertyStr(ctx, (JSValue)t, "__updating", JS_TRUE);
    for (int i = 0; i < MAXWRAP; i++) if (g_sbwrap[i].sb == sb) g_sbwrap[i].pending_end = 1;
    fire(t, "updatestart");
    arm_pump();
    return JS_UNDEFINED;
}

static JSValue js_sb_remove(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    sbuf *sb = JS_GetOpaque2(ctx, t, g_sb_cid);
    if (!sb || argc < 2) return JS_EXCEPTION;
    double a = 0, b = 0;
    if (JS_ToFloat64(ctx, &a, argv[0]) || JS_ToFloat64(ctx, &b, argv[1])) return JS_EXCEPTION;
    int rc = sb_remove(sb, a, b);
    if (rc != MSE_OK) return throw_mse(ctx, rc);
    JS_SetPropertyStr(ctx, (JSValue)t, "__updating", JS_TRUE);
    for (int i = 0; i < MAXWRAP; i++) if (g_sbwrap[i].sb == sb) g_sbwrap[i].pending_end = 1;
    fire(t, "updatestart");
    arm_pump();
    return JS_UNDEFINED;
}

static JSValue js_sb_abort(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    sbuf *sb = JS_GetOpaque2(ctx, t, g_sb_cid);
    if (!sb) return JS_EXCEPTION;
    sb_abort(sb);
    JS_SetPropertyStr(ctx, (JSValue)t, "__updating", JS_FALSE);
    fire(t, "abort");
    return JS_UNDEFINED;
}

/* TimeRanges, built as a plain object with the two accessor methods -- which is
 * all a player ever calls on one. */
static JSValue make_ranges(JSContext *ctx, int n, double *starts, double *ends)
{
    JSValue arr_s = JS_NewArray(ctx), arr_e = JS_NewArray(ctx);
    for (int i = 0; i < n; i++) {
        JS_SetPropertyUint32(ctx, arr_s, (uint32_t)i, JS_NewFloat64(ctx, starts[i]));
        JS_SetPropertyUint32(ctx, arr_e, (uint32_t)i, JS_NewFloat64(ctx, ends[i]));
    }
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue mk = JS_GetPropertyStr(ctx, g, "__mkTimeRanges");
    JSValue out;
    if (JS_IsFunction(ctx, mk)) {
        JSValueConst a[2] = { arr_s, arr_e };
        out = JS_Call(ctx, mk, JS_UNDEFINED, 2, a);
    } else {
        out = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, out, "length", JS_NewInt32(ctx, n));
    }
    JS_FreeValue(ctx, mk);
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, arr_s);
    JS_FreeValue(ctx, arr_e);
    return out;
}

#define MAXR 16
static JSValue js_sb_get_buffered(JSContext *ctx, JSValueConst t)
{
    sbuf *sb = JS_GetOpaque2(ctx, t, g_sb_cid);
    if (!sb) return JS_EXCEPTION;
    double s[MAXR], e[MAXR];
    int n = sb_buffered_count(sb);
    if (n > MAXR) n = MAXR;
    for (int i = 0; i < n; i++) sb_buffered_range(sb, i, &s[i], &e[i]);
    return make_ranges(ctx, n, s, e);
}
static JSValue js_sb_get_mode(JSContext *ctx, JSValueConst t)
{
    sbuf *sb = JS_GetOpaque2(ctx, t, g_sb_cid);
    if (!sb) return JS_EXCEPTION;
    return JS_NewString(ctx, sb_mode(sb) == MSE_MODE_SEQUENCE ? "sequence" : "segments");
}
static JSValue js_sb_set_mode(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    sbuf *sb = JS_GetOpaque2(ctx, t, g_sb_cid);
    if (!sb) return JS_EXCEPTION;
    const char *s = JS_ToCString(ctx, v);
    if (!s) return JS_EXCEPTION;
    int seq = !strcmp(s, "sequence");
    int ok = seq || !strcmp(s, "segments");
    JS_FreeCString(ctx, s);
    if (!ok) return JS_ThrowTypeError(ctx, "mode must be 'segments' or 'sequence'");
    sb_set_mode(sb, seq ? MSE_MODE_SEQUENCE : MSE_MODE_SEGMENTS);
    return JS_UNDEFINED;
}
static JSValue js_sb_get_tso(JSContext *ctx, JSValueConst t)
{
    sbuf *sb = JS_GetOpaque2(ctx, t, g_sb_cid);
    if (!sb) return JS_EXCEPTION;
    return JS_NewFloat64(ctx, sb_timestamp_offset(sb));
}
static JSValue js_sb_set_tso(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    sbuf *sb = JS_GetOpaque2(ctx, t, g_sb_cid);
    if (!sb) return JS_EXCEPTION;
    double d = 0;
    if (JS_ToFloat64(ctx, &d, v)) return JS_EXCEPTION;
    sb_set_timestamp_offset(sb, d);
    return JS_UNDEFINED;
}
static JSValue js_sb_get_updating(JSContext *ctx, JSValueConst t)
{
    JSValue u = JS_GetPropertyStr(ctx, t, "__updating");
    int b = JS_ToBool(ctx, u);
    JS_FreeValue(ctx, u);
    return JS_NewBool(ctx, b);
}

static const JSCFunctionListEntry g_sb_proto[] = {
    JS_CFUNC_DEF("appendBuffer", 1, js_sb_appendBuffer),
    JS_CFUNC_DEF("remove", 2, js_sb_remove),
    JS_CFUNC_DEF("abort", 0, js_sb_abort),
    JS_CGETSET_DEF("buffered", js_sb_get_buffered, 0),
    JS_CGETSET_DEF("mode", js_sb_get_mode, js_sb_set_mode),
    JS_CGETSET_DEF("timestampOffset", js_sb_get_tso, js_sb_set_tso),
    JS_CGETSET_DEF("updating", js_sb_get_updating, 0),
};

/* ================================== the HTMLMediaElement natives ======== */
/* One entry point per verb, all taking the element's integer key. The shim
 * turns them into properties and methods on the element. */
static melem *el_of(int key, int create) { return mel_for_key(key, create); }

static JSValue js_m_bind(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int key = 0;
    if (argc < 1 || JS_ToInt32(ctx, &key, argv[0])) return JS_FALSE;
    melem *el = el_of(key, 1);
    if (!el) return JS_FALSE;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        for (int i = 0; i < MAXWRAP; i++)
            if (!g_elwrap[i].key) { g_elwrap[i].key = key;
                                    g_elwrap[i].obj = JS_DupValue(ctx, argv[1]); break; }
    }
    return JS_TRUE;
}

static JSValue js_m_src(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int key = 0;
    if (argc < 2 || JS_ToInt32(ctx, &key, argv[0])) return JS_EXCEPTION;
    melem *el = el_of(key, 1);
    if (!el) return JS_ThrowInternalError(ctx, "too many media elements");

    /* srcObject = mediaSource is the modern spelling and needs no object URL. */
    msource *direct = JS_GetOpaque(argv[1], g_ms_cid);
    int rc;
    if (direct) {
        rc = mse_attach(direct, el);
    } else {
        const char *u = JS_ToCString(ctx, argv[1]);
        if (!u) return JS_EXCEPTION;
        rc = mel_attach_url(el, u);
        JS_FreeCString(ctx, u);
    }
    if (rc == MSE_OK) {
        msource *ms = direct;
        if (!ms) {
            const char *u = JS_ToCString(ctx, argv[1]);
            if (u) { ms = mse_from_object_url(u); JS_FreeCString(ctx, u); }
        }
        if (ms) fire(ms_obj(ms), "sourceopen");
        arm_pump();
    }
    return JS_NewInt32(ctx, rc);
}

/* Property ids. A single get/set pair beats forty exported functions, and the
 * shim's property table is the readable half. */
enum {
    P_CURRENTTIME = 0, P_DURATION, P_PAUSED, P_ENDED, P_SEEKING, P_VOLUME,
    P_MUTED, P_READYSTATE, P_NETWORKSTATE, P_VIDEOWIDTH, P_VIDEOHEIGHT,
    P_ERROR, P_ERRORMSG
};

static JSValue js_m_get(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int key = 0, prop = 0;
    if (argc < 2 || JS_ToInt32(ctx, &key, argv[0]) || JS_ToInt32(ctx, &prop, argv[1]))
        return JS_UNDEFINED;
    melem *el = el_of(key, 0);
    if (!el) return JS_UNDEFINED;
    switch (prop) {
    case P_CURRENTTIME: return JS_NewFloat64(ctx, mel_current_time(el));
    case P_DURATION: { double d = mel_duration(el);
                       return JS_NewFloat64(ctx, d < 0 ? (0.0 / 0.0) : d); }
    case P_PAUSED:      return JS_NewBool(ctx, mel_paused(el));
    case P_ENDED:       return JS_NewBool(ctx, mel_ended(el));
    case P_SEEKING:     return JS_NewBool(ctx, mel_seeking(el));
    case P_VOLUME:      return JS_NewFloat64(ctx, mel_volume(el));
    case P_MUTED:       return JS_NewBool(ctx, mel_muted(el));
    case P_READYSTATE:  return JS_NewInt32(ctx, mel_ready_state(el));
    case P_NETWORKSTATE:return JS_NewInt32(ctx, mel_network_state(el));
    case P_VIDEOWIDTH:  return JS_NewInt32(ctx, mel_video_width(el));
    case P_VIDEOHEIGHT: return JS_NewInt32(ctx, mel_video_height(el));
    case P_ERROR:       return JS_NewInt32(ctx, mel_error(el));
    case P_ERRORMSG:    return JS_NewString(ctx, mel_error_message(el));
    }
    return JS_UNDEFINED;
}

static JSValue js_m_set(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int key = 0, prop = 0;
    double v = 0;
    if (argc < 3 || JS_ToInt32(ctx, &key, argv[0]) || JS_ToInt32(ctx, &prop, argv[1]))
        return JS_UNDEFINED;
    melem *el = el_of(key, 0);
    if (!el) return JS_UNDEFINED;
    if (prop == P_MUTED) { mel_set_muted(el, JS_ToBool(ctx, argv[2])); return JS_UNDEFINED; }
    if (JS_ToFloat64(ctx, &v, argv[2])) return JS_EXCEPTION;
    switch (prop) {
    case P_CURRENTTIME: mel_seek(el, v); arm_pump(); break;
    case P_VOLUME:      mel_set_volume(el, v); break;
    }
    return JS_UNDEFINED;
}

enum { M_PLAY = 0, M_PAUSE, M_LOAD };

static JSValue js_m_call(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int key = 0, m = 0;
    if (argc < 2 || JS_ToInt32(ctx, &key, argv[0]) || JS_ToInt32(ctx, &m, argv[1]))
        return JS_UNDEFINED;
    melem *el = el_of(key, 0);
    if (!el) return JS_UNDEFINED;
    switch (m) {
    case M_PLAY:  mel_play(el); arm_pump(); break;
    case M_PAUSE: mel_pause(el); break;
    case M_LOAD:  mel_load(el); arm_pump(); break;
    }
    return JS_UNDEFINED;
}

static JSValue js_m_buffered(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int key = 0;
    if (argc < 1 || JS_ToInt32(ctx, &key, argv[0])) return JS_UNDEFINED;
    melem *el = el_of(key, 0);
    double s[MAXR], e[MAXR];
    int n = el ? mel_buffered_count(el) : 0;
    if (n > MAXR) n = MAXR;
    for (int i = 0; i < n; i++) mel_buffered_range(el, i, &s[i], &e[i]);
    return make_ranges(ctx, n, s, e);
}

/* The instrument. Everything a test reads back about a playing element, as one
 * object -- so "it played" is a set of numbers and not a screenshot. */
static JSValue js_m_stats(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    int key = 0;
    if (argc < 1 || JS_ToInt32(ctx, &key, argv[0])) return JS_UNDEFINED;
    melem *el = el_of(key, 0);
    if (!el) return JS_UNDEFINED;
    struct mel_stats st;
    mel_get_stats(el, &st);
    JSValue o = JS_NewObject(ctx);
#define N(k, v) JS_SetPropertyStr(ctx, o, k, JS_NewFloat64(ctx, (double)(v)))
    N("framesDecoded", st.frames_decoded);
    N("framesShown", st.frames_shown);
    N("framesDropped", st.frames_dropped);
    N("resyncs", st.resyncs);
    N("driftMeanMs", (double)st.drift_mean_ns / 1e6);
    N("driftMaxMs", (double)st.drift_max_ns / 1e6);
    N("driftMinMs", (double)st.drift_min_ns / 1e6);
    N("audioFrames", st.audio_frames_written);
    N("appends", st.appends);
    N("bytesAppended", st.bytes_appended);
    N("reparses", st.reparses);
#undef N
    return o;
}

/* ---- URL.createObjectURL ---- */
static JSValue js_create_object_url(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_ThrowTypeError(ctx, "createObjectURL needs an object");
    msource *ms = JS_GetOpaque(argv[0], g_ms_cid);
    if (!ms) return JS_ThrowTypeError(ctx,
        "createObjectURL: only a MediaSource is supported here (no Blob/File)");
    char url[64];
    if (!mse_object_url(ms, url, sizeof url))
        return JS_ThrowInternalError(ctx, "too many object URLs");
    return JS_NewString(ctx, url);
}
static JSValue js_revoke_object_url(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    const char *u = JS_ToCString(ctx, argv[0]);
    if (u) { mse_revoke_object_url(u); JS_FreeCString(ctx, u); }
    return JS_UNDEFINED;
}

/* ================================================= the pump ============= */
/* A playing element keeps itself alive with a setTimeout chain. The browser's
 * main loop already calls js_page_run_due() whenever timers are pending, so
 * this needs no change anywhere else -- which is the point.
 *
 * 16 ms, not 0: a zero-delay chain would spin the loop at whatever rate the
 * machine can manage and starve input. The clock decides when a frame is
 * actually shown; this only decides how often we look. */
static JSValue js_m_tick(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    g_pump_armed = 0;
    js_media_pump(ctx);
    if (js_media_pending()) arm_pump();
    return JS_UNDEFINED;
}

static void arm_pump(void)
{
    if (g_pump_armed || !g_ctx) return;
    JSValue g = JS_GetGlobalObject(g_ctx);
    JSValue st = JS_GetPropertyStr(g_ctx, g, "setTimeout");
    if (JS_IsFunction(g_ctx, st)) {
        JSValue fn = JS_NewCFunction(g_ctx, js_m_tick, "__mediaTick", 0);
        JSValueConst a[2] = { fn, JS_MKVAL(JS_TAG_INT, 16) };
        JSValue r = JS_Call(g_ctx, st, g, 2, a);
        if (!JS_IsException(r)) g_pump_armed = 1;
        else JS_FreeValue(g_ctx, JS_GetException(g_ctx));
        JS_FreeValue(g_ctx, r);
        JS_FreeValue(g_ctx, fn);
    }
    JS_FreeValue(g_ctx, st);
    JS_FreeValue(g_ctx, g);
}

int js_media_pending(void)
{
    if (mel_pending()) return 1;
    for (int i = 0; i < MAXWRAP; i++) if (g_sbwrap[i].pending_end) return 1;
    return 0;
}

int js_media_pump(JSContext *ctx)
{
    if (!ctx) return 0;
    int did = 0;

    /* The asynchronous half of appendBuffer: updating goes false and the pair
     * of events fires HERE, from the event loop, so a player's updateend
     * handler appending the next segment does not recurse into this one. */
    for (int i = 0; i < MAXWRAP; i++) {
        if (!g_sbwrap[i].sb || !g_sbwrap[i].pending_end) continue;
        g_sbwrap[i].pending_end = 0;
        JSValue o = g_sbwrap[i].obj;
        JS_SetPropertyStr(ctx, o, "__updating", JS_FALSE);
        fire(o, "update");
        fire(o, "updateend");
        did++;
    }

    did += media_pump();

    /* Element events. The engine decided they happened; this turns them into
     * DOM events on the element, through js_dom.c's real dispatch so a
     * listener registered with addEventListener is called the same way a click
     * handler is. */
    for (int i = 0; i < MAXWRAP; i++) {
        melem *el = mel_at(i);
        if (!el) continue;
        unsigned ev = mel_take_events(el);
        if (!ev) continue;
        struct { unsigned bit; const char *name; } tab[] = {
            { MEV_LOADEDMETADATA, "loadedmetadata" },
            { MEV_DURATIONCHANGE, "durationchange" },
            { MEV_CANPLAY,        "canplay" },
            { MEV_PLAYING,        "playing" },
            { MEV_TIMEUPDATE,     "timeupdate" },
            { MEV_WAITING,        "waiting" },
            { MEV_SEEKED,         "seeked" },
            { MEV_ENDED,          "ended" },
            { MEV_ERROR,          "error" },
        };
        for (unsigned k = 0; k < sizeof tab / sizeof tab[0]; k++) {
            if (!(ev & tab[k].bit)) continue;
            for (int w = 0; w < MAXWRAP; w++) {
                if (!g_elwrap[w].key) continue;
                JSValue f = JS_GetPropertyStr(ctx, g_elwrap[w].obj, "__mediaFire");
                if (JS_IsFunction(ctx, f)) {
                    JSValue a = JS_NewString(ctx, tab[k].name);
                    JSValue r = JS_Call(ctx, f, g_elwrap[w].obj, 1, (JSValueConst *)&a);
                    if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
                    JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, a);
                }
                JS_FreeValue(ctx, f);
            }
            did++;
        }
    }
    js_dom_run_jobs(ctx);
    return did;
}

/* ================================================== the shim ============ */
/* Plumbing, in the language it is plumbing in. See the file header. */
static const char g_shim[] =
"(function (G) {\n"
"var def = function (o, n, v) {\n"
"  try { Object.defineProperty(o, n, { value: v, writable: true, configurable: true }); }\n"
"  catch (e) { try { o[n] = v; } catch (e2) {} }\n"
"};\n"
"var acc = function (o, n, g, s) {\n"
"  try { Object.defineProperty(o, n, { get: g, set: s, configurable: true }); } catch (e) {}\n"
"};\n"
/* --- TimeRanges --------------------------------------------------------- */
"G.__mkTimeRanges = function (starts, ends) {\n"
"  var r = Object.create(null);\n"
"  r = {};\n"
"  Object.defineProperty(r, 'length', { value: starts.length, configurable: true });\n"
"  r.start = function (i) { if (i < 0 || i >= starts.length) throw new RangeError('index'); return starts[i]; };\n"
"  r.end = function (i) { if (i < 0 || i >= ends.length) throw new RangeError('index'); return ends[i]; };\n"
"  return r;\n"
"};\n"
/* --- the EventTarget mixin the two MSE objects need ---------------------- */
"var mixTarget = function (o, onprops) {\n"
"  var L = {};\n"
"  def(o, 'addEventListener', function (t, f) { if (!f) return;\n"
"    (L[t] || (L[t] = [])).push(f); });\n"
"  def(o, 'removeEventListener', function (t, f) { var a = L[t]; if (!a) return;\n"
"    for (var i = 0; i < a.length; i++) if (a[i] === f) { a.splice(i, 1); return; } });\n"
"  def(o, '__fire', function (t) {\n"
"    var ev = { type: t, target: o, currentTarget: o, timeStamp: Date.now() };\n"
"    var h = o['on' + t];\n"
"    if (typeof h === 'function') { try { h.call(o, ev); } catch (e) { G.__mediaLog(t, e); } }\n"
"    var a = L[t]; if (!a) return;\n"
"    a = a.slice();\n"
"    for (var i = 0; i < a.length; i++) {\n"
"      try { (typeof a[i] === 'function' ? a[i] : a[i].handleEvent).call(o, ev); }\n"
"      catch (e) { G.__mediaLog(t, e); }\n"
"    }\n"
"  });\n"
"  def(o, 'dispatchEvent', function (ev) { o.__fire(ev && ev.type); return true; });\n"
"  for (var i = 0; i < onprops.length; i++) def(o, 'on' + onprops[i], null);\n"
"};\n"
"G.__mediaLog = function (t, e) { try { console.error('media ' + t + ' handler: ' + e); } catch (x) {} };\n"
/* --- MediaSource: the parts that are bookkeeping ------------------------- */
"var MS = G.MediaSource;\n"
"if (!MS) return;\n"
"var msInit = function (ms) {\n"
"  mixTarget(ms, ['sourceopen', 'sourceended', 'sourceclose']);\n"
"  def(ms, '__initSB', function (sb) {\n"
"    mixTarget(sb, ['updatestart', 'update', 'updateend', 'error', 'abort']);\n"
"    def(sb, '__updating', false);\n"
"    def(sb, 'appendWindowStart', 0);\n"
"    def(sb, 'appendWindowEnd', Infinity);\n"
"    ms.sourceBuffers[ms.sourceBuffers.length] = sb;\n"
"    ms.activeSourceBuffers[ms.activeSourceBuffers.length] = sb;\n"
"  });\n"
"};\n"
"var RealMS = MS;\n"
"G.MediaSource = function MediaSource() {\n"
"  var ms = new RealMS();\n"
"  msInit(ms);\n"
"  return ms;\n"
"};\n"
"G.MediaSource.prototype = RealMS.prototype;\n"
"G.MediaSource.isTypeSupported = RealMS.isTypeSupported;\n"
"G.SourceBuffer = G.SourceBuffer || function SourceBuffer() {\n"
"  throw new TypeError('SourceBuffer is not constructible; use addSourceBuffer');\n"
"};\n"
/* --- URL.createObjectURL ------------------------------------------------- */
"if (!G.URL) G.URL = {};\n"
"if (!G.URL.createObjectURL) def(G.URL, 'createObjectURL', G.__createObjectURL);\n"
"if (!G.URL.revokeObjectURL) def(G.URL, 'revokeObjectURL', G.__revokeObjectURL);\n"
/* --- HTMLMediaElement ---------------------------------------------------- */
/* Installed on the ELEMENT PROTOTYPE, because js_dom.c has one Element class
 * and therefore one prototype. Every member is guarded on the tag name, so a
 * <div> that is asked to play says what a <div> should say rather than half
 * doing it. */
"var doc = G.document;\n"
"if (!doc || typeof doc.createElement !== 'function') return;\n"
"var proto = Object.getPrototypeOf(doc.createElement('video'));\n"
"if (!proto || proto.__mediaInstalled) return;\n"
"def(proto, '__mediaInstalled', true);\n"
"var nextKey = 0;\n"
"var isMedia = function (el) {\n"
"  var t = el && el.tagName;\n"
"  return t === 'VIDEO' || t === 'AUDIO' || t === 'video' || t === 'audio';\n"
"};\n"
"var keyOf = function (el, make) {\n"
"  if (!isMedia(el)) throw new TypeError('not an HTMLMediaElement');\n"
"  var k = el.getAttribute('data-logit-mediaid');\n"
"  if (k) return parseInt(k, 10);\n"
"  if (!make) return 0;\n"
"  k = ++nextKey;\n"
"  el.setAttribute('data-logit-mediaid', String(k));\n"
"  G.__mediaBind(k, el);\n"
"  mixTarget(el, []);\n"
"  def(el, '__mediaFire', function (t) {\n"
"    var ev = { type: t, target: el, currentTarget: el, timeStamp: Date.now() };\n"
"    var h = el['on' + t];\n"
"    if (typeof h === 'function') { try { h.call(el, ev); } catch (e) { G.__mediaLog(t, e); } }\n"
"    try { el.dispatchEvent && el.__domFire ? el.__domFire(t) : 0; } catch (e) {}\n"
"    if (el.__fire) el.__fire(t);\n"
"  });\n"
"  return k;\n"
"};\n"
"var P = { currentTime: 0, duration: 1, paused: 2, ended: 3, seeking: 4,\n"
"          volume: 5, muted: 6, readyState: 7, networkState: 8,\n"
"          videoWidth: 9, videoHeight: 10 };\n"
"var ro = ['duration', 'paused', 'ended', 'seeking', 'readyState', 'networkState',\n"
"          'videoWidth', 'videoHeight'];\n"
"for (var i = 0; i < ro.length; i++) (function (n) {\n"
"  acc(proto, n, function () { return G.__mediaGet(keyOf(this, true), P[n]); });\n"
"})(ro[i]);\n"
"var rw = ['currentTime', 'volume', 'muted'];\n"
"for (var j = 0; j < rw.length; j++) (function (n) {\n"
"  acc(proto, n, function () { return G.__mediaGet(keyOf(this, true), P[n]); },\n"
"               function (v) { G.__mediaSet(keyOf(this, true), P[n], v); });\n"
"})(rw[j]);\n"
"acc(proto, 'buffered', function () { return G.__mediaBuffered(keyOf(this, true)); });\n"
"acc(proto, 'error', function () {\n"
"  var c = G.__mediaGet(keyOf(this, true), 11);\n"
"  if (!c) return null;\n"
"  return { code: c, message: G.__mediaGet(keyOf(this, true), 12) };\n"
"});\n"
"acc(proto, 'srcObject', function () { return this.__srcObject || null; },\n"
"                       function (v) { this.__srcObject = v;\n"
"                                      G.__mediaSrc(keyOf(this, true), v); });\n"
"acc(proto, 'src', function () { return this.getAttribute('src') || ''; },\n"
"                  function (v) { this.setAttribute('src', v);\n"
"                                 if (isMedia(this)) G.__mediaSrc(keyOf(this, true), v); });\n"
"acc(proto, 'currentSrc', function () { return this.getAttribute('src') || ''; });\n"
"acc(proto, 'playbackRate', function () { return 1; }, function () {});\n"
"def(proto, 'play', function () {\n"
"  G.__mediaCall(keyOf(this, true), 0);\n"
"  var el = this;\n"
"  if (typeof el.__mediaFire === 'function') el.__mediaFire('play');\n"
"  return Promise.resolve();\n"
"});\n"
"def(proto, 'pause', function () { G.__mediaCall(keyOf(this, true), 1);\n"
"  if (typeof this.__mediaFire === 'function') this.__mediaFire('pause'); });\n"
"def(proto, 'load', function () { G.__mediaCall(keyOf(this, true), 2); });\n"
"def(proto, 'canPlayType', function (t) {\n"
"  return G.MediaSource.isTypeSupported(t) ? 'probably' : '';\n"
"});\n"
"def(proto, '__mediaStats', function () { return G.__mediaStats(keyOf(this, true)); });\n"
"def(proto, 'HAVE_NOTHING', 0); def(proto, 'HAVE_METADATA', 1);\n"
"def(proto, 'HAVE_CURRENT_DATA', 2); def(proto, 'HAVE_FUTURE_DATA', 3);\n"
"def(proto, 'HAVE_ENOUGH_DATA', 4);\n"
"def(proto, 'NETWORK_EMPTY', 0); def(proto, 'NETWORK_IDLE', 1);\n"
"def(proto, 'NETWORK_LOADING', 2); def(proto, 'NETWORK_NO_SOURCE', 3);\n"
"})(globalThis);\n";

/* ================================================== install ============= */
void js_media_install(JSContext *ctx)
{
    if (!ctx) return;
    g_ctx = ctx;
    media_set_platform(&g_osplat);

    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!g_ms_cid) {
        JS_NewClassID(&g_ms_cid);
        JS_NewClassID(&g_sb_cid);
    }
    JS_NewClass(rt, g_ms_cid, &g_ms_class);
    JS_NewClass(rt, g_sb_cid, &g_sb_class);

    JSValue g = JS_GetGlobalObject(ctx);

    JSValue msproto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, msproto, g_ms_proto,
                               (int)(sizeof g_ms_proto / sizeof g_ms_proto[0]));
    JS_SetClassProto(ctx, g_ms_cid, JS_DupValue(ctx, msproto));

    JSValue ctor = JS_NewCFunction2(ctx, js_ms_ctor, "MediaSource", 0,
                                    JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, msproto);
    JS_SetPropertyStr(ctx, ctor, "isTypeSupported",
                      JS_NewCFunction(ctx, js_ms_isTypeSupported, "isTypeSupported", 1));
    JS_SetPropertyStr(ctx, g, "MediaSource", ctor);
    JS_FreeValue(ctx, msproto);

    JSValue sbproto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, sbproto, g_sb_proto,
                               (int)(sizeof g_sb_proto / sizeof g_sb_proto[0]));
    JS_SetClassProto(ctx, g_sb_cid, sbproto);

    /* The natives the shim binds to. Deliberately double-underscored and
     * non-enumerable-ish: a page has no business calling them. */
    JS_SetPropertyStr(ctx, g, "__mediaBind", JS_NewCFunction(ctx, js_m_bind, "__mediaBind", 2));
    JS_SetPropertyStr(ctx, g, "__mediaSrc", JS_NewCFunction(ctx, js_m_src, "__mediaSrc", 2));
    JS_SetPropertyStr(ctx, g, "__mediaGet", JS_NewCFunction(ctx, js_m_get, "__mediaGet", 2));
    JS_SetPropertyStr(ctx, g, "__mediaSet", JS_NewCFunction(ctx, js_m_set, "__mediaSet", 3));
    JS_SetPropertyStr(ctx, g, "__mediaCall", JS_NewCFunction(ctx, js_m_call, "__mediaCall", 2));
    JS_SetPropertyStr(ctx, g, "__mediaBuffered",
                      JS_NewCFunction(ctx, js_m_buffered, "__mediaBuffered", 1));
    JS_SetPropertyStr(ctx, g, "__mediaStats",
                      JS_NewCFunction(ctx, js_m_stats, "__mediaStats", 1));
    JS_SetPropertyStr(ctx, g, "__createObjectURL",
                      JS_NewCFunction(ctx, js_create_object_url, "createObjectURL", 1));
    JS_SetPropertyStr(ctx, g, "__revokeObjectURL",
                      JS_NewCFunction(ctx, js_revoke_object_url, "revokeObjectURL", 1));
    JS_FreeValue(ctx, g);

    JSValue r = JS_Eval(ctx, g_shim, sizeof g_shim - 1, "<media>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("js_media: shim failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
}

void js_media_close(JSContext *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < MAXWRAP; i++) {
        if (g_mswrap[i].ms) { JS_FreeValue(ctx, g_mswrap[i].obj); g_mswrap[i].ms = 0; }
        if (g_sbwrap[i].sb) { JS_FreeValue(ctx, g_sbwrap[i].obj); g_sbwrap[i].sb = 0; }
        if (g_elwrap[i].key) { JS_FreeValue(ctx, g_elwrap[i].obj); g_elwrap[i].key = 0; }
        g_mswrap[i].obj = g_sbwrap[i].obj = g_elwrap[i].obj = JS_UNDEFINED;
    }
    /* The DOM is about to be freed and every decoder holds megabytes of
     * reference frames; releasing them here rather than at the next page's
     * first append is the difference between a browser you can navigate in and
     * one that grows by a film per page. */
    mel_free_all();
    g_pump_armed = 0;
    g_ctx = 0;
}
