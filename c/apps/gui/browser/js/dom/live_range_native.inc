/* The JS object owns the subscriber, not vice versa. Keeping JS values in the
 * document's range registry would pin every short-lived cloneRange until
 * navigation and leak an editor's selection history. */
static JSClassID range_state_cid;
static void range_state_finalizer(JSRuntime *rt, JSValue val)
{
    struct dom_live_range *r = JS_GetOpaque(val, range_state_cid);
    if (r) { dom_range_dispose(r); js_free_rt(rt, r); }
}
static JSValue jf_range_new(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    JSValue v = JS_NewObjectClass(ctx, range_state_cid);
    if (JS_IsException(v)) return v;
    struct dom_live_range *r = js_mallocz(ctx, sizeof *r);
    if (!r) { JS_FreeValue(ctx, v); return JS_EXCEPTION; }
    struct node *root = js_dom_root();
    dom_range_init(r, root ? root->doc : 0);
    JS_SetOpaque(v, r); return v;
}
static JSValue jf_range_get(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int magic)
{
    (void)t;
    struct dom_live_range *r = argc ? JS_GetOpaque2(ctx, argv[0], range_state_cid) : 0;
    if (!r) return JS_EXCEPTION;
    if (!r->start || !r->end) return js_dom_throw_dom(ctx, "InvalidStateError", "Range document or node was destroyed");
    if (magic == 0) return js_dom_node_value(ctx, r->start);
    if (magic == 1) return JS_NewUint32(ctx, r->start_offset);
    if (magic == 2) return js_dom_node_value(ctx, r->end);
    return JS_NewUint32(ctx, r->end_offset);
}
static JSValue jf_range_set(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int magic)
{
    (void)t;
    if (argc < 3) return JS_ThrowTypeError(ctx, "Range boundary requires node and offset");
    struct dom_live_range *r = JS_GetOpaque2(ctx, argv[0], range_state_cid);
    if (!r) return JS_EXCEPTION;
    struct node *n = js_dom_node_from(argv[1]);
    if (!n) return JS_ThrowTypeError(ctx, "Range boundary requires native Node");
    uint32_t off;
    if (JS_ToUint32(ctx, &off, argv[2])) return JS_EXCEPTION;
    if (n->type == N_DOCTYPE) return js_dom_throw_dom(ctx, "InvalidNodeTypeError", "Doctype cannot contain a boundary");
    int ok = dom_range_set(r, magic, n, off);
    if (ok < 0) return js_dom_throw_dom(ctx, "WrongDocumentError", "Range cannot address another document");
    if (!ok) return js_dom_throw_dom(ctx, "IndexSizeError", "Range offset exceeds node length");
    return JS_UNDEFINED;
}
static JSValue jf_range_compare(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 4) return JS_ThrowTypeError(ctx, "boundary comparison requires four arguments");
    struct node *a = js_dom_node_from(argv[0]), *b = js_dom_node_from(argv[2]);
    uint32_t ao, bo;
    if (!a || !b) return JS_ThrowTypeError(ctx, "boundary comparison requires native Nodes");
    if (JS_ToUint32(ctx, &ao, argv[1]) || JS_ToUint32(ctx, &bo, argv[3])) return JS_EXCEPTION;
    int result = dom_point_compare(a, ao, b, bo);
    if (result == 2) return js_dom_throw_dom(ctx, "WrongDocumentError", "Boundary points have different tree roots");
    return JS_NewInt32(ctx, result);
}
static void range_native_install(JSContext *ctx)
{
    if (!range_state_cid) JS_NewClassID(&range_state_cid);
    JSClassDef def = { .class_name = "LiveRangeState", .finalizer = range_state_finalizer };
    JS_NewClass(JS_GetRuntime(ctx), range_state_cid, &def);
}
