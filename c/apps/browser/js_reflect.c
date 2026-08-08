/* js_reflect.c -- IDL attribute reflection.
 *
 * WHAT THIS IS. `el.title` is not a property with a value; it is a VIEW of the
 * `title` content attribute. Reading it reads the attribute, writing it writes
 * the attribute, and in between sits a coercion that HTML specifies exactly.
 * There are about a dozen coercions -- DOMString, URL, enumerated, boolean,
 * long, unsigned long, double, and their limited/clamped/nullable variants --
 * and several hundred (element, IDL name, content name, type) triples that use
 * them. That ratio is the whole design: the types are code, the triples are
 * DATA, and the data is generated from the same table the tests are generated
 * from (tools/gen_reflect.py -> js_reflect.inc).
 *
 * WHY IT IS ITS OWN FILE. js_dom.c is ~3,100 lines before this and owns the
 * node wrappers, the interface hierarchy, mutation invalidation and event
 * dispatch. Reflection touches none of that: it needs "read attribute", "write
 * attribute", "remove attribute" and a prototype to hang an accessor on.
 *
 * WHY THE COERCIONS ARE THE POINT, and this is the thing to keep if anything
 * here is ever rewritten. A reflected attribute that is a plain string
 * pass-through LOOKS like it works. `el.title = "x"` round-trips, `el.id`
 * round-trips, ordinary pages behave. What breaks is everything the type says:
 *
 *   - `input.size = 0` must throw IndexSizeError, not write "0".
 *   - `td.colSpan` with colspan="abc" is 1, not "abc" and not 0.
 *   - `img.width = 4294967295` writes the DEFAULT, because the value is
 *     outside 0..2147483647; `img.width = -1` writes 4294967295 first (WebIDL
 *     ToUint32 wraps) and then the same rule applies.
 *   - `<a referrerpolicy=BOGUS>`.referrerPolicy is "" (the INVALID value
 *     default), while `<a>`.referrerPolicy is also "" (the MISSING value
 *     default) -- and for `<input formmethod=BOGUS>` those two differ: invalid
 *     gives "get" and missing gives "". Conflating them is the single most
 *     common way to be wrong here, which is why the negative control below
 *     attacks exactly that.
 *   - `a.href` returns an ABSOLUTE url; `a.getAttribute("href")` returns what
 *     the markup said.
 *   - `marquee.scrollAmount` defaults to 6, `input.size` to 20, `textarea.cols`
 *     to 20, `td.rowSpan` to 1, `maxLength` to -1: the default is per attribute,
 *     not per type.
 *
 * NUL IS A LEGAL ATTRIBUTE VALUE, and it is not academic here: a large part of
 * the corpus sets an attribute to a string containing U+0000 and requires
 * getAttribute() to hand the same string back. dom.c has always been able to
 * store it (`struct dom_attr.vlen`, `dom_set_attr_raw`); js_dom.c's getAttribute
 * and setAttribute went through NUL-terminated C strings and silently truncated,
 * which also made every keyword comparison below wrong in the same direction
 * ("text\0" would have matched the keyword "text"). Both halves are
 * length-carrying now, so this file compares LENGTHS as well as bytes.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "js_reflect.h"
#include "js_dom.h"
#include "dom.h"

/* ---- the types the table names ---------------------------------------- */
enum {
    RT_STRING = 0, RT_URL, RT_ENUM, RT_BOOL,
    RT_LONG, RT_LIMLONG,
    RT_ULONG, RT_LIMULONG, RT_LIMULONG_FB, RT_CLAMPED_ULONG,
    RT_DOUBLE, RT_LIMDOUBLE,
    RT_TOKENLIST
};

#define RF_NULLABLE  0x01u   /* null is a value: absent reads as null */
#define RF_NULLEMPTY 0x02u   /* [LegacyNullToEmptyString] on the setter */
#define RF_CUSTOMGET 0x04u   /* the real getter has magic; reflect anyway */
#define RF_HASDEF    0x08u   /* dnum carries an attribute-specific default */
#define RF_DEFNULL   0x10u   /* missing-value default is null / "unspecified" */
#define RF_INVNULL   0x20u   /* invalid-value default is null */

struct refl_attr {
    const char *idl;         /* the IDL attribute name (camelCase) */
    const char *attr;        /* the content attribute name (lowercase) */
    unsigned char type;
    short nkw;               /* enumerated: keyword count ... */
    const char *const *kw;   /* ... and the keywords, in canonical case */
    short ncan;              /* non-canonical map: pair count ... */
    const char *const *can;  /* ... and [from, to] pairs; a NULL `to` is null */
    const char *dstr;        /* enumerated: missing-value default */
    const char *istr;        /* enumerated: invalid-value default */
    double dnum;             /* numeric: the attribute's own default */
    int vmin, vmax;          /* clamped unsigned long */
    unsigned flags;
    /* WHICH element the attribute lives on. 0 -- every generated row -- is
     * "this one". The two others exist because `document.bgColor` reflects the
     * BODY's bgcolor and `document.dir` reflects the DOCUMENT ELEMENT's dir:
     * the IDL attribute and the content attribute are on different nodes. */
    unsigned char target;
};
enum { RTGT_SELF = 0, RTGT_DOCEL, RTGT_BODY };

struct refl_elem { const char *tag; short first; short n; };

#include "js_reflect.inc"

/* ---- the attributes EVERY HTML element reflects -------------------------
 *
 * Not generated, because they are not in the generated data: reflection.js
 * applies them to every element in the table from its own code rather than
 * listing them per element. They land on HTMLElement.prototype, which is where
 * the spec puts them and which is also the only place they can go without
 * repeating them 74 times.
 *
 * className and classList are NOT here. js_dom.c already publishes both, they
 * pass today, and a second writer for the class attribute is exactly what that
 * file's `class_write` comment exists to prevent. `id` likewise: it is
 * Element's, not HTMLElement's, and js_dom.c owns it because the id index
 * depends on it.
 *
 * tabIndex's default is deliberately not the type's. HTML says the default
 * depends on whether the element is focusable, WPT declines to test it
 * ("the rules are complicated, and a lot of them are SHOULDs") and passes
 * defaultVal:null, so every subtest whose expectation would be the default is
 * skipped. We answer 0 for the elements that are focusable without help and -1
 * for the rest, which is what browsers do. */
static const char *const KW_DIR[] = { "ltr", "rtl", "auto" };

static const struct refl_attr RFL_GLOBAL[] = {
    { "title",     "title",     RT_STRING, 0,0, 0,0, 0, 0, 0.0, 0,0, 0, RTGT_SELF },
    { "lang",      "lang",      RT_STRING, 0,0, 0,0, 0, 0, 0.0, 0,0, 0, RTGT_SELF },
    { "dir",       "dir",       RT_ENUM,   3,KW_DIR, 0,0, "", "", 0.0, 0,0, 0, RTGT_SELF },
    { "autofocus", "autofocus", RT_BOOL,   0,0, 0,0, 0, 0, 0.0, 0,0, 0, RTGT_SELF },
    { "hidden",    "hidden",    RT_BOOL,   0,0, 0,0, 0, 0, 0.0, 0,0, 0, RTGT_SELF },
    { "accessKey", "accesskey", RT_STRING, 0,0, 0,0, 0, 0, 0.0, 0,0, 0, RTGT_SELF },
    { "tabIndex",  "tabindex",  RT_LONG,   0,0, 0,0, 0, 0, 0.0, 0,0, RF_DEFNULL, RTGT_SELF },
};
#define RFL_NGLOBAL ((int)(sizeof RFL_GLOBAL / sizeof RFL_GLOBAL[0]))

/* ---- the reflections that live on Document -----------------------------
 *
 * The IDL attribute is `document`'s and the content attribute is somebody
 * else's: document.dir is the DOCUMENT ELEMENT's dir, and the five colour
 * attributes are the BODY's. They are the last of the pre-HTML5 document
 * surface that is still specified and still shipped, and the corpus tests them
 * (elements-sections.js's extraTests) exactly like any other reflection.
 *
 * treatNullAsEmptyString on the colours is not decoration: `document.bgColor =
 * null` writes the empty string, not "null". */
static const struct refl_attr RFL_DOCUMENT[] = {
    { "dir",        "dir",     RT_ENUM,   3,KW_DIR, 0,0, "", "", 0.0, 0,0, 0, RTGT_DOCEL },
    { "fgColor",    "text",    RT_STRING, 0,0, 0,0, 0, 0, 0.0, 0,0, RF_NULLEMPTY, RTGT_BODY },
    { "linkColor",  "link",    RT_STRING, 0,0, 0,0, 0, 0, 0.0, 0,0, RF_NULLEMPTY, RTGT_BODY },
    { "vlinkColor", "vlink",   RT_STRING, 0,0, 0,0, 0, 0, 0.0, 0,0, RF_NULLEMPTY, RTGT_BODY },
    { "alinkColor", "alink",   RT_STRING, 0,0, 0,0, 0, 0, 0.0, 0,0, RF_NULLEMPTY, RTGT_BODY },
    { "bgColor",    "bgcolor", RT_STRING, 0,0, 0,0, 0, 0, 0.0, 0,0, RF_NULLEMPTY, RTGT_BODY },
};
#define RFL_NDOC ((int)(sizeof RFL_DOCUMENT / sizeof RFL_DOCUMENT[0]))

/* Magic is one flat index over the three tables, so one accessor pair serves
 * all of them and QuickJS carries no per-property closure. */
#define MAGIC_GLOBAL RFL_NATTRS
#define MAGIC_DOC    (MAGIC_GLOBAL + RFL_NGLOBAL)
#define MAGIC_END    (MAGIC_DOC + RFL_NDOC)

static const struct refl_attr *row_at(int magic)
{
    if (magic < 0) return 0;
    if (magic < RFL_NATTRS) return &RFL_ATTRS[magic];
    if (magic < MAGIC_DOC)  return &RFL_GLOBAL[magic - MAGIC_GLOBAL];
    if (magic < MAGIC_END)  return &RFL_DOCUMENT[magic - MAGIC_DOC];
    return 0;
}

/* THE NEGATIVE CONTROL, and the reason it is HERE and not "remove reflection".
 *
 * Deleting the file would prove that a missing feature is missing, which nobody
 * doubted. The failure this code can actually have is subtler and it is the one
 * a reviewer cannot see: reflection that WORKS and is untyped. Every property
 * exists, every prototype is right, every attribute round-trips, `el.title` and
 * `el.id` behave, a real page renders -- and the enumerated invalid-value
 * defaults, the integer parsing rules, the range clamping, the throwing setters
 * and the URL resolution are all gone.
 *
 * So the control leaves the plumbing entirely intact and attacks ONLY the
 * coercion: every attribute becomes a plain DOMString pass-through.
 * tests/reflect.mk builds it and requires tests/unit/reflect_test.c to go red. */
static unsigned char eff_type(const struct refl_attr *r)
{
#ifdef REFLECT_NEGCTL
    (void)r;
    return RT_STRING;
#else
    return r->type;
#endif
}

/* ---- the HTML parsing rules, ported from the spec ----------------------
 *
 * These are the spec's algorithms, not strtol/strtod. The differences are the
 * whole reason they are written out:
 *   - only U+0009/A/C/D/20 are leading whitespace. U+000B is NOT, and neither
 *     is U+00A0, U+FEFF or any of the Unicode spaces -- "7" does not
 *     parse as 7, which is a fifth of the numeric subtests on its own.
 *   - a trailing non-digit does not fail: "5%" is 5, "1. 1" is 1.
 *   - "+100" parses (the sign is consumed), ".5" parses as a float and not as
 *     an integer, and "-0" is 0 rather than -0.
 *   - strtol would accept "0x10" and " 0b1"; the spec does not.
 */
#define MAXINT_ 2147483647.0
#define MININT_ (-2147483648.0)

static int is_space(unsigned char c)
{ return c == 0x09 || c == 0x0a || c == 0x0c || c == 0x0d || c == 0x20; }
static int is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* The rules for parsing integers. Returns 1 and fills *out on success. The
 * accumulator is a double so that a 20-digit run cannot wrap into range -- an
 * overflowing value must be REJECTED as out of range, and an int64 that wrapped
 * would silently look acceptable. */
static int parse_int(const char *s, int len, double *out)
{
    int p = 0, sign = 1;
    while (p < len && is_space((unsigned char)s[p])) p++;
    if (p >= len) return 0;
    if (s[p] == '-') { sign = -1; p++; }
    else if (s[p] == '+') p++;
    if (p >= len || !is_digit((unsigned char)s[p])) return 0;
    double v = 0;
    while (p < len && is_digit((unsigned char)s[p])) {
        if (v < 1e300) v = v * 10 + (s[p] - '0');
        p++;
    }
    *out = (v == 0) ? 0 : sign * v;
    return 1;
}

/* The rules for parsing non-negative integers: the above, then reject < 0. */
static int parse_nonneg(const char *s, int len, double *out)
{
    double v;
    if (!parse_int(s, len, &v) || v < 0) return 0;
    *out = v;
    return 1;
}

/* The rules for parsing floating-point number values. */
static int parse_float(const char *s, int len, double *out)
{
    int p = 0;
    double value = 1, divisor = 1, exponent = 1;
    while (p < len && is_space((unsigned char)s[p])) p++;
    if (p >= len) return 0;
    if (s[p] == '-') { value = -1; divisor = -1; p++; }
    else if (s[p] == '+') p++;
    if (p >= len) return 0;

    if (s[p] == '.' && p + 1 < len && is_digit((unsigned char)s[p + 1])) {
        value = 0;
    } else if (!is_digit((unsigned char)s[p])) {
        return 0;
    } else {
        double val = 0;
        while (p < len && is_digit((unsigned char)s[p])) {
            val = val * 10 + (s[p] - '0');
            p++;
        }
        value *= val;
    }
    if (p < len && s[p] == '.') {
        p++;
        while (p < len && is_digit((unsigned char)s[p])) {
            divisor *= 10;
            value += (s[p] - '0') / divisor;
            p++;
        }
    }
    if (p < len && (s[p] == 'e' || s[p] == 'E')) {
        p++;
        if (p < len) {
            if (s[p] == '-') { exponent = -1; p++; }
            else if (s[p] == '+') p++;
            if (p < len && is_digit((unsigned char)s[p])) {
                double exp = 0;
                do {
                    exp = exp * 10 + (s[p] - '0');
                    p++;
                } while (p < len && is_digit((unsigned char)s[p]));
                exponent *= exp;
                value *= pow(10, exponent);
            }
        }
    }
    if (!isfinite(value)) return 0;
    *out = (value == 0) ? 0 : value;
    return 1;
}

/* ASCII case-insensitive, LENGTH-AWARE. Both halves matter: the corpus tests
 * the Kelvin sign U+212A against keywords containing "k" and U+017F against
 * "s", which must NOT match (they are Unicode case folds, not ASCII ones -- a
 * byte comparison gets that right for free), and it tests keyword+"\0", which
 * must not match either (only the length says so). */
static int ascii_ci_eq(const char *a, int alen, const char *b)
{
    int blen = (int)strlen(b);
    if (alen != blen) return 0;
    for (int i = 0; i < alen; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

/* ---- numeric defaults --------------------------------------------------- */

static double type_default(const struct refl_attr *r)
{
    if (r->flags & RF_HASDEF) return r->dnum;
    switch (r->type) {
        case RT_LIMLONG:       return -1;
        case RT_LIMULONG:
        case RT_LIMULONG_FB:   return 1;
        default:               return 0;
    }
}

/* ---- attribute access --------------------------------------------------- */

static const char *get_attr(const struct node *n, const char *name, int *len)
{
    *len = 0;
    if (!n) return 0;
    return js_dom_attr_len(n, name, len);
}

/* ---- URL resolution -----------------------------------------------------
 *
 * A reflected URL getter returns the content attribute RESOLVED against the
 * document's base URL, and the literal attribute if that fails. The resolver is
 * the page's own `URL` constructor rather than a C call into c/net/http/url.c,
 * for a link reason and not a taste one: url.c is not on every source list that
 * builds this file, and a hard dependency would break three fragments that
 * belong to other lines. When there is no URL constructor (a build without the
 * web-API layer) the literal is returned, which is the same answer the spec
 * gives for an unparseable URL. */
static JSValue url_object(JSContext *ctx, const char *v, int len)
{
    JSValue lit = JS_NewStringLen(ctx, v ? v : "", (size_t)(len > 0 ? len : 0));
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, g, "URL");
    JSValue u = JS_UNDEFINED;
    if (JS_IsFunction(ctx, ctor)) {
        JSValue base = JS_UNDEFINED;
        JSValue doc = JS_GetPropertyStr(ctx, g, "document");
        if (JS_IsObject(doc)) base = JS_GetPropertyStr(ctx, doc, "baseURI");
        JS_FreeValue(ctx, doc);
        if (!JS_IsString(base)) {
            JS_FreeValue(ctx, base);
            JSValue loc = JS_GetPropertyStr(ctx, g, "location");
            base = JS_IsObject(loc) ? JS_GetPropertyStr(ctx, loc, "href") : JS_UNDEFINED;
            JS_FreeValue(ctx, loc);
        }
        JSValueConst argv[2] = { lit, base };
        u = JS_CallConstructor(ctx, ctor, JS_IsString(base) ? 2 : 1, argv);
        if (JS_IsException(u)) {
            JS_FreeValue(ctx, JS_GetException(ctx));       /* unparseable */
            u = JS_UNDEFINED;
        }
        JS_FreeValue(ctx, base);
    }
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, g);
    JS_FreeValue(ctx, lit);
    return u;
}

static JSValue resolve_url(JSContext *ctx, const char *v, int len)
{
    JSValue u = url_object(ctx, v, len);
    if (JS_IsObject(u)) {
        JSValue href = JS_GetPropertyStr(ctx, u, "href");
        JS_FreeValue(ctx, u);
        if (JS_IsString(href)) return href;
        JS_FreeValue(ctx, href);
    } else {
        JS_FreeValue(ctx, u);
    }
    /* Parsing failed (or there is no URL implementation in this build): the
     * spec says return the content attribute as it stands. */
    return JS_NewStringLen(ctx, v ? v : "", (size_t)(len > 0 ? len : 0));
}

/* ---- enumerated: content value -> IDL value ----------------------------- */

static const char *enum_canon(const struct refl_attr *r, const char *ret)
{
    for (int i = 0; i < r->ncan; i++)
        if (ret && !strcmp(ret, r->can[i * 2])) return r->can[i * 2 + 1];
    return ret;
}

static JSValue enum_get(JSContext *ctx, const struct refl_attr *r,
                        const char *v, int len)
{
    const char *ret;
    if (!v) {
        /* missing value default */
        if (r->flags & RF_DEFNULL) return JS_NULL;
        ret = r->dstr ? r->dstr : "";
    } else {
        ret = 0;
        for (int i = 0; i < r->nkw; i++)
            if (ascii_ci_eq(v, len, r->kw[i])) { ret = r->kw[i]; break; }  /* canonical case */
        if (!ret) {
            /* invalid value default -- a DIFFERENT knob from the missing one */
            if (r->flags & RF_INVNULL) return JS_NULL;
            ret = r->istr ? r->istr : "";
        }
    }
    ret = enum_canon(r, ret);
    return ret ? JS_NewString(ctx, ret) : JS_NULL;
}

/* ---- the getter --------------------------------------------------------- */

static int tab_focusable(const char *tag)
{
    static const char *const F[] = { "a", "area", "button", "input", "select",
                                     "textarea", "iframe", "object" };
    for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++)
        if (!strcmp(tag, F[i])) return 1;
    return 0;
}

/* The node whose content attribute this IDL attribute is a view of -- itself,
 * unless it is one of the six on Document. Reached through the wrapper's own
 * `documentElement` / `body` properties rather than through the DOM, so this
 * file inherits js_dom.c's answer to "which <body> counts" instead of holding a
 * second opinion about it. */
static struct node *target_node(JSContext *ctx, JSValueConst this_val,
                                const struct refl_attr *r)
{
    if (r->target == RTGT_SELF) return js_dom_node_from(this_val);
    JSValue v = JS_GetPropertyStr(ctx, this_val,
                                  r->target == RTGT_DOCEL ? "documentElement" : "body");
    struct node *n = js_dom_node_from(v);
    JS_FreeValue(ctx, v);       /* the wrapper is cached on the node; this only
                                 * drops our reference to it */
    return n;
}

/* `form.action` and `input`/`button.formAction` read as the DOCUMENT's URL when
 * the content attribute is absent, not as the empty string every other
 * reflected URL gives. It is a genuine per-attribute rule in HTML (the form
 * submits to the current document), reflection.js carries it as its own
 * "Hard-coded special case", and resolving the empty string against the base is
 * exactly what produces it. */
static int url_defaults_to_document(const struct refl_attr *r)
{
    return !strcmp(r->attr, "action") || !strcmp(r->attr, "formaction");
}

static JSValue refl_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    const struct refl_attr *r = row_at(magic);
    if (!r) return JS_UNDEFINED;
    struct node *n = target_node(ctx, this_val, r);
    int len = 0;
    const char *v = n ? get_attr(n, r->attr, &len) : 0;
    double num, dflt = type_default(r);
    if (!n && r->target != RTGT_SELF) {
        /* No <body> / no document element: every one of these is a DOMString,
         * and an absent element is not different from an absent attribute. */
        return JS_NewString(ctx, "");
    }
    if (!n) return JS_UNDEFINED;

    switch (eff_type(r)) {
    case RT_STRING:
    case RT_TOKENLIST:
        return JS_NewStringLen(ctx, v ? v : "", v ? (size_t)len : 0);

    case RT_URL:
        if (!v) {
            if (!url_defaults_to_document(r)) return JS_NewString(ctx, "");
            v = ""; len = 0;
        }
        /* An attribute that is PRESENT and empty is not absent: it parses to
         * the document's own URL. Only an absent one is "". */
        return resolve_url(ctx, v, len);

    case RT_ENUM:
        return enum_get(ctx, r, v, len);

    case RT_BOOL:
        return JS_NewBool(ctx, v != 0);

    case RT_LONG:
        if (r->flags & RF_DEFNULL)
            dflt = tab_focusable(n->tag) ? 0 : -1;
        if (v && parse_int(v, len, &num) && num >= MININT_ && num <= MAXINT_)
            return JS_NewInt32(ctx, (int32_t)num);
        return JS_NewInt32(ctx, (int32_t)dflt);

    case RT_LIMLONG:
        if (v && parse_nonneg(v, len, &num) && num <= MAXINT_)
            return JS_NewInt32(ctx, (int32_t)num);
        return JS_NewInt32(ctx, (int32_t)dflt);

    case RT_ULONG:
        if (v && parse_nonneg(v, len, &num) && num <= MAXINT_)
            return JS_NewUint32(ctx, (uint32_t)num);
        return JS_NewUint32(ctx, (uint32_t)dflt);

    case RT_LIMULONG:
    case RT_LIMULONG_FB:
        if (v && parse_nonneg(v, len, &num) && num >= 1 && num <= MAXINT_)
            return JS_NewUint32(ctx, (uint32_t)num);
        return JS_NewUint32(ctx, (uint32_t)dflt);

    case RT_CLAMPED_ULONG:
        if (v && parse_nonneg(v, len, &num)) {
            if (num < r->vmin) num = r->vmin;
            if (num > r->vmax) num = r->vmax;
            return JS_NewUint32(ctx, (uint32_t)num);
        }
        return JS_NewUint32(ctx, (uint32_t)dflt);

    case RT_DOUBLE:
        if (v && parse_float(v, len, &num)) return JS_NewFloat64(ctx, num);
        return JS_NewFloat64(ctx, dflt);

    case RT_LIMDOUBLE:
        if (v && parse_float(v, len, &num) && num > 0) return JS_NewFloat64(ctx, num);
        return JS_NewFloat64(ctx, dflt);
    }
    return JS_UNDEFINED;
}

/* ---- the setter --------------------------------------------------------- */

/* The shortest string representing an integer, which is what the spec asks for
 * and what "%d" gives. The double form goes through the engine so that the
 * "best representation of the number as a floating point number" is the
 * ECMAScript one -- 1e25 must serialise as "1e+25" and 0.0001 as "0.0001", and
 * neither %g nor %.17g produces those. */
static void set_num(JSContext *ctx, struct node *n, const char *attr, double d,
                    int as_int)
{
    if (as_int) {
        char buf[32];
        int len = snprintf(buf, sizeof buf, "%d", (int)d);
        js_dom_attr_write(ctx, n, attr, buf, len);
        return;
    }
    JSValue nv = JS_NewFloat64(ctx, d);
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, nv);
    if (s) { js_dom_attr_write(ctx, n, attr, s, (int)len); JS_FreeCString(ctx, s); }
    JS_FreeValue(ctx, nv);
}

static JSValue set_string(JSContext *ctx, struct node *n, const char *attr,
                          JSValueConst v)
{
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, v);
    if (!s) return JS_EXCEPTION;
    js_dom_attr_write(ctx, n, attr, s, (int)len);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue refl_set(JSContext *ctx, JSValueConst this_val, JSValueConst v,
                        int magic)
{
    const struct refl_attr *r = row_at(magic);
    if (!r) return JS_UNDEFINED;
    struct node *n = target_node(ctx, this_val, r);
    if (!n || n->type != N_ELEM) return JS_UNDEFINED;

    switch (eff_type(r)) {
    case RT_STRING:
    case RT_URL:
    case RT_TOKENLIST:
        if ((r->flags & RF_NULLEMPTY) && JS_IsNull(v)) {
            js_dom_attr_write(ctx, n, r->attr, "", 0);
            return JS_UNDEFINED;
        }
        return set_string(ctx, n, r->attr, v);

    case RT_ENUM:
        /* A nullable enumerated attribute set to null (or undefined) REMOVES
         * the content attribute. Set to anything else, the content attribute
         * becomes the value as given -- not the canonical keyword; the
         * canonicalisation happens on the way back out. */
        if ((r->flags & RF_NULLABLE) && (JS_IsNull(v) || JS_IsUndefined(v))) {
            js_dom_attr_erase(ctx, n, r->attr);
            return JS_UNDEFINED;
        }
        return set_string(ctx, n, r->attr, v);

    case RT_BOOL:
        if (JS_ToBool(ctx, v)) js_dom_attr_write(ctx, n, r->attr, "", 0);
        else                   js_dom_attr_erase(ctx, n, r->attr);
        return JS_UNDEFINED;

    case RT_LONG: {
        int32_t x;
        if (JS_ToInt32(ctx, &x, v)) return JS_EXCEPTION;
        set_num(ctx, n, r->attr, x, 1);
        return JS_UNDEFINED;
    }
    case RT_LIMLONG: {
        int32_t x;
        if (JS_ToInt32(ctx, &x, v)) return JS_EXCEPTION;
        if (x < 0)
            return js_dom_throw_dom(ctx, "IndexSizeError",
                                    "the value must not be negative");
        set_num(ctx, n, r->attr, x, 1);
        return JS_UNDEFINED;
    }
    case RT_ULONG:
    case RT_CLAMPED_ULONG: {
        uint32_t x;
        if (JS_ToUint32(ctx, &x, v)) return JS_EXCEPTION;
        double d = (x > (uint32_t)MAXINT_) ? type_default(r) : (double)x;
        set_num(ctx, n, r->attr, d, 1);
        return JS_UNDEFINED;
    }
    case RT_LIMULONG: {
        uint32_t x;
        if (JS_ToUint32(ctx, &x, v)) return JS_EXCEPTION;
        if (x == 0)
            return js_dom_throw_dom(ctx, "IndexSizeError",
                                    "the value must be greater than zero");
        double d = (x > (uint32_t)MAXINT_) ? type_default(r) : (double)x;
        set_num(ctx, n, r->attr, d, 1);
        return JS_UNDEFINED;
    }
    case RT_LIMULONG_FB: {
        uint32_t x;
        if (JS_ToUint32(ctx, &x, v)) return JS_EXCEPTION;
        double d = (x == 0 || x > (uint32_t)MAXINT_) ? type_default(r) : (double)x;
        set_num(ctx, n, r->attr, d, 1);
        return JS_UNDEFINED;
    }
    case RT_DOUBLE: {
        double d;
        if (JS_ToFloat64(ctx, &d, v)) return JS_EXCEPTION;
        set_num(ctx, n, r->attr, d, 0);
        return JS_UNDEFINED;
    }
    case RT_LIMDOUBLE: {
        double d;
        if (JS_ToFloat64(ctx, &d, v)) return JS_EXCEPTION;
        if (d <= 0) return JS_UNDEFINED;      /* ignored; attribute unchanged */
        set_num(ctx, n, r->attr, d, 0);
        return JS_UNDEFINED;
    }
    }
    return JS_UNDEFINED;
}

/* ---- HTMLHyperlinkElementUtils ------------------------------------------
 *
 * `a.protocol`, `.host`, `.pathname`, `.search`, `.hash` and the rest: the same
 * URL the href reflection resolves, taken apart. They belong here rather than
 * beside href in the table because they are not reflections -- there is no
 * `protocol` content attribute; they are a VIEW of one attribute through nine
 * names, and a write to any of them writes back through href.
 *
 * They are also the reason the url-typed subtests can pass at all. WPT's
 * reflection.js does not carry expected URLs; it computes each one by creating
 * an <a>, assigning the input to `.href`, and reading
 *   el.protocol + "//" + el.host + el.pathname + el.search + el.hash
 * back off it. With those members missing the expectation for EVERY url subtest
 * in the corpus was the literal string "undefined//undefinedundefinedundefined"
 * -- a number no implementation of href can reach. The oracle is our own
 * decomposition, so what the corpus actually asks is that href and the
 * components agree. Routing both through one URL object makes that structural
 * rather than a coincidence.
 *
 * The write direction is the ordinary one: rebuild the URL with the component
 * replaced and store its href back in the content attribute. */
static const char *const HL_NAMES[] = {
    "protocol", "username", "password", "host", "hostname",
    "port", "pathname", "search", "hash", "origin"
};
#define HL_COUNT ((int)(sizeof HL_NAMES / sizeof HL_NAMES[0]))
#define HL_ORIGIN (HL_COUNT - 1)              /* read-only */

/* Which element gets them. The spec's list is <a> and <area> -- and NOT <link>,
 * whose href is a plain reflected URL with no decomposition. */
static const char *const HL_TAGS[] = { "a", "area" };

static JSValue hlink_url(JSContext *ctx, JSValueConst this_val)
{
    struct node *n = js_dom_node_from(this_val);
    if (!n || n->type != N_ELEM) return JS_UNDEFINED;
    int len = 0;
    const char *v = js_dom_attr_len(n, "href", &len);
    if (!v) return JS_UNDEFINED;              /* no href: every part is "" */
    return url_object(ctx, v, len);
}

static JSValue hlink_get(JSContext *ctx, JSValueConst this_val, int magic)
{
    JSValue u = hlink_url(ctx, this_val);
    if (!JS_IsObject(u)) { JS_FreeValue(ctx, u); return JS_NewString(ctx, ""); }
    JSValue out = JS_GetPropertyStr(ctx, u, HL_NAMES[magic]);
    JS_FreeValue(ctx, u);
    if (!JS_IsString(out)) { JS_FreeValue(ctx, out); return JS_NewString(ctx, ""); }
    return out;
}

static JSValue hlink_set(JSContext *ctx, JSValueConst this_val, JSValueConst v,
                         int magic)
{
    struct node *n = js_dom_node_from(this_val);
    if (!n || n->type != N_ELEM || magic == HL_ORIGIN) return JS_UNDEFINED;
    JSValue u = hlink_url(ctx, this_val);
    if (!JS_IsObject(u)) { JS_FreeValue(ctx, u); return JS_UNDEFINED; }
    JS_SetPropertyStr(ctx, u, HL_NAMES[magic], JS_DupValue(ctx, v));
    JSValue href = JS_GetPropertyStr(ctx, u, "href");
    JS_FreeValue(ctx, u);
    if (JS_IsString(href)) {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, href);
        if (s) { js_dom_attr_write(ctx, n, "href", s, (int)len); JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, href);
    return JS_UNDEFINED;
}

/* ---- installation ------------------------------------------------------- */

static int g_installed;

int js_reflect_installed(void) { return g_installed; }

/* One accessor pair, defined ONLY if this exact prototype does not already own
 * a property of that name.
 *
 * OWN, not inherited, and that is the whole coexistence rule with the other
 * lines in this directory. js_forms.c and js_platform.c both install reflected
 * properties of their own and both use "never clobber an inherited one", and
 * both run AFTER js_dom_init -- so writing here first means theirs step aside
 * where we overlap and stay where we do not. Checking the CHAIN instead would
 * make this file skip `input.name` because Element.prototype gained a `name`
 * from somewhere, which is the wrong answer for a per-tag attribute. */
static void define_one(JSContext *ctx, JSValueConst proto, int magic)
{
    const struct refl_attr *r = row_at(magic);
    if (!r || r->type == RT_TOKENLIST) return;   /* see the note in the header */
    JSAtom a = JS_NewAtom(ctx, r->idl);
    JSPropertyDescriptor d;
    if (JS_GetOwnProperty(ctx, &d, proto, a) > 0) {
        JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
        JS_FreeAtom(ctx, a);
        return;
    }
    JSValue get = JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)refl_get, r->idl,
                                       0, JS_CFUNC_getter_magic, magic);
    JSValue set = JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)refl_set, r->idl,
                                       1, JS_CFUNC_setter_magic, magic);
    JS_DefinePropertyGetSet(ctx, proto, a, get, set,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
    g_installed++;
}

/* The nine URL components, on one prototype. Same never-clobber rule. */
static void define_hlink(JSContext *ctx, JSValueConst proto)
{
    for (int i = 0; i < HL_COUNT; i++) {
        JSAtom a = JS_NewAtom(ctx, HL_NAMES[i]);
        JSPropertyDescriptor d;
        if (JS_GetOwnProperty(ctx, &d, proto, a) > 0) {
            JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter);
            JS_FreeValue(ctx, d.setter); JS_FreeAtom(ctx, a);
            continue;
        }
        JSValue get = JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)hlink_get,
                                           HL_NAMES[i], 0, JS_CFUNC_getter_magic, i);
        JSValue set = (i == HL_ORIGIN) ? JS_UNDEFINED
            : JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)hlink_set,
                                   HL_NAMES[i], 1, JS_CFUNC_setter_magic, i);
        JS_DefinePropertyGetSet(ctx, proto, a, get, set,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
        g_installed++;
    }
}

void js_reflect_install(JSContext *ctx, JSValueConst html_proto,
                        js_reflect_proto_fn proto_for, void *ud)
{
    g_installed = 0;
    if (JS_IsObject(html_proto))
        for (int i = 0; i < RFL_NGLOBAL; i++)
            define_one(ctx, html_proto, MAGIC_GLOBAL + i);

    /* Document's six go on the document object itself. `document` is a Node
     * wrapper now (js_dom.c wraps the #document node), so this is the same kind
     * of accessor as every other one here -- it just reads its attribute off a
     * different node. */
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue doc = JS_GetPropertyStr(ctx, g, "document");
    if (JS_IsObject(doc)) {
        JSValue dp = JS_GetPrototype(ctx, doc);
        JSValueConst on = JS_IsObject(dp) ? dp : doc;
        for (int i = 0; i < RFL_NDOC; i++) define_one(ctx, on, MAGIC_DOC + i);
        JS_FreeValue(ctx, dp);
    }
    JS_FreeValue(ctx, doc);
    JS_FreeValue(ctx, g);

    if (!proto_for) return;
    for (int e = 0; e < RFL_NELEMS; e++) {
        JSValueConst proto = proto_for(ud, RFL_ELEMS[e].tag);
        if (!JS_IsObject(proto)) continue;
        for (int k = 0; k < RFL_ELEMS[e].n; k++)
            define_one(ctx, proto, RFL_ELEMS[e].first + k);
    }
    for (unsigned t = 0; t < sizeof HL_TAGS / sizeof HL_TAGS[0]; t++) {
        JSValueConst proto = proto_for(ud, HL_TAGS[t]);
        if (JS_IsObject(proto)) define_hlink(ctx, proto);
    }
}
