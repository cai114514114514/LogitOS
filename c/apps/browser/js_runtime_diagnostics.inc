/* Included only by a JS_RUNTIME_DIAGNOSTICS build. The C function is a private
 * prelude argument, not a page global. Native Error prototype identity avoids
 * reading name/stack/constructor getters (and Proxy traps) on thrown values.
 * Only direct builtin Error subclasses are named; altered/custom prototypes
 * are conservatively Error. JSON is reparsed with the native parser, bounded
 * to 64 KiB UTF-8, then only own data properties are examined: a polluted
 * Object.prototype must neither run code nor supply a fake business code. */
#define XRD_MAX_LINES 256
static unsigned xrd_lines;
static const char *const xrd_errors[] = {
    "Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError",
    "TypeError", "URIError", "InternalError", "AggregateError"
};
#define XRD_ERRORS ((int)(sizeof xrd_errors / sizeof xrd_errors[0]))
static int xrd_int(JSValueConst v)
{ return JS_VALUE_GET_TAG(v) == JS_TAG_INT ? JS_VALUE_GET_INT(v) : -1; }
static long long xrd_number(JSContext *ctx, JSValueConst v)
{
    double n = 0;
    if (JS_IsNumber(v)) JS_ToFloat64(ctx, &n, v);
    return n >= 0 && n <= 9007199254740991.0 ? (long long)n : 0;
}
static const char *xrd_event(int n)
{
    static const char *const names[] = { "other", "readystatechange", "progress", "load", "error", "abort", "loadend" };
    return n >= 0 && n < 7 ? names[n] : names[0];
}
static const char *xrd_error_type(JSContext *ctx, JSValueConst v, JSValue *protos)
{
    if (JS_IsError(ctx, v)) {
        /* JS_IsError tests the native class id, so GetPrototype's receiver
         * cannot be a Proxy. No traversal into arbitrary prototypes occurs. */
        JSValue p = JS_GetPrototype(ctx, v);
        const char *name = "Error";
        for (int i = 0; i < XRD_ERRORS; i++)
            if (JS_IsObject(p) && JS_IsObject(protos[i]) &&
                JS_VALUE_GET_PTR(p) == JS_VALUE_GET_PTR(protos[i])) { name = xrd_errors[i]; break; }
        JS_FreeValue(ctx, p);
        return name;
    }
    if (JS_IsObject(v)) return "object";
    if (JS_IsString(v)) return "string";
    if (JS_IsNumber(v)) return "number";
    if (JS_IsBool(v)) return "boolean";
    if (JS_IsNull(v)) return "null";
    if (JS_IsUndefined(v)) return "undefined";
    return "other";
}
/* Call only on values freshly produced by JS_ParseJSON, never page objects. */
static JSValue xrd_json_own(JSContext *ctx, JSValueConst o, const char *key, int *found)
{
    *found = 0;
    if (!JS_IsObject(o)) return JS_UNDEFINED;
    JSAtom atom = JS_NewAtom(ctx, key);
    JSPropertyDescriptor d;
    int rc = JS_GetOwnProperty(ctx, &d, o, atom);
    JS_FreeAtom(ctx, atom);
    if (rc <= 0) { if (rc < 0) JS_FreeValue(ctx, JS_GetException(ctx)); return JS_UNDEFINED; }
    *found = 1;
    JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);
    return d.value;
}
static void xrd_json_code(JSContext *ctx, JSValueConst v, int found, char out[64])
{
    if (!found) { strcpy(out, "missing"); return; }
    double n;
    if (JS_IsNumber(v) && JS_ToFloat64(ctx, &n, v) == 0 &&
        n >= -9007199254740991.0 && n <= 9007199254740991.0 && (double)(long long)n == n)
        snprintf(out, 64, "int:%lld", (long long)n);
    else strcpy(out, "non_integer");
}
static void xrd_json(JSContext *ctx, int id, int fetch, JSValueConst body)
{
    if (!JS_IsString(body) || xrd_lines >= XRD_MAX_LINES) return;
    size_t n = 0;
    const char *bytes = JS_ToCStringLen(ctx, &n, body);
    if (!bytes) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    if (n > 65536) { JS_FreeCString(ctx, bytes); return; }
    JSValue o = JS_ParseJSON(ctx, bytes, n, "<xhr-diagnostic>");
    JS_FreeCString(ctx, bytes);
    if (JS_IsException(o)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        xrd_lines++;
        printf("[runtime-diag] xhr id=%d fetch=%d phase=json parse=invalid\n", id, fetch);
        return;
    }
    int found, data_found;
    JSValue code = xrd_json_own(ctx, o, "code", &found);
    char c[64], b[64]; xrd_json_code(ctx, code, found, c);
    JSValue data = xrd_json_own(ctx, o, "data", &data_found);
    JSValue biz = xrd_json_own(ctx, data, "biz_code", &found);
    xrd_json_code(ctx, biz, data_found && found, b);
    JS_FreeValue(ctx, biz); JS_FreeValue(ctx, data); JS_FreeValue(ctx, code); JS_FreeValue(ctx, o);
    xrd_lines++;
    printf("[runtime-diag] xhr id=%d fetch=%d phase=json parse=ok code=%s biz_code=%s\n", id, fetch, c, b);
}
/* A Response.json() consumer chose to parse this 4xx body. Classification is
 * deliberately narrower than a body dump: only six exact validation types,
 * the first location category, the detail shape and counts can reach printf.
 * All inspected values come from a fresh native parse, and own descriptors
 * keep polluted prototypes from supplying fields or running accessors. Array
 * entries are aggregated into at most 6*5 fixed buckets rather than logged
 * individually; neither message text nor later location/field names is read. */
static const char *const xrd_validation_types[] = {
    "missing", "string_type", "string_pattern_mismatch", "json_invalid", "value_error", "other"
};
static const char *const xrd_validation_locs[] = { "body", "query", "path", "header", "other" };
static int xrd_enum(JSContext *ctx, JSValueConst value, const char *const *names, int count)
{
    if (!JS_IsString(value)) return count-1;
    size_t n=0;const char *s=JS_ToCStringLen(ctx,&n,value);
    if (!s) { JS_FreeValue(ctx,JS_GetException(ctx)); return count-1; }
    int out=count-1;
    for (int i=0;i<count-1;i++) if (n==strlen(names[i])&&!memcmp(s,names[i],n)) { out=i;break; }
    JS_FreeCString(ctx,s);return out;
}
static void xrd_validation_row(JSContext *ctx, JSValueConst entry, unsigned counts[6][5])
{
    int found;
    JSValue type=xrd_json_own(ctx,entry,"type",&found);
    int ti=xrd_enum(ctx,type,xrd_validation_types,6);JS_FreeValue(ctx,type);
    JSValue loc=xrd_json_own(ctx,entry,"loc",&found);
    JSValue first=JS_IsArray(ctx,loc)?xrd_json_own(ctx,loc,"0",&found):JS_UNDEFINED;
    int li=xrd_enum(ctx,first,xrd_validation_locs,5);
    JS_FreeValue(ctx,first);JS_FreeValue(ctx,loc);counts[ti][li]++;
}
static void xrd_validation_print(int fetch,int status,const char *kind,unsigned total,
                                 int type,int loc,unsigned count)
{
    if (xrd_lines>=XRD_MAX_LINES)return;
    xrd_lines++;
    printf("[runtime-diag] fetch-json fetch=%d status=%d detail_kind=%s detail_count=%u type=%s loc=%s count=%u\n",
           fetch,status,kind,total,xrd_validation_types[type],xrd_validation_locs[loc],count);
}
static void xrd_response_json(JSContext *ctx,int fetch,int status,JSValueConst body)
{
    if (status<400||status>=500||!JS_IsString(body)||xrd_lines>=XRD_MAX_LINES)return;
    size_t n=0;const char *bytes=JS_ToCStringLen(ctx,&n,body);
    if (!bytes) { JS_FreeValue(ctx,JS_GetException(ctx));return; }
    if (n>65536) { JS_FreeCString(ctx,bytes);return; }
    JSValue parsed=JS_ParseJSON(ctx,bytes,n,"<response-diagnostic>");
    JS_FreeCString(ctx,bytes);
    if (JS_IsException(parsed)) {
        JS_FreeValue(ctx,JS_GetException(ctx));
        xrd_validation_print(fetch,status,"invalid_json",0,3,4,1);return;
    }
    int found;JSValue detail=xrd_json_own(ctx,parsed,"detail",&found);
    const char *kind="missing";unsigned total=0,counts[6][5]={{0}};
    if (found && JS_IsArray(ctx,detail)) {
        kind="array";
        JSValue length=xrd_json_own(ctx,detail,"length",&found);
        int len=xrd_int(length);JS_FreeValue(ctx,length);
        if (len>0)total=(unsigned)len;
        for(unsigned i=0;i<total;i++){
            char key[16];snprintf(key,sizeof key,"%u",i);
            JSValue entry=xrd_json_own(ctx,detail,key,&found);
            xrd_validation_row(ctx,entry,counts);JS_FreeValue(ctx,entry);
        }
    } else if (found) {
        kind=JS_IsNull(detail)?"null":JS_IsString(detail)?"string":JS_IsNumber(detail)?"number":JS_IsBool(detail)?"boolean":JS_IsObject(detail)?"object":"other";
        if (!JS_IsNull(detail)) {total=1;xrd_validation_row(ctx,detail,counts);}
    }
    JS_FreeValue(ctx,detail);JS_FreeValue(ctx,parsed);
    if (!total)xrd_validation_print(fetch,status,kind,0,5,4,0);
    else for(int t=0;t<6;t++)for(int l=0;l<5;l++)if(counts[t][l])
        xrd_validation_print(fetch,status,kind,total,t,l,counts[t][l]);
}
static JSValue js_xhr_diag(JSContext *ctx, JSValueConst self, int argc,
                           JSValueConst *v, int magic, JSValue *protos)
{
    (void)self; (void)magic;
    if (argc < 3 || xrd_lines >= XRD_MAX_LINES) return JS_UNDEFINED;
    int phase = xrd_int(v[0]), id = xrd_int(v[1]), fetch = xrd_int(v[2]);
    if (phase == 5 && argc >= 5) {
        xrd_response_json(ctx,fetch,xrd_int(v[3]),v[4]);return JS_UNDEFINED;
    }
    if (id < 1 || phase < 0 || phase > 4) return JS_UNDEFINED;
    xrd_lines++;
    if (phase == 0 && argc >= 5)
        printf("[runtime-diag] xhr id=%d fetch=%d phase=headers status=%d mime=%s\n", id, fetch, xrd_int(v[3]), xrd_int(v[4]) == 1 ? "json" : "other");
    else if (phase == 1 && argc >= 5)
        printf("[runtime-diag] xhr id=%d fetch=%d phase=callback-error event=%s type=%s\n", id, fetch, xrd_event(xrd_int(v[3])), xrd_error_type(ctx, v[4], protos));
    else if (phase == 2 && argc >= 4)
        printf("[runtime-diag] xhr id=%d fetch=%d phase=fetch-error type=%s\n", id, fetch, xrd_error_type(ctx, v[3], protos));
    else if (phase == 3 && argc >= 7) {
        printf("[runtime-diag] xhr id=%d fetch=%d phase=body-end received=%lld chars=%lld mime=%s\n", id, fetch, xrd_number(ctx, v[3]), xrd_number(ctx, v[4]), xrd_int(v[5]) == 1 ? "json" : "other");
        if (xrd_int(v[5]) == 1) xrd_json(ctx, id, fetch, v[6]);
    } else if (phase == 4 && argc >= 9)
        printf("[runtime-diag] xhr id=%d fetch=%d phase=terminal progress=%lld load=%lld error=%lld abort=%lld received=%lld chars=%lld\n", id, fetch, xrd_number(ctx, v[3]), xrd_number(ctx, v[4]), xrd_number(ctx, v[5]), xrd_number(ctx, v[6]), xrd_number(ctx, v[7]), xrd_number(ctx, v[8]));
    return JS_UNDEFINED;
}
static JSValue make_xhr_diag_hook(JSContext *ctx)
{
    /* Install runs before page scripts; retain pristine prototype identities
     * in the private native closure, never read a page's constructors later. */
    JSValue g = JS_GetGlobalObject(ctx), p[XRD_ERRORS];
    for (int i = 0; i < XRD_ERRORS; i++) {
        JSValue ctor = JS_GetPropertyStr(ctx, g, xrd_errors[i]);
        p[i] = JS_IsFunction(ctx, ctor) ? JS_GetPropertyStr(ctx, ctor, "prototype") : JS_UNDEFINED;
        JS_FreeValue(ctx, ctor);
    }
    JS_FreeValue(ctx, g);
    JSValue fn = JS_NewCFunctionData(ctx, js_xhr_diag, 3, 0, XRD_ERRORS, p);
    for (int i = 0; i < XRD_ERRORS; i++) JS_FreeValue(ctx, p[i]);
    xrd_lines = 0;
    return fn;
}
