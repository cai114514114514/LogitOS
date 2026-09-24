/* HTML sandboxing flag sets (HTML Standard, "Sandboxing", and "parse a
 * sandboxing directive"), shared by every door that produces one:
 *
 *   - an <iframe sandbox> attribute, snapshotted when the frame navigates;
 *   - an enforced Content-Security-Policy `sandbox` directive delivered in a
 *     RESPONSE HEADER (CSP3 6.3.2: ignored in <meta> and in report-only);
 *   - the embedding document's own active flag set, which a nested frame
 *     inherits (the creation sandboxing flags are the union of the two).
 *
 * ONE PARSER, SEVERAL DOORS. Header-only on purpose: the network frame
 * loader, the JS about:blank/srcdoc loader (platform.c, through a native
 * function) and the host gate all call this very function, and none of the
 * dozens of hand-copied host source lists that link platform.c has to learn
 * a new translation unit (CLAUDE.md, "hand-copied source lists").
 *
 * A SET BIT IS A RESTRICTION. The empty set means "not sandboxed". Parsing
 * starts from every flag and each allow-* keyword clears the ones it names;
 * flags no keyword clears (navigation, plugins, document.domain) are always
 * set for a sandboxed document. Tokens are split on ASCII whitespace and
 * compared ASCII case-insensitively; unknown tokens and duplicates have no
 * effect, exactly as the algorithm's "unless tokens contains" wording gives.
 *
 * Which flags are ENFORCED is decided by the consumers, not here -- see
 * frame/passive.c (scripts, origin), js/runtime/page.c (modals, popups,
 * forms) and window.inc (opaque-origin messaging). A flag this engine has no
 * feature for (pointer lock, orientation lock, presentation, downloads,
 * custom protocols, storage access) is still parsed and stored, so a future
 * consumer inherits the right answer instead of an absent one. */
#ifndef LOGIT_FRAME_SANDBOX_H
#define LOGIT_FRAME_SANDBOX_H
#include <stddef.h>

enum {
    SANDBOX_NAVIGATION               = 1u << 0,  /* no keyword clears it */
    SANDBOX_AUXILIARY_NAVIGATION     = 1u << 1,  /* allow-popups */
    SANDBOX_TOP_NAVIGATION           = 1u << 2,  /* ... without user activation: allow-top-navigation */
    SANDBOX_TOP_NAVIGATION_USER      = 1u << 3,  /* ... with user activation: also -by-user-activation */
    SANDBOX_PLUGINS                  = 1u << 4,  /* no keyword clears it */
    SANDBOX_ORIGIN                   = 1u << 5,  /* allow-same-origin */
    SANDBOX_FORMS                    = 1u << 6,  /* allow-forms */
    SANDBOX_POINTER_LOCK             = 1u << 7,  /* allow-pointer-lock */
    SANDBOX_SCRIPTS                  = 1u << 8,  /* allow-scripts */
    SANDBOX_AUTOMATIC_FEATURES       = 1u << 9,  /* allow-scripts, the same keyword */
    SANDBOX_DOCUMENT_DOMAIN          = 1u << 10, /* no keyword clears it */
    SANDBOX_PROPAGATES_TO_AUXILIARY  = 1u << 11, /* allow-popups-to-escape-sandbox */
    SANDBOX_MODALS                   = 1u << 12, /* allow-modals */
    SANDBOX_ORIENTATION_LOCK         = 1u << 13, /* allow-orientation-lock */
    SANDBOX_PRESENTATION             = 1u << 14, /* allow-presentation */
    SANDBOX_DOWNLOADS                = 1u << 15, /* allow-downloads */
    SANDBOX_CUSTOM_PROTOCOLS         = 1u << 16, /* allow-popups / -top-navigation* / -to-custom-protocols */
    SANDBOX_STORAGE_ACCESS           = 1u << 17, /* allow-storage-access-by-user-activation */
    SANDBOX_ALL                      = (1u << 18) - 1
};

static inline int sandbox_ascii_space(int c)
{ return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

static inline int sandbox_token_is(const char *t, size_t n, const char *keyword)
{
    size_t i = 0;
    for (; i < n && keyword[i]; i++) {
        int c = (unsigned char)t[i];
#ifndef SANDBOX_NEGCTL_CASE_SENSITIVE              /* host-gate control only */
        if (c >= 'A' && c <= 'Z') c += 32;           /* ASCII only: no Unicode folding */
#endif
        if (c != keyword[i]) return 0;
    }
    return i == n && !keyword[i];
}

/* The HTML algorithm. `len` bounds `value`; pass (size_t)-1 for a C string.
 * NULL is the empty attribute value, i.e. every restriction. */
static inline unsigned sandbox_parse_n(const char *value, size_t len)
{
    unsigned allow = 0;
    enum { A_POPUPS = 1, A_TOP = 2, A_TOP_USER = 4, A_SAME_ORIGIN = 8, A_FORMS = 16,
           A_POINTER = 32, A_SCRIPTS = 64, A_ESCAPE = 128, A_MODALS = 256,
           A_ORIENTATION = 512, A_PRESENTATION = 1024, A_DOWNLOADS = 2048,
           A_CUSTOM = 4096, A_STORAGE = 8192 };
    static const struct { const char *name; unsigned bit; } words[] = {
        { "allow-popups", A_POPUPS },
        { "allow-top-navigation", A_TOP },
        { "allow-top-navigation-by-user-activation", A_TOP_USER },
        { "allow-same-origin", A_SAME_ORIGIN },
        { "allow-forms", A_FORMS },
        { "allow-pointer-lock", A_POINTER },
        { "allow-scripts", A_SCRIPTS },
        { "allow-popups-to-escape-sandbox", A_ESCAPE },
        { "allow-modals", A_MODALS },
        { "allow-orientation-lock", A_ORIENTATION },
        { "allow-presentation", A_PRESENTATION },
        { "allow-downloads", A_DOWNLOADS },
        { "allow-top-navigation-to-custom-protocols", A_CUSTOM },
        { "allow-storage-access-by-user-activation", A_STORAGE },
    };
    size_t i = 0;
    while (value && i < len && value[i]) {
        while (i < len && value[i] && sandbox_ascii_space((unsigned char)value[i])) i++;
        size_t b = i;
        while (i < len && value[i] && !sandbox_ascii_space((unsigned char)value[i])) i++;
        if (i == b) break;
        for (size_t w = 0; w < sizeof words / sizeof words[0]; w++)
            if (sandbox_token_is(value + b, i - b, words[w].name)) allow |= words[w].bit;
    }
    unsigned flags = SANDBOX_NAVIGATION | SANDBOX_PLUGINS | SANDBOX_DOCUMENT_DOMAIN;
    if (!(allow & A_POPUPS)) flags |= SANDBOX_AUXILIARY_NAVIGATION;
    if (!(allow & A_TOP)) flags |= SANDBOX_TOP_NAVIGATION;
    if (!(allow & (A_TOP | A_TOP_USER))) flags |= SANDBOX_TOP_NAVIGATION_USER;
    if (!(allow & A_SAME_ORIGIN)) flags |= SANDBOX_ORIGIN;
    if (!(allow & A_FORMS)) flags |= SANDBOX_FORMS;
    if (!(allow & A_POINTER)) flags |= SANDBOX_POINTER_LOCK;
    if (!(allow & A_SCRIPTS)) flags |= SANDBOX_SCRIPTS | SANDBOX_AUTOMATIC_FEATURES;
    if (!(allow & A_ESCAPE)) flags |= SANDBOX_PROPAGATES_TO_AUXILIARY;
    if (!(allow & A_MODALS)) flags |= SANDBOX_MODALS;
    if (!(allow & A_ORIENTATION)) flags |= SANDBOX_ORIENTATION_LOCK;
    if (!(allow & A_PRESENTATION)) flags |= SANDBOX_PRESENTATION;
    if (!(allow & A_DOWNLOADS)) flags |= SANDBOX_DOWNLOADS;
    if (!(allow & (A_POPUPS | A_TOP | A_TOP_USER | A_CUSTOM))) flags |= SANDBOX_CUSTOM_PROTOCOLS;
    if (!(allow & A_STORAGE)) flags |= SANDBOX_STORAGE_ACCESS;
    return flags;
}
static inline unsigned sandbox_parse(const char *value)
{ return sandbox_parse_n(value, (size_t)-1); }

/* CSP3: the union over every ENFORCED policy of the flags its FIRST `sandbox`
 * directive parses to (a later duplicate directive in one policy is ignored,
 * CSP3 2.2.1). `policies` is the header value list the loader joined with
 * '\n'; ',' also separates serialized policies. Directive names compare
 * ASCII case-insensitively. 0 = no sandbox directive at all. Never feed this
 * <meta> policies or Content-Security-Policy-Report-Only. */
static inline unsigned sandbox_from_csp(const char *policies)
{
    unsigned out = 0;
    const char *p = policies;
    while (p && *p) {
        const char *end = p;
        while (*end && *end != '\n' && *end != ',') end++;
        int found = 0;
        for (const char *d = p; d < end && !found;) {
            const char *de = d;
            while (de < end && *de != ';') de++;
            const char *name = d;
            while (name < de && sandbox_ascii_space((unsigned char)*name)) name++;
            const char *ne = name;
            while (ne < de && !sandbox_ascii_space((unsigned char)*ne)) ne++;
            if (sandbox_token_is(name, (size_t)(ne - name), "sandbox")) {
                out |= sandbox_parse_n(ne, (size_t)(de - ne));
                found = 1;
            }
            d = de < end ? de + 1 : de;
        }
        p = *end ? end + 1 : end;
    }
    return out;
}

/* Diagnostics only: the keywords the set still ALLOWS, space separated,
 * truncated safely. Never used to decide anything. */
static inline void sandbox_describe(unsigned flags, char *out, size_t cap)
{
    static const struct { unsigned bit; const char *name; } allow[] = {
        { SANDBOX_SCRIPTS, "allow-scripts" }, { SANDBOX_ORIGIN, "allow-same-origin" },
        { SANDBOX_FORMS, "allow-forms" }, { SANDBOX_AUXILIARY_NAVIGATION, "allow-popups" },
        { SANDBOX_MODALS, "allow-modals" }, { SANDBOX_TOP_NAVIGATION, "allow-top-navigation" },
    };
    size_t n = 0;
    if (!cap) return;
    out[0] = 0;
    if (!flags) { const char *s = "unsandboxed"; while (*s && n + 1 < cap) out[n++] = *s++; out[n] = 0; return; }
    for (size_t i = 0; i < sizeof allow / sizeof allow[0]; i++) {
        if (flags & allow[i].bit) continue;
        if (n && n + 1 < cap) out[n++] = ' ';
        for (const char *s = allow[i].name; *s && n + 1 < cap; s++) out[n++] = *s;
    }
    if (!n) { const char *s = "none"; while (*s && n + 1 < cap) out[n++] = *s++; }
    out[n] = 0;
}
#endif
