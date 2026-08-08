/* dom_serialize.c -- DOM subtree -> text.  See dom_serialize.h for the two
 * formats and why both live here.
 */

#include "dom_serialize.h"
#include "html_tokenizer.h"      /* enum html_tag, for the void-element set */

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* growable string                                                     */
/* ------------------------------------------------------------------ */
struct sb { char *p; size_t len, cap; int oom; };

static void sb_put(struct sb *s, const char *d, size_t n)
{
    if (s->oom) return;
    if (s->len + n + 1 > s->cap) {
        size_t c = s->cap ? s->cap : 256;
        while (c < s->len + n + 1) c *= 2;
        char *np = (char *)realloc(s->p, c);
        if (!np) { s->oom = 1; return; }
        s->p = np; s->cap = c;
    }
    if (n) memcpy(s->p + s->len, d, n);
    s->len += n;
    s->p[s->len] = 0;
}

static void sb_str(struct sb *s, const char *d) { sb_put(s, d, d ? strlen(d) : 0); }

static char *sb_finish(struct sb *s)
{
    if (s->oom) { free(s->p); return 0; }
    if (!s->p) sb_put(s, "", 0);
    return s->p;
}

/* ------------------------------------------------------------------ */
/* html5lib dump format                                                */
/* ------------------------------------------------------------------ */

static void ser_indent(struct sb *s, int depth)
{
    sb_str(s, "| ");
    for (int i = 0; i < depth; i++) sb_str(s, "  ");
}

/* Attribute order in the dump is by name -- the format treats attributes as a
 * set, so emitting them in source order would make identical trees compare
 * unequal.  Insertion sort over an index array: elements have a handful of
 * attributes, and this way the DOM's own order is left alone. */
static void ser_attrs(struct sb *s, const struct node *n, int depth)
{
    if (n->nattr <= 0) return;
    int *order = (int *)malloc(sizeof(int) * (size_t)n->nattr);
    if (!order) { s->oom = 1; return; }
    for (int i = 0; i < n->nattr; i++) order[i] = i;
    for (int i = 1; i < n->nattr; i++) {
        int v = order[i], j = i - 1;
        while (j >= 0 && strcmp(dom_attr_name_at(n, order[j]), dom_attr_name_at(n, v)) > 0) {
            order[j + 1] = order[j]; j--;
        }
        order[j + 1] = v;
    }
    for (int i = 0; i < n->nattr; i++) {
        ser_indent(s, depth);
        sb_str(s, dom_attr_name_at(n, order[i]));
        sb_str(s, "=\"");
        sb_put(s, n->attrs[order[i]].value, n->attrs[order[i]].vlen);
        sb_str(s, "\"\n");
    }
    free(order);
}

static void ser_node(struct sb *s, const struct node *n, int depth)
{
    switch (n->type) {
    case N_TEXT:
        ser_indent(s, depth);
        sb_str(s, "\"");
        sb_put(s, n->text ? n->text : "", n->text ? (size_t)n->textlen : 0);
        sb_str(s, "\"\n");
        return;

    case N_COMMENT:
        ser_indent(s, depth);
        sb_str(s, "<!-- ");
        sb_put(s, n->text ? n->text : "", n->text ? (size_t)n->textlen : 0);
        sb_str(s, " -->\n");
        return;

    case N_DOCTYPE: {
        ser_indent(s, depth);
        sb_str(s, "<!DOCTYPE ");
        sb_str(s, dom_doctype_name(n));
        /* "absent" and "empty" are different: <!DOCTYPE html> prints no
         * identifiers at all, <!DOCTYPE html SYSTEM "x"> prints both slots. */
        if (n->pubid || n->sysid) {
            sb_str(s, " \"");
            sb_str(s, n->pubid ? n->pubid : "");
            sb_str(s, "\" \"");
            sb_str(s, n->sysid ? n->sysid : "");
            sb_str(s, "\"");
        }
        sb_str(s, ">\n");
        return;
    }

    default: break;
    }

    ser_indent(s, depth);
    sb_str(s, "<");
    if (n->ns == NS_SVG)         sb_str(s, "svg ");
    else if (n->ns == NS_MATHML) sb_str(s, "math ");
    sb_str(s, n->tag);
    sb_str(s, ">\n");

    ser_attrs(s, n, depth + 1);

    /* DEVIATION (html_tree.c #2): our <template> holds its children directly
     * rather than in a DocumentFragment, so there is no separate node to print.
     * The dump still shows the "content" level, because the shape the format is
     * describing IS the spec's -- the only thing missing is the indirection. */
    int child_depth = depth + 1;
    if (n->type == N_ELEM && n->ns == NS_HTML && n->htag == HTAG_TEMPLATE) {
        ser_indent(s, depth + 1);
        sb_str(s, "content\n");
        child_depth = depth + 2;
    }

    for (const struct node *c = n->first_child; c; c = c->next)
        ser_node(s, c, child_depth);
}

char *dom_serialize_test(const struct node *root)
{
    struct sb s = { 0, 0, 0, 0 };
    if (root)
        for (const struct node *c = root->first_child; c; c = c->next)
            ser_node(&s, c, 0);
    return sb_finish(&s);
}

/* ------------------------------------------------------------------ */
/* HTML markup (innerHTML / outerHTML)                                 */
/* ------------------------------------------------------------------ */

/* The spec's void elements: no end tag, and no children to serialise.
 *
 * htag is only filled in by html_tree.c, so a node built through the DOM API
 * (document.createElement) reads as HTAG_UNKNOWN and has to be matched by
 * name. Both paths stay live: innerHTML serialises parsed and script-built
 * subtrees alike, and a page mixes the two freely. */
static int is_void_element(const struct node *n)
{
    if (n->ns != NS_HTML) return 0;
    switch (n->htag) {
    case HTAG_AREA: case HTAG_BASE: case HTAG_BASEFONT: case HTAG_BGSOUND:
    case HTAG_BR: case HTAG_COL: case HTAG_EMBED: case HTAG_FRAME:
    case HTAG_HR: case HTAG_IMG: case HTAG_INPUT: case HTAG_KEYGEN:
    case HTAG_LINK: case HTAG_META: case HTAG_PARAM: case HTAG_SOURCE:
    case HTAG_TRACK: case HTAG_WBR:
        return 1;
    case HTAG_UNKNOWN: break;
    default: return 0;
    }
    static const char *const V[] = { "area","base","basefont","bgsound","br","col",
        "embed","frame","hr","img","input","keygen","link","meta","param",
        "source","track","wbr", 0 };
    for (int i = 0; V[i]; i++) if (!strcmp(n->tag, V[i])) return 1;
    return 0;
}

/* Raw-text elements' children are emitted verbatim: escaping the contents of
 * <script> or <style> would change what they mean.
 *
 * <noscript> is on this list because it is only raw text when SCRIPTING IS
 * ENABLED, and this parser always has it enabled (test-html5lib skips the
 * corpus's 28 #script-off cases for the same reason). With scripting off a
 * <noscript> is parsed as ordinary markup and its text must be re-escaped --
 * so if a scripting-disabled parse ever becomes a mode here, this predicate
 * has to take the flag rather than assume it. serializing.html case 25 and
 * serializing-html-fragments/escaping.html pin both halves of that. */
static int is_rawtext(const struct node *n)
{
    if (n->ns != NS_HTML) return 0;
    switch (n->htag) {
    case HTAG_SCRIPT: case HTAG_STYLE: case HTAG_XMP: case HTAG_IFRAME:
    case HTAG_NOEMBED: case HTAG_NOFRAMES: case HTAG_PLAINTEXT:
    case HTAG_NOSCRIPT:
        return 1;
    default: break;
    }
    return !strcmp(n->tag, "script") || !strcmp(n->tag, "style");
}

/* NOT DONE ON PURPOSE, because it is the obvious-looking wrong answer.
 *
 * <pre>, <textarea> and <listing> swallow one leading U+000A when parsed, so
 * serialising one of them and re-parsing the result loses a newline. The
 * intuitive fix is to emit an extra newline after the start tag. This file
 * briefly did that. The spec says not to, in as many words:
 *
 *   "For historical reasons, this algorithm does not roundtrip an initial
 *    U+000A (LF) character in pre, textarea, or listing elements ... The HTML
 *    parser will drop such a character during parsing, but this algorithm does
 *    not serialize an extra U+000A (LF) character."
 *
 * So the round trip is lossy by design and the serialiser must be lossy the
 * same way every other engine is. initial-linefeed-pre.html and
 * serializing.html's "<pre> context starting with U+000A" cases pin it. */

/* "Escaping a string", HTML fragment serialisation, in the spec's own order:
 *
 *   1. "&"     -> "&amp;"
 *   2. U+00A0  -> "&nbsp;"
 *   3. "<"     -> "&lt;"
 *   4. ">"     -> "&gt;"
 *   5. attribute mode only: '"' -> "&quot;"
 *
 * Steps 3 and 4 are NOT conditional on the mode. An older edition of the spec
 * escaped "<" and ">" only outside attributes, this file implemented that, and
 * html/syntax/serializing-html-fragments/serializing-lt-gt.html exists purely
 * to pin the current rule -- its title is "Escape "<" and ">" in attribute
 * values when serializing". Getting this wrong is not cosmetic: an attribute
 * value holding "<" re-parses as a tag when the serialised text is fed back in,
 * which is how a serialise/re-parse round trip turns into markup injection.
 *
 * On U+00A0. The comment that used to sit here declined to escape it, on the
 * grounds that our text is UTF-8 and finding a no-break space would mean
 * decoding UTF-8 to avoid mangling other C2-led characters. That reasoning was
 * wrong. UTF-8 is self-synchronising: the byte pair C2 A0 IS U+00A0 and can be
 * nothing else, because A0 is a continuation byte and only ever follows the
 * lead byte it belongs to. Matching the two bytes literally needs no decoder
 * and cannot false-positive. */
static void esc_str(struct sb *s, const char *d, size_t n, int attr)
{
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        const char *rep = 0;
        size_t adv = 1;
        unsigned char c = (unsigned char)d[i];
        switch (c) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;";  break;
        case '>': rep = "&gt;";  break;
        case '"': if (attr) rep = "&quot;"; break;
        case 0xC2:
            if (i + 1 < n && (unsigned char)d[i + 1] == 0xA0) {
                rep = "&nbsp;"; adv = 2;
            }
            break;
        default: break;
        }
        if (!rep) { run++; continue; }
        if (run) sb_put(s, d + i - run, run);
        run = 0;
        sb_str(s, rep);
        i += adv - 1;
    }
    if (run) sb_put(s, d + n - run, run);
}

/* An attribute's "serialized name". dom.h stores the namespaced foreign
 * attributes under a space-separated form -- "xlink href", "xml lang",
 * "xmlns xlink" -- because a space is the one character an HTML attribute name
 * can never contain, so one interned name still identifies one attribute. The
 * html5lib dump format wants exactly that space form and ser_attrs() below
 * keeps printing it; HTML serialisation wants the prefixed form, so the space
 * becomes a colon here and nowhere else.
 *
 * "xmlns" itself is the exception the spec calls out: an attribute in the XMLNS
 * namespace whose local name is "xmlns" serialises as bare "xmlns", not
 * "xmlns:xmlns" -- which in this encoding is the stored name "xmlns xmlns". */
static void ser_attr_name(struct sb *s, const char *name)
{
    if (!name) return;
    const char *sp = strchr(name, ' ');
    if (!sp) { sb_str(s, name); return; }
    if (!strcmp(name, "xmlns xmlns")) { sb_str(s, "xmlns"); return; }
    sb_put(s, name, (size_t)(sp - name));
    sb_str(s, ":");
    sb_str(s, sp + 1);
}

static void ser_html(struct sb *s, const struct node *n, int self);

static void ser_html_children(struct sb *s, const struct node *n)
{
    for (const struct node *c = n->first_child; c; c = c->next) ser_html(s, c, 1);
}

static void ser_html(struct sb *s, const struct node *n, int self)
{
    if (!self) { ser_html_children(s, n); return; }

    switch (n->type) {
    case N_TEXT: {
        const struct node *p = n->parent;
        if (p && p->type == N_ELEM && is_rawtext(p))
            sb_put(s, n->text ? n->text : "", n->text ? (size_t)n->textlen : 0);
        else
            esc_str(s, n->text ? n->text : "", (size_t)(n->text ? n->textlen : 0), 0);
        return;
    }
    case N_COMMENT:
        sb_str(s, "<!--");
        sb_put(s, n->text ? n->text : "", n->text ? (size_t)n->textlen : 0);
        sb_str(s, "-->");
        return;
    case N_DOCTYPE:
        sb_str(s, "<!DOCTYPE ");
        sb_str(s, dom_doctype_name(n));
        sb_str(s, ">");
        return;
    case N_DOCUMENT:
        ser_html_children(s, n);
        return;
    default: break;
    }

    sb_str(s, "<");
    sb_str(s, n->tag);
    for (int i = 0; i < n->nattr; i++) {
        sb_str(s, " ");
        ser_attr_name(s, dom_attr_name_at(n, i));
        sb_str(s, "=\"");
        esc_str(s, n->attrs[i].value, (size_t)n->attrs[i].vlen, 1);
        sb_str(s, "\"");
    }
    sb_str(s, ">");

    if (is_void_element(n)) return;               /* no children, no end tag */

    ser_html_children(s, n);

    sb_str(s, "</");
    sb_str(s, n->tag);
    sb_str(s, ">");
}

char *dom_serialize_html(const struct node *n, int include_self)
{
    struct sb s = { 0, 0, 0, 0 };
    if (n) ser_html(&s, n, include_self);
    return sb_finish(&s);
}
