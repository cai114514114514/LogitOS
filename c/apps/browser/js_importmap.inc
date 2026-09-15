/* Import-map foundation, HTML's module-specifier-resolution algorithm:
 * https://html.spec.whatwg.org/multipage/webappapis.html#import-maps
 *
 * Correction to js_module.c's old "bare specifiers are refused" claim:
 * unmapped bare names still fail, but imports/scopes now resolve them before
 * both prefetch and linking. Existing keys win when maps merge; a late map
 * cannot change an already resolved (referrer, specifier) pair. Null entries
 * BLOCK resolution, including fallback to a less-specific scope.
 *
 * Deliberate first-stage boundaries: the transport resolver only accepts
 * HTTP(S); integrity maps are rejected, not ignored; the parser-first loader
 * still lacks HTML's streaming parser execution order. Limits are explicit:
 * 512 entries, 2048 resolved pairs, 1 MiB JSON, 767-byte URLs. These are memory
 * budgets, not compatibility measurements. A limit fails loudly rather than
 * silently changing a graph after an earlier import already ran. */
#define IM_ENTRIES 512
#define IM_RESOLVED 2048
struct im_entry { char *scope, *key, *address; struct im_entry *next; };
struct im_seen { char *base, *name; struct im_seen *next; };
static struct im_entry *im_entries;
static struct im_seen *im_seen;
static int im_count, im_seen_count;
struct im_inline { char *name, *base; struct im_inline *next; };
static struct im_inline *im_inlines;
static int im_inline_count;

static char *im_copy(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n); if (p) memcpy(p, s, n); return p;
}
static void im_free_entries(struct im_entry *p)
{
    while (p) { struct im_entry *n = p->next;
        free(p->scope); free(p->key); free(p->address); free(p); p = n; }
}
static void im_clear(void)
{
    im_free_entries(im_entries); im_entries = NULL; im_count = 0;
    while (im_seen) { struct im_seen *n = im_seen->next;
        free(im_seen->base); free(im_seen->name); free(im_seen); im_seen = n; }
    im_seen_count = 0;
    while (im_inlines) { struct im_inline *n = im_inlines->next;
        free(im_inlines->name); free(im_inlines->base); free(im_inlines); im_inlines = n; }
    im_inline_count = 0;
}
static const char *im_base(const char *name)
{
    for (struct im_inline *p = im_inlines; name && p; p = p->next)
        if (!strcmp(name, p->name)) return p->base;
    return name;
}
int js_module_inline_referrer(const char *name, const char *base)
{
    if (!name || !base) return 0;
    for (struct im_inline *p = im_inlines; p; p = p->next)
        if (!strcmp(name, p->name)) return !strcmp(base, p->base);
    if (im_inline_count >= IM_ENTRIES) {
        printf("[js] inline script identity limit (512)\n"); return 0;
    }
    struct im_inline *p = calloc(1, sizeof *p);
    if (!p) return 0;
    p->name = im_copy(name); p->base = im_copy(base);
    if (!p->name || !p->base) { free(p->name); free(p->base); free(p); return 0; }
    p->next = im_inlines; im_inlines = p; im_inline_count++; return 1;
}
static int im_prefix(const char *prefix, const char *s)
{ return strncmp(prefix, s, strlen(prefix)) == 0; }
static int im_match(const char *key, const char *name)
{
    size_t n = strlen(key);
    return !strcmp(key, name) || (n && key[n-1] == '/' && im_prefix(key, name));
}
static int im_scope(const char *scope, const char *base)
{ return !scope[0] || im_match(scope, base); }
static int im_url(const char *base, const char *ref, char *out, int max)
{
    /* bfetch's relative-path helper does not itself distinguish an unknown
     * absolute scheme from a relative path. Import-map data must not turn
     * data:/blob:/file: into a plausible HTTP path under the document. */
    const char *p = ref;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.') p++;
    if (*p == ':' && strncmp(ref, "http://", 7) && strncmp(ref, "https://", 8)) return -1;
    if ((int)strlen(ref) >= max) return -1;
    return bfetch_resolve(base, ref, out, max);
}

static int im_remember(JSContext *ctx, const char *base, const char *name)
{
    if (!base) base = "";
    for (struct im_seen *p = im_seen; p; p = p->next)
        if (!strcmp(base, p->base) && !strcmp(name, p->name)) return 1;
    if (im_seen_count >= IM_RESOLVED) {
        JS_ThrowTypeError(ctx, "module resolution history limit (2048 pairs)"); return 0;
    }
    struct im_seen *p = calloc(1, sizeof *p);
    if (!p) { JS_ThrowOutOfMemory(ctx); return 0; }
    p->base = im_copy(base); p->name = im_copy(name);
    if (!p->base || !p->name) {
        free(p->base); free(p->name); free(p); JS_ThrowOutOfMemory(ctx); return 0;
    }
    p->next = im_seen; im_seen = p; im_seen_count++; return 1;
}

/* Return 1 for a mapped address, 0 for no match, -1 with a pending exception
 * for a blocked or invalid resolution. Choose scopes first, keys second:
 * a short key in a specific scope beats a longer key in global imports. */
static int im_resolve(JSContext *ctx, const char *base, const char *name,
                      char *out, int max)
{
#ifdef JS_MODULE_NO_IMPORTMAP
    (void)ctx; (void)base; (void)name; (void)out; (void)max; return 0;
#else
    char normalized[MOD_URLMAX];
    const char *spec = name;
    if (!is_bare_specifier(name)) {
        if (im_url(base, name, normalized, sizeof normalized) != 0) {
            JS_ThrowTypeError(ctx, "invalid module URL '%s'", name); return -1;
        }
        spec = normalized;
    }
    struct im_entry *best = NULL;
    for (struct im_entry *p = im_entries; p; p = p->next) {
        if (!im_scope(p->scope, base ? base : "") || !im_match(p->key, spec)) continue;
        if (!best || strlen(p->scope) > strlen(best->scope) ||
            (strlen(p->scope) == strlen(best->scope) && strlen(p->key) > strlen(best->key))) best = p;
    }
    if (!best) return 0;
    if (!best->address) {
        JS_ThrowTypeError(ctx, "import map blocks '%s'", name); return -1;
    }
    if (!strcmp(best->key, spec)) {
        if ((int)strlen(best->address) >= max) {
            JS_ThrowTypeError(ctx, "import map address too long"); return -1;
        }
        memcpy(out, best->address, strlen(best->address) + 1);
    } else {
        const char *suffix = spec + strlen(best->key);
        if (im_url(best->address, suffix, out, max) != 0 || !im_prefix(best->address, out)) {
            JS_ThrowTypeError(ctx, "import map package path escapes its prefix: '%s'", name); return -1;
        }
    }
    return im_remember(ctx, base, spec) ? 1 : -1;
#endif
}

static int im_object(JSContext *ctx, JSValueConst v)
{ return JS_IsObject(v) && !JS_IsArray(ctx, v); }
static int im_conflict(struct im_entry *list, const char *scope, const char *key)
{
    for (struct im_entry *p = list; p; p = p->next)
        if (!strcmp(p->scope, scope) && !strcmp(p->key, key)) return 1;
    return 0;
}
static int im_add(JSContext *ctx, struct im_entry **list, int *count,
                  const char *scope, const char *key, const char *address)
{
    if (im_conflict(im_entries, scope, key) || im_conflict(*list, scope, key)) return 1;
    for (struct im_seen *p = im_seen; p; p = p->next) {
        if (im_scope(scope, p->base) && im_match(key, p->name)) {
            printf("[js] import map ignored resolved key '%s'\n", key); return 1;
        }
    }
    if (im_count + *count >= IM_ENTRIES) {
        JS_ThrowTypeError(ctx, "import map entry limit (512)"); return 0;
    }
    struct im_entry *p = calloc(1, sizeof *p);
    if (!p) { JS_ThrowOutOfMemory(ctx); return 0; }
    p->scope = im_copy(scope); p->key = im_copy(key);
    if (address) p->address = im_copy(address);
    if (!p->scope || !p->key || (address && !p->address)) {
        im_free_entries(p); JS_ThrowOutOfMemory(ctx); return 0;
    }
    p->next = *list; *list = p; (*count)++; return 1;
}

/* JSON supplies data properties, never invoking page getters. Embedded NUL
 * cannot be represented by the C URL seam, so reject it instead of resolving
 * a different truncated name. Invalid addresses become blocking null entries,
 * as in HTML, rather than disappearing and accidentally enabling fallback. */
static int im_parse_table(JSContext *ctx, JSValueConst obj, const char *base,
                          const char *scope, struct im_entry **list, int *count)
{
    JSPropertyEnum *props = NULL; uint32_t n = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &n, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) return 0;
    int ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) {
        JSValue keyval = JS_AtomToString(ctx, props[i].atom);
        size_t klen = 0; const char *rawkey = JS_ToCStringLen(ctx, &klen, keyval);
        JSValue value = JS_GetProperty(ctx, obj, props[i].atom);
        const char *rawaddr = NULL; size_t alen = 0;
        char key[MOD_URLMAX], address[MOD_URLMAX]; const char *addr = NULL;
        if (!rawkey) ok = 0;
        else if (klen == 0) printf("[js] import map ignored empty key\n");
        else if (klen >= sizeof key || strlen(rawkey) != klen) {
            JS_ThrowTypeError(ctx, "import map key exceeds URL representation"); ok = 0;
        } else {
            if (is_bare_specifier(rawkey)) memcpy(key, rawkey, klen + 1);
            else if (im_url(base, rawkey, key, sizeof key) != 0) {
                JS_ThrowTypeError(ctx, "unsupported import map key URL"); ok = 0;
            }
            if (ok && JS_IsString(value)) {
                rawaddr = JS_ToCStringLen(ctx, &alen, value);
                if (!rawaddr) ok = 0;
                else if (strlen(rawaddr) != alen || alen >= sizeof address) {
                    JS_ThrowTypeError(ctx, "import map address exceeds URL representation"); ok = 0;
                } else if (!is_bare_specifier(rawaddr) && im_url(base, rawaddr, address, sizeof address) == 0) {
                    size_t kn = strlen(key), an = strlen(address);
                    if (key[kn-1] != '/' || (an && address[an-1] == '/')) addr = address;
                }
            }
            if (ok) ok = im_add(ctx, list, count, scope, key, addr);
        }
        if (rawaddr) JS_FreeCString(ctx, rawaddr);
        if (rawkey) JS_FreeCString(ctx, rawkey);
        JS_FreeValue(ctx, keyval); JS_FreeValue(ctx, value);
    }
    for (uint32_t i = 0; i < n; i++) JS_FreeAtom(ctx, props[i].atom);
    js_free(ctx, props); return ok;
}

/* JSON fields are own data, not inherited Object.prototype properties. Read
 * descriptors so a page's prototype getter cannot run while parsing a map. */
static JSValue im_own(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    JSPropertyDescriptor desc;
    int found = JS_GetOwnProperty(ctx, &desc, obj, atom);
    JS_FreeAtom(ctx, atom);
    if (found < 0) return JS_EXCEPTION;
    if (!found) return JS_UNDEFINED;
    JS_FreeValue(ctx, desc.getter); JS_FreeValue(ctx, desc.setter);
    return desc.value;
}
static int im_parse(JSContext *ctx, const char *json, int len, const char *base)
{
    if (len > 1024 * 1024) { JS_ThrowTypeError(ctx, "import map JSON limit (1 MiB)"); return 0; }
    JSValue doc = JS_ParseJSON(ctx, json, len, "<importmap>");
    if (JS_IsException(doc)) return 0;
    if (!im_object(ctx, doc)) {
        JS_FreeValue(ctx, doc); JS_ThrowTypeError(ctx, "import map must be an object"); return 0;
    }
    struct im_entry *pending = NULL; int count = 0, ok = 1;
    JSValue imports = im_own(ctx, doc, "imports");
    JSValue scopes = im_own(ctx, doc, "scopes");
    JSValue integrity = im_own(ctx, doc, "integrity");
    if (JS_IsException(imports) || JS_IsException(scopes) || JS_IsException(integrity)) ok = 0;
    if (ok && ((!JS_IsUndefined(imports) && !im_object(ctx, imports)) ||
        (!JS_IsUndefined(scopes) && !im_object(ctx, scopes)))) {
        JS_ThrowTypeError(ctx, "import map/imports/scopes must be objects"); ok = 0;
    }
    if (ok && !JS_IsUndefined(integrity)) {
        JS_ThrowTypeError(ctx, "import map integrity is not enforced by this loader yet"); ok = 0;
    }
    if (ok && !JS_IsUndefined(imports)) ok = im_parse_table(ctx, imports, base, "", &pending, &count);
    if (ok && !JS_IsUndefined(scopes)) {
        JSPropertyEnum *props = NULL; uint32_t n = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &n, scopes, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) ok = 0;
        for (uint32_t i = 0; i < n && ok; i++) {
            JSValue keyval = JS_AtomToString(ctx, props[i].atom);
            size_t len = 0; const char *key = JS_ToCStringLen(ctx, &len, keyval);
            JSValue table = JS_GetProperty(ctx, scopes, props[i].atom);
            char scope[MOD_URLMAX];
            if (!key) ok = 0;
            else if (!im_object(ctx, table)) { JS_ThrowTypeError(ctx, "import map scope must be an object"); ok = 0; }
            else if (strlen(key) != len || len >= sizeof scope || im_url(base, key, scope, sizeof scope) != 0) {
                JS_ThrowTypeError(ctx, "unsupported import map scope URL"); ok = 0;
            } else ok = im_parse_table(ctx, table, base, scope, &pending, &count);
            if (key) JS_FreeCString(ctx, key);
            JS_FreeValue(ctx, keyval); JS_FreeValue(ctx, table);
        }
        for (uint32_t i = 0; i < n; i++) JS_FreeAtom(ctx, props[i].atom);
        js_free(ctx, props);
    }
    JS_FreeValue(ctx, imports); JS_FreeValue(ctx, scopes); JS_FreeValue(ctx, integrity); JS_FreeValue(ctx, doc);
    if (!ok) { im_free_entries(pending); return 0; }
    /* Commit only after full validation: malformed scopes may not leave half
     * an import map installed while the browser reports rejection. */
    while (pending) { struct im_entry *n = pending->next; pending->next = im_entries;
        im_entries = pending; pending = n; }
    im_count += count;
    printf("[js] import map registered: %d new entries (%d total)\n", count, im_count);
    return 1;
}
