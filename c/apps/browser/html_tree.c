/* html_tree.c -- HTML5 tree construction.  See html_tree.h for why this file
 * tracks the spec's structure so literally.
 *
 * Map of the file:
 *
 *   tag sets            the spec's "special"/formatting/scope/implied-end-tag
 *                       sets, as one flag byte per enum html_tag.  Written as
 *                       strcmp chains these are unreadable AND O(n) per
 *                       element; as a table they are one array index.
 *   stack + AFE         the stack of open elements, the list of active
 *                       formatting elements (with markers and Noah's Ark), and
 *                       the insertion primitives every mode is written over.
 *   adoption agency     the full algorithm, step by numbered step.
 *   insertion modes     one function each, in spec order.
 *   foreign content     SVG/MathML name+attribute case fixups, integration
 *                       points, the breakout set.
 *   entry points        html_parse / html_parse_fragment.
 *
 * DELIBERATE DEVIATIONS, all marked "DEVIATION:" at the point they happen:
 *   1. Open-element depth is capped at DOM_MAX_TREE_DEPTH (512).
 *   2. <template> holds its children directly instead of in a DocumentFragment.
 *   3. Scripts are never executed (no document.write, no re-entrant parsing).
 *   4. Parse errors are not reported -- nothing above consumes them.
 */

#include "html_tree.h"
#include "html_tokenizer.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* small helpers                                                             */
/* ------------------------------------------------------------------------ */

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* The spec's "ASCII whitespace" for tree construction.  CR never reaches us --
 * the tokenizer's input preprocessing already folded CRLF/CR to LF -- but the
 * set is written out in full so this predicate means what it says. */
static int is_ws(int c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

static int ci_eq_z(const char *a, const char *b)     /* b is a lowercase literal */
{
    while (*b) { if (lc((unsigned char)*a) != (unsigned char)*b) return 0; a++; b++; }
    return *a == 0;
}

static int ci_eq_n(const char *a, int alen, const char *b)  /* b lowercase literal */
{
    int i = 0;
    for (i = 0; i < alen; i++) {
        if (!b[i] || lc((unsigned char)a[i]) != (unsigned char)b[i]) return 0;
    }
    return b[i] == 0;
}

static int ci_prefix(const char *s, const char *pfx)  /* pfx compared case-insensitively */
{
    while (*pfx) { if (!*s || lc((unsigned char)*s) != lc((unsigned char)*pfx)) return 0; s++; pfx++; }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* tag sets                                                                  */
/* ------------------------------------------------------------------------ */
/* One byte per enum html_tag.  HTAG_UNKNOWN (index 0) is all-zero, which is
 * exactly right: a custom element is in none of these sets, and that single
 * fact is what makes <my-widget> behave like <span> without any special code. */

enum {
    TF_SPECIAL    = 0x01,   /* the "special" category */
    TF_FORMAT     = 0x02,   /* the formatting elements (adoption agency) */
    TF_SCOPE      = 0x04,   /* base scope-stop set, HTML namespace */
    TF_HEADING    = 0x08,   /* h1..h6 */
    TF_IMPLIED    = 0x10,   /* "generate implied end tags" */
    TF_IMPLIED_T  = 0x20,   /* ...the extra members of "thoroughly" */
    TF_BREAKOUT   = 0x40    /* HTML start tags that break out of foreign content */
};

#define SP   TF_SPECIAL
#define FM   TF_FORMAT
#define SC   TF_SCOPE
#define HD   TF_HEADING
#define IM   TF_IMPLIED
#define IT   TF_IMPLIED_T
#define BO   TF_BREAKOUT

static const unsigned char TAGF[HTAG__COUNT] = {
    [HTAG_A]              = FM,
    [HTAG_ADDRESS]        = SP,
    [HTAG_APPLET]         = SP | SC,
    [HTAG_AREA]           = SP,
    [HTAG_ARTICLE]        = SP,
    [HTAG_ASIDE]          = SP,
    [HTAG_B]              = FM | BO,
    [HTAG_BASE]           = SP,
    [HTAG_BASEFONT]       = SP,
    [HTAG_BGSOUND]        = SP,
    [HTAG_BIG]            = FM | BO,
    [HTAG_BLOCKQUOTE]     = SP | BO,
    [HTAG_BODY]           = SP | BO,
    [HTAG_BR]             = SP | BO,
    [HTAG_BUTTON]         = SP,
    [HTAG_CAPTION]        = SP | SC | IT,
    [HTAG_CENTER]         = SP | BO,
    [HTAG_CODE]           = FM | BO,
    [HTAG_COL]            = SP,
    [HTAG_COLGROUP]       = SP | IT,
    [HTAG_DD]             = SP | IM | BO,
    [HTAG_DETAILS]        = SP,
    [HTAG_DIR]            = SP,
    [HTAG_DIV]            = SP | BO,
    [HTAG_DL]             = SP | BO,
    [HTAG_DT]             = SP | IM | BO,
    [HTAG_EM]             = FM | BO,
    [HTAG_EMBED]          = SP | BO,
    [HTAG_FIELDSET]       = SP,
    [HTAG_FIGCAPTION]     = SP,
    [HTAG_FIGURE]         = SP,
    [HTAG_FONT]           = FM,   /* breakout only when it carries colour/face/size */
    [HTAG_FOOTER]         = SP,
    [HTAG_FORM]           = SP,
    [HTAG_FRAME]          = SP,
    [HTAG_FRAMESET]       = SP,
    [HTAG_H1]             = SP | HD | BO,
    [HTAG_H2]             = SP | HD | BO,
    [HTAG_H3]             = SP | HD | BO,
    [HTAG_H4]             = SP | HD | BO,
    [HTAG_H5]             = SP | HD | BO,
    [HTAG_H6]             = SP | HD | BO,
    [HTAG_HEAD]           = SP | BO,
    [HTAG_HEADER]         = SP,
    [HTAG_HGROUP]         = SP,
    [HTAG_HR]             = SP | BO,
    [HTAG_HTML]           = SP | SC,
    [HTAG_I]              = FM | BO,
    [HTAG_IFRAME]         = SP,
    [HTAG_IMG]            = SP | BO,
    [HTAG_INPUT]          = SP,
    [HTAG_KEYGEN]         = SP,
    [HTAG_LI]             = SP | IM | BO,
    [HTAG_LINK]           = SP,
    [HTAG_LISTING]        = SP | BO,
    [HTAG_MAIN]           = SP,
    [HTAG_MARQUEE]        = SP | SC,
    [HTAG_MENU]           = SP | BO,
    [HTAG_META]           = SP | BO,
    [HTAG_NAV]            = SP,
    [HTAG_NOBR]           = FM | BO,
    [HTAG_NOEMBED]        = SP,
    [HTAG_NOFRAMES]       = SP,
    [HTAG_NOSCRIPT]       = SP,
    [HTAG_OBJECT]         = SP | SC,
    [HTAG_OL]             = SP | BO,
    [HTAG_OPTGROUP]       = IM,
    [HTAG_OPTION]         = IM,
    [HTAG_P]              = SP | IM | BO,
    [HTAG_PARAM]          = SP,
    [HTAG_PLAINTEXT]      = SP,
    [HTAG_PRE]            = SP | BO,
    [HTAG_RB]             = IM,
    [HTAG_RP]             = IM,
    [HTAG_RT]             = IM,
    [HTAG_RTC]            = IM,
    [HTAG_RUBY]           = BO,
    [HTAG_S]              = FM | BO,
    [HTAG_SCRIPT]         = SP,
    [HTAG_SEARCH]         = SP,
    [HTAG_SECTION]        = SP,
    /* NOT special: <select> may now contain flow content, so it must not stop
     * the adoption agency's furthest-block search -- otherwise
     * "<font><select><option>a</option></font></select>" reparents the select
     * out of the font instead of leaving the tree alone. */
    [HTAG_SELECT]         = 0,
    [HTAG_SMALL]          = FM | BO,
    [HTAG_SOURCE]         = SP,
    [HTAG_SPAN]           = BO,
    [HTAG_STRIKE]         = FM | BO,
    [HTAG_STRONG]         = FM | BO,
    [HTAG_STYLE]          = SP,
    [HTAG_SUB]            = BO,
    [HTAG_SUMMARY]        = SP,
    [HTAG_SUP]            = BO,
    [HTAG_TABLE]          = SP | SC | BO,
    [HTAG_TBODY]          = SP | IT,
    [HTAG_TD]             = SP | SC | IT,
    [HTAG_TEMPLATE]       = SP | SC,
    [HTAG_TEXTAREA]       = SP,
    [HTAG_TFOOT]          = SP | IT,
    [HTAG_TH]             = SP | SC | IT,
    [HTAG_THEAD]          = SP | IT,
    [HTAG_TITLE]          = SP,
    [HTAG_TR]             = SP | IT,
    [HTAG_TRACK]          = SP,
    [HTAG_TT]             = FM | BO,
    [HTAG_U]              = FM | BO,
    [HTAG_UL]             = SP | BO,
    [HTAG_VAR]            = BO,
    [HTAG_WBR]            = SP,
    [HTAG_XMP]            = SP
};

#undef SP
#undef FM
#undef SC
#undef HD
#undef IM
#undef IT
#undef BO

static unsigned tagf(uint16_t h) { return (h < HTAG__COUNT) ? TAGF[h] : 0u; }

/* MathML text integration points, and the MathML members of the "special" and
 * scope sets -- the same five element names, which is not a coincidence: they
 * are the MathML elements whose content is text rather than markup. */
static int is_mathml_text_ip_tag(uint16_t h)
{
    return h == HTAG_MI || h == HTAG_MO || h == HTAG_MN || h == HTAG_MS || h == HTAG_MTEXT;
}

static int is_special(const struct node *n)
{
    if (!n || n->type != N_ELEM) return 0;
    if (n->ns == NS_HTML)   return (tagf(n->htag) & TF_SPECIAL) != 0;
    if (n->ns == NS_MATHML) return is_mathml_text_ip_tag(n->htag) || n->htag == HTAG_ANNOTATION_XML;
    /* SVG */
    return n->htag == HTAG_FOREIGNOBJECT || n->htag == HTAG_DESC || n->htag == HTAG_TITLE;
}

/* ------------------------------------------------------------------------ */
/* the parser state                                                          */
/* ------------------------------------------------------------------------ */

enum {
    M_INITIAL = 0, M_BEFORE_HTML, M_BEFORE_HEAD, M_IN_HEAD, M_IN_HEAD_NOSCRIPT,
    M_AFTER_HEAD, M_IN_BODY, M_TEXT, M_IN_TABLE, M_IN_TABLE_TEXT, M_IN_CAPTION,
    M_IN_COLUMN_GROUP, M_IN_TABLE_BODY, M_IN_ROW, M_IN_CELL,
    M_IN_TEMPLATE, M_AFTER_BODY, M_IN_FRAMESET,
    M_AFTER_FRAMESET, M_AFTER_AFTER_BODY, M_AFTER_AFTER_FRAMESET
};

/* scope kinds for has_in_scope() */
enum { SK_DEFAULT = 0, SK_LIST, SK_BUTTON, SK_TABLE };

struct html_tb {
    struct html_tokenizer tok;
    struct dom_doc *doc;
    struct node *docroot;

    const char *src;
    int srclen;

    struct node **open;  int nopen,  opencap;   /* stack of open elements */
    struct node **afe;   int nafe,   afecap;    /* active formatting, NULL = marker */
    int *tmpl;           int ntmpl,  tmplcap;   /* stack of template insertion modes */

    int mode, orig_mode;
    int reprocess;
    int done;

    struct node *head_elem;
    struct node *form_elem;
    struct node *context;                       /* fragment: the context element */
    int fragment;
    int scripting;
    int frameset_ok;
    int foster;                                 /* foster parenting enabled */
    int ignore_lf;                              /* drop one leading LF */

    /* "in table text": characters seen while the current node was a table
     * element, held until we know whether the run is whitespace-only. */
    char *tbuf; int tlen, tcap;

    /* verbatim <svg> source span (node->raw) -- layout decodes SVG from the
     * source, not from the DOM, because the DOM lowercases nothing now but the
     * SVG parser wants the bytes it was given. */
    struct node *svg_root;
    uint32_t svg_start, cur_src_end;
};

#define CURNODE(tb) ((tb)->nopen ? (tb)->open[(tb)->nopen - 1] : (struct node *)0)

/* ------------------------------------------------------------------------ */
/* stack of open elements                                                    */
/* ------------------------------------------------------------------------ */

static int stack_push(struct html_tb *tb, struct node *n)
{
    if (tb->nopen == tb->opencap) {
        int c = tb->opencap ? tb->opencap * 2 : 32;
        struct node **p = (struct node **)realloc(tb->open, (size_t)c * sizeof *p);
        if (!p) return 0;
        tb->open = p; tb->opencap = c;
    }
    tb->open[tb->nopen++] = n;
    return 1;
}

static void stack_pop(struct html_tb *tb)
{
    if (!tb->nopen) return;
    struct node *n = tb->open[--tb->nopen];
    /* The outermost <svg> closes: hand layout the exact bytes between its '<'
     * and the end of the token that closed it. */
    if (n == tb->svg_root) {
        if (tb->src && tb->cur_src_end > tb->svg_start)
            dom_set_raw(n, tb->src + tb->svg_start, (int)(tb->cur_src_end - tb->svg_start));
        tb->svg_root = 0;
    }
}

static int stack_index(struct html_tb *tb, const struct node *n)
{
    for (int i = tb->nopen - 1; i >= 0; i--) if (tb->open[i] == n) return i;
    return -1;
}

static void stack_remove(struct html_tb *tb, struct node *n)
{
    int i = stack_index(tb, n);
    if (i < 0) return;
    if (i == tb->nopen - 1) { stack_pop(tb); return; }
    for (int k = i; k + 1 < tb->nopen; k++) tb->open[k] = tb->open[k + 1];
    tb->nopen--;
}

static int stack_insert_at(struct html_tb *tb, int idx, struct node *n)
{
    if (idx < 0 || idx > tb->nopen) return 0;
    if (!stack_push(tb, n)) return 0;              /* grow, then shift into place */
    for (int k = tb->nopen - 1; k > idx; k--) tb->open[k] = tb->open[k - 1];
    tb->open[idx] = n;
    return 1;
}

/* An HTML element with this tag id? (the spec's "an HTML element whose tag name
 * is X" -- namespace matters, which is the whole point of tests like
 * <svg><title> not closing an HTML <title>.) */
static int is_html_tag(const struct node *n, uint16_t h)
{
    return n && n->type == N_ELEM && n->ns == NS_HTML && n->htag == h;
}

static int node_matches_token(const struct node *n, const struct html_token *t)
{
    if (!n || n->type != N_ELEM) return 0;
    if (t->tag || n->htag) return n->htag == t->tag;
    /* Both unknown to the tag table: compare bytes, lowercasing the node's name
     * (foreign element names keep their source case). */
    const char *s = n->tag;
    uint32_t i;
    for (i = 0; i < t->namelen; i++)
        if (!s[i] || lc((unsigned char)s[i]) != (unsigned char)t->name[i]) return 0;
    return s[i] == 0;
}

static int stack_has_html_tag(struct html_tb *tb, uint16_t h)
{
    for (int i = tb->nopen - 1; i >= 0; i--) if (is_html_tag(tb->open[i], h)) return 1;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* the five scope predicates                                                 */
/* ------------------------------------------------------------------------ */
/* The four share one walk; they differ only in which elements STOP it.  The
 * base set is the same for default/list-item/button scope, which is why they
 * are one function with a kind rather than three copies that drift apart.
 *
 * TABLE scope is the odd one and getting it wrong is expensive: its stop set is
 * ONLY html/table/template in the HTML namespace -- the MathML/SVG members of
 * the base set are NOT in it.  Including them makes <td><svg><desc><td> fail to
 * see the enclosing cell, so the second <td> nests inside the SVG and every
 * later row lands in the wrong place. */
static int scope_stops(const struct node *n, int kind)
{
    if (kind == SK_TABLE)
        return n->ns == NS_HTML && (n->htag == HTAG_HTML || n->htag == HTAG_TABLE ||
                                    n->htag == HTAG_TEMPLATE);

    if (n->ns == NS_MATHML)
        return is_mathml_text_ip_tag(n->htag) || n->htag == HTAG_ANNOTATION_XML;
    if (n->ns == NS_SVG)
        return n->htag == HTAG_FOREIGNOBJECT || n->htag == HTAG_DESC || n->htag == HTAG_TITLE;

    if (kind == SK_LIST   && (n->htag == HTAG_OL || n->htag == HTAG_UL)) return 1;
    if (kind == SK_BUTTON && n->htag == HTAG_BUTTON) return 1;
    return (tagf(n->htag) & TF_SCOPE) != 0;
}

static int has_in_scope(struct html_tb *tb, uint16_t h, int kind)
{
    for (int i = tb->nopen - 1; i >= 0; i--) {
        struct node *n = tb->open[i];
        if (is_html_tag(n, h)) return 1;
        if (scope_stops(n, kind)) return 0;
    }
    return 0;
}

static int has_node_in_scope(struct html_tb *tb, struct node *target, int kind)
{
    for (int i = tb->nopen - 1; i >= 0; i--) {
        struct node *n = tb->open[i];
        if (n == target) return 1;
        if (scope_stops(n, kind)) return 0;
    }
    return 0;
}

/* "an element in scope that is an HTML element whose tag name is one of h1..h6" */
static int has_heading_in_scope(struct html_tb *tb)
{
    for (int i = tb->nopen - 1; i >= 0; i--) {
        struct node *n = tb->open[i];
        if (n->ns == NS_HTML && (tagf(n->htag) & TF_HEADING)) return 1;
        if (scope_stops(n, SK_DEFAULT)) return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* list of active formatting elements                                        */
/* ------------------------------------------------------------------------ */

static int afe_push_raw(struct html_tb *tb, struct node *n)
{
    if (tb->nafe == tb->afecap) {
        int c = tb->afecap ? tb->afecap * 2 : 16;
        struct node **p = (struct node **)realloc(tb->afe, (size_t)c * sizeof *p);
        if (!p) return 0;
        tb->afe = p; tb->afecap = c;
    }
    tb->afe[tb->nafe++] = n;
    return 1;
}

static void afe_insert_marker(struct html_tb *tb) { afe_push_raw(tb, 0); }

static int afe_last_marker(struct html_tb *tb)
{
    for (int i = tb->nafe - 1; i >= 0; i--) if (!tb->afe[i]) return i;
    return -1;
}

static void afe_remove_at(struct html_tb *tb, int i)
{
    if (i < 0 || i >= tb->nafe) return;
    for (int k = i; k + 1 < tb->nafe; k++) tb->afe[k] = tb->afe[k + 1];
    tb->nafe--;
}

static int afe_index(struct html_tb *tb, const struct node *n)
{
    if (!n) return -1;
    for (int i = tb->nafe - 1; i >= 0; i--) if (tb->afe[i] == n) return i;
    return -1;
}

static void afe_remove(struct html_tb *tb, struct node *n)
{
    afe_remove_at(tb, afe_index(tb, n));
}

static void afe_clear_to_marker(struct html_tb *tb)
{
    while (tb->nafe) {
        struct node *n = tb->afe[--tb->nafe];
        if (!n) return;                     /* the marker itself is removed too */
    }
}

/* Same tag name, namespace and attribute set -- the Noah's Ark comparison. */
static int elems_equal(const struct node *a, const struct node *b)
{
    if (a->ns != b->ns || a->name != b->name || a->nattr != b->nattr) return 0;
    for (int i = 0; i < a->nattr; i++) {
        int j;
        for (j = 0; j < b->nattr; j++) {
            if (a->attrs[i].name != b->attrs[j].name) continue;
            if (a->attrs[i].vlen != b->attrs[j].vlen) return 0;
            if (a->attrs[i].vlen &&
                memcmp(a->attrs[i].value, b->attrs[j].value, a->attrs[i].vlen)) return 0;
            break;
        }
        if (j == b->nattr) return 0;
    }
    return 1;
}

/* "Push onto the list of active formatting elements", with the Noah's Ark
 * clause: at most three otherwise-identical formatting elements may be live at
 * once.  Without it, <b><b><b>...  nests once per tag and the reconstruction
 * step re-creates all of them around every subsequent text run -- a page with a
 * few hundred stray <b>s becomes quadratic and then unrenderable. */
static void afe_push(struct html_tb *tb, struct node *el)
{
    int stop = afe_last_marker(tb);
    int count = 0, earliest = -1;
    for (int i = tb->nafe - 1; i > stop; i--) {
        if (!tb->afe[i]) break;
        if (elems_equal(tb->afe[i], el)) { count++; earliest = i; }
    }
    if (count >= 3 && earliest >= 0) afe_remove_at(tb, earliest);
    afe_push_raw(tb, el);
}

/* ------------------------------------------------------------------------ */
/* insertion primitives                                                      */
/* ------------------------------------------------------------------------ */

struct ins_place { struct node *parent, *before; };

static struct node *adjusted_current(struct html_tb *tb)
{
    if (tb->fragment && tb->nopen == 1) return tb->context;
    return CURNODE(tb);
}

/* "the appropriate place for inserting a node" */
static struct ins_place appropriate_place(struct html_tb *tb, struct node *override)
{
    struct ins_place p;
    struct node *target = override ? override : CURNODE(tb);
    p.parent = target;
    p.before = 0;
    if (!target) { p.parent = tb->docroot; return p; }

    if (tb->foster && target->ns == NS_HTML &&
        (target->htag == HTAG_TABLE || target->htag == HTAG_TBODY ||
         target->htag == HTAG_TFOOT || target->htag == HTAG_THEAD ||
         target->htag == HTAG_TR)) {
        int last_template = -1, last_table = -1;
        for (int i = tb->nopen - 1; i >= 0; i--) {
            if (last_template < 0 && is_html_tag(tb->open[i], HTAG_TEMPLATE)) last_template = i;
            if (last_table < 0 && is_html_tag(tb->open[i], HTAG_TABLE)) last_table = i;
            if (last_template >= 0 && last_table >= 0) break;
        }
        if (last_template >= 0 && (last_table < 0 || last_template > last_table)) {
            /* DEVIATION 2: a template has no separate contents fragment here,
             * so "inside the template's contents" is "inside the template". */
            p.parent = tb->open[last_template]; p.before = 0;
        } else if (last_table < 0) {
            p.parent = tb->open[0]; p.before = 0;         /* fragment case */
        } else if (tb->open[last_table]->parent) {
            p.parent = tb->open[last_table]->parent;
            p.before = tb->open[last_table];
        } else {
            p.parent = tb->open[last_table - 1]; p.before = 0;
        }
    }
    return p;
}

static void place_insert(struct ins_place p, struct node *n)
{
    if (!p.parent || !n) return;
    if (p.before) dom_insert_before(p.parent, n, p.before);
    else          dom_append_child(p.parent, n);
}

/* "Insert a character": append to the Text node immediately before the
 * insertion location if there is one, so a run broken across tokens by entity
 * expansion still produces ONE text node -- which is what the DOM, and every
 * expected tree in the corpus, says. */
static void insert_chars(struct html_tb *tb, const char *d, int n)
{
    if (n <= 0) return;
    struct ins_place p = appropriate_place(tb, 0);
    if (!p.parent) return;
    struct node *prev = p.before ? p.before->prev : p.parent->last_child;
    if (prev && prev->type == N_TEXT) { dom_text_append(prev, d, n); return; }
    struct node *t = dom_create_text(tb->doc, d, n);
    if (t) place_insert(p, t);
}

static void insert_comment_at(struct html_tb *tb, const struct html_token *t, struct node *parent)
{
    struct node *c = dom_create_comment(tb->doc, t->data ? t->data : "", (int)t->datalen);
    if (!c) return;
    if (parent) dom_append_child(parent, c);
    else place_insert(appropriate_place(tb, 0), c);
}

/* Build the element a start tag describes, without inserting it. */
static struct node *elem_for_token(struct html_tb *tb, const struct html_token *t, int ns)
{
    struct node *e = dom_create_element_ns(tb->doc, t->name, (int)t->namelen, ns);
    if (!e) return 0;
    e->htag = t->tag;
    if (t->self_closing) e->flags |= NF_SELF_CLOSED;
    for (int i = 0; i < t->nattr; i++)
        dom_set_attr_raw(e, t->attrs[i].n, (int)t->attrs[i].nl,
                         t->attrs[i].v, (int)t->attrs[i].vl);
    return e;
}

/* DEVIATION 1: the DOM has no depth limit, but its consumers -- css_engine's
 * style_node, layout_block, css_extra's walk -- recurse, on the browser's 8 MiB
 * ring-3 stack.  Past DOM_MAX_TREE_DEPTH we keep parsing (the stack of open
 * elements stays consistent, so end tags still match) but stop LINKING the new
 * element into the document: its subtree becomes an orphan that dies with the
 * arena.  Browsers cap in the same place and for the same reason; the
 * alternative is a stack overflow on a page nobody can render anyway. */
static struct node *insert_element(struct html_tb *tb, struct node *e)
{
    if (!e) return 0;
    if (tb->nopen < DOM_MAX_TREE_DEPTH) place_insert(appropriate_place(tb, 0), e);
    stack_push(tb, e);
    return e;
}

static struct node *insert_html_element(struct html_tb *tb, const struct html_token *t)
{
    return insert_element(tb, elem_for_token(tb, t, NS_HTML));
}

/* A start tag the spec conjures out of nothing (<head>, <body>, <tbody>, ...). */
static struct node *insert_fake(struct html_tb *tb, const char *name, uint16_t htag)
{
    struct node *e = dom_create_element_ns(tb->doc, name, -1, NS_HTML);
    if (!e) return 0;
    e->htag = htag;
    return insert_element(tb, e);
}

/* ------------------------------------------------------------------------ */
/* end-tag helpers                                                           */
/* ------------------------------------------------------------------------ */

static void generate_implied_end_tags(struct html_tb *tb, uint16_t except)
{
    while (tb->nopen) {
        struct node *n = CURNODE(tb);
        if (n->ns != NS_HTML) break;
        if (!(tagf(n->htag) & TF_IMPLIED)) break;
        if (n->htag == except) break;
        stack_pop(tb);
    }
}

static void generate_implied_end_tags_thoroughly(struct html_tb *tb)
{
    while (tb->nopen) {
        struct node *n = CURNODE(tb);
        if (n->ns != NS_HTML) break;
        if (!(tagf(n->htag) & (TF_IMPLIED | TF_IMPLIED_T))) break;
        stack_pop(tb);
    }
}

static void pop_until_html_tag(struct html_tb *tb, uint16_t h)
{
    while (tb->nopen) {
        struct node *n = CURNODE(tb);
        stack_pop(tb);
        if (is_html_tag(n, h)) break;
    }
}

static void pop_until_heading(struct html_tb *tb)
{
    while (tb->nopen) {
        struct node *n = CURNODE(tb);
        stack_pop(tb);
        if (n->ns == NS_HTML && (tagf(n->htag) & TF_HEADING)) break;
    }
}

static void close_p_element(struct html_tb *tb)
{
    generate_implied_end_tags(tb, HTAG_P);
    pop_until_html_tag(tb, HTAG_P);
}

/* "Reconstruct the active formatting elements": re-open, around the insertion
 * point, every formatting element that is still active but no longer on the
 * stack.  This is what makes <b>a<p>b</p> render "b" bold too. */
static void reconstruct_afe(struct html_tb *tb)
{
    if (!tb->nafe) return;
    struct node *last = tb->afe[tb->nafe - 1];
    if (!last || stack_index(tb, last) >= 0) return;

    int i = tb->nafe - 1;
    while (i > 0) {
        i--;
        if (!tb->afe[i] || stack_index(tb, tb->afe[i]) >= 0) { i++; break; }
    }
    for (; i < tb->nafe; i++) {
        struct node *clone = dom_clone_element(tb->afe[i]);
        if (!clone) break;
        insert_element(tb, clone);
        tb->afe[i] = clone;
    }
}

/* ------------------------------------------------------------------------ */
/* reset the insertion mode appropriately                                    */
/* ------------------------------------------------------------------------ */

static void reset_insertion_mode(struct html_tb *tb)
{
    int last = 0;
    for (int i = tb->nopen - 1; i >= 0; i--) {
        struct node *node = tb->open[i];
        if (i == 0) { last = 1; if (tb->fragment && tb->context) node = tb->context; }

        if (node->ns != NS_HTML) { if (last) { tb->mode = M_IN_BODY; return; } continue; }

        /* No "select" case: with the select insertion modes gone, a select on
         * the stack is not a mode of its own and the walk keeps going. */
        switch (node->htag) {
        case HTAG_TD: case HTAG_TH:
            if (!last) { tb->mode = M_IN_CELL; return; }
            break;
        case HTAG_TR:      tb->mode = M_IN_ROW; return;
        case HTAG_TBODY: case HTAG_THEAD: case HTAG_TFOOT:
                           tb->mode = M_IN_TABLE_BODY; return;
        case HTAG_CAPTION: tb->mode = M_IN_CAPTION; return;
        case HTAG_COLGROUP:tb->mode = M_IN_COLUMN_GROUP; return;
        case HTAG_TABLE:   tb->mode = M_IN_TABLE; return;
        case HTAG_TEMPLATE:
            tb->mode = tb->ntmpl ? tb->tmpl[tb->ntmpl - 1] : M_IN_BODY;
            return;
        case HTAG_HEAD:
            if (!last) { tb->mode = M_IN_HEAD; return; }
            break;
        case HTAG_BODY:    tb->mode = M_IN_BODY; return;
        case HTAG_FRAMESET:tb->mode = M_IN_FRAMESET; return;
        case HTAG_HTML:
            tb->mode = tb->head_elem ? M_AFTER_HEAD : M_BEFORE_HEAD;
            return;
        default: break;
        }
        if (last) { tb->mode = M_IN_BODY; return; }
    }
    tb->mode = M_IN_BODY;
}

/* ------------------------------------------------------------------------ */
/* the adoption agency algorithm                                             */
/* ------------------------------------------------------------------------ */
/* Implemented in full, step by numbered step.  A "simplified" version is worse
 * than none: it fails by silently reparenting the wrong nodes, so the page's
 * remaining content lands in a container that renders but is wrong, and there
 * is nothing in the output to point at.  Both html5lib adoption files must be
 * at 100% for this to be considered working.
 *
 * Returns 1 if the token was handled, 0 if the caller must fall through to the
 * "any other end tag" rules (step 5's "no such element"). */
static int adoption_agency(struct html_tb *tb, const struct html_token *t);
static void in_body_any_other_end_tag(struct html_tb *tb, const struct html_token *t);

static int adoption_agency(struct html_tb *tb, const struct html_token *t)
{
    /* 1. If the current node is an HTML element whose tag name is subject and
     *    it is not in the list of active formatting elements, pop it and return. */
    struct node *cur = CURNODE(tb);
    if (cur && cur->ns == NS_HTML && node_matches_token(cur, t) && afe_index(tb, cur) < 0) {
        stack_pop(tb);
        return 1;
    }

    for (int outer = 0; outer < 8; outer++) {
        /* 5. formattingElement = last entry after the last marker with this name */
        int fe_idx = -1;
        for (int i = tb->nafe - 1; i >= 0; i--) {
            if (!tb->afe[i]) break;                     /* hit the marker */
            if (tb->afe[i]->ns == NS_HTML && node_matches_token(tb->afe[i], t)) { fe_idx = i; break; }
        }
        if (fe_idx < 0) return 0;                       /* -> any other end tag */
        struct node *fe = tb->afe[fe_idx];

        /* 6. not on the stack: a stale entry, drop it. */
        int fe_stack = stack_index(tb, fe);
        if (fe_stack < 0) { afe_remove_at(tb, fe_idx); return 1; }

        /* 7. on the stack but not in scope: ignore the token. */
        if (!has_node_in_scope(tb, fe, SK_DEFAULT)) return 1;

        /* 9. furthestBlock = topmost special element BELOW formattingElement */
        int fb_idx = -1;
        for (int i = fe_stack + 1; i < tb->nopen; i++)
            if (is_special(tb->open[i])) { fb_idx = i; break; }

        /* 10. no furthestBlock: this is the common case (well-nested markup) --
         *     pop through formattingElement and drop it from the list. */
        if (fb_idx < 0) {
            while (tb->nopen > fe_stack) stack_pop(tb);
            afe_remove_at(tb, fe_idx);
            return 1;
        }

        struct node *fb = tb->open[fb_idx];
        struct node *common = tb->open[fe_stack - 1];   /* 11 */
        int bookmark = fe_idx;                          /* 12 */

        /* 13/14 */
        struct node *node = fb, *last_node = fb;
        int node_idx = fb_idx;

        for (int inner = 1; ; inner++) {
            /* 16. the element above node -- by INDEX, because step 19 removes
             *     entries from the stack under us and the spec explicitly means
             *     "where node used to be". */
            node_idx--;
            if (node_idx < 0) break;                    /* defensive; step 17 fires first */
            node = tb->open[node_idx];

            if (node == fe) break;                      /* 17 */

            int a_idx = afe_index(tb, node);
            if (inner > 3 && a_idx >= 0) {              /* 18 */
                afe_remove_at(tb, a_idx);
                if (a_idx < bookmark) bookmark--;
                a_idx = -1;
            }
            if (a_idx < 0) {                            /* 19 */
                for (int k = node_idx; k + 1 < tb->nopen; k++) tb->open[k] = tb->open[k + 1];
                tb->nopen--;
                if (node_idx < fb_idx) fb_idx--;
                continue;
            }

            /* 20. clone node, replacing it in BOTH lists */
            struct node *clone = dom_clone_element(node);
            if (!clone) break;
            tb->afe[a_idx] = clone;
            tb->open[node_idx] = clone;
            node = clone;

            /* 21 */
            if (last_node == fb) bookmark = a_idx + 1;

            /* 22 */
            dom_append_child(node, last_node);

            /* 23 */
            last_node = node;
        }

        /* 25. Insert lastNode at commonAncestor.  html5lib (which generated the
         * expected trees) foster parents here whenever commonAncestor is a
         * table-ish element, independently of the foster-parenting flag -- and
         * so does every shipping browser, because otherwise a formatting
         * element that straddles a table drags content back inside it. */
        {
            int saved = tb->foster;
            tb->foster = 1;
            struct ins_place p = appropriate_place(tb, common);
            tb->foster = saved;
            place_insert(p, last_node);
        }

        /* 26-28 */
        struct node *fe_clone = dom_clone_element(fe);
        if (!fe_clone) return 1;
        while (fb->first_child) dom_append_child(fe_clone, fb->first_child);
        dom_append_child(fb, fe_clone);

        /* 29 */
        afe_remove(tb, fe);
        if (bookmark > tb->nafe) bookmark = tb->nafe;
        if (bookmark < 0) bookmark = 0;
        afe_push_raw(tb, 0);                            /* grow by one */
        for (int k = tb->nafe - 1; k > bookmark; k--) tb->afe[k] = tb->afe[k - 1];
        tb->afe[bookmark] = fe_clone;

        /* 30 */
        stack_remove(tb, fe);
        int fbi = stack_index(tb, fb);
        if (fbi < 0) stack_push(tb, fe_clone);
        else stack_insert_at(tb, fbi + 1, fe_clone);
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* foreign content: name and attribute fixups                                */
/* ------------------------------------------------------------------------ */
/* SVG and MathML are case-sensitive XML vocabularies reached through a
 * case-insensitive HTML tokenizer, so the spec carries literal tables mapping
 * the lowercased name back to the real one.  There is no rule to derive them
 * from -- "feGaussianBlur" is just what it is called. */

struct namefix { const char *from, *to; };

static const struct namefix SVG_TAGS[] = {   /* sorted by `from` */
    {"altglyph","altGlyph"}, {"altglyphdef","altGlyphDef"}, {"altglyphitem","altGlyphItem"},
    {"animatecolor","animateColor"}, {"animatemotion","animateMotion"},
    {"animatetransform","animateTransform"}, {"clippath","clipPath"},
    {"feblend","feBlend"}, {"fecolormatrix","feColorMatrix"},
    {"fecomponenttransfer","feComponentTransfer"}, {"fecomposite","feComposite"},
    {"feconvolvematrix","feConvolveMatrix"}, {"fediffuselighting","feDiffuseLighting"},
    {"fedisplacementmap","feDisplacementMap"}, {"fedistantlight","feDistantLight"},
    {"fedropshadow","feDropShadow"}, {"feflood","feFlood"}, {"fefunca","feFuncA"},
    {"fefuncb","feFuncB"}, {"fefuncg","feFuncG"}, {"fefuncr","feFuncR"},
    {"fegaussianblur","feGaussianBlur"}, {"feimage","feImage"}, {"femerge","feMerge"},
    {"femergenode","feMergeNode"}, {"femorphology","feMorphology"}, {"feoffset","feOffset"},
    {"fepointlight","fePointLight"}, {"fespecularlighting","feSpecularLighting"},
    {"fespotlight","feSpotLight"}, {"fetile","feTile"}, {"feturbulence","feTurbulence"},
    {"foreignobject","foreignObject"}, {"glyphref","glyphRef"},
    {"lineargradient","linearGradient"}, {"radialgradient","radialGradient"},
    {"textpath","textPath"}
};

static const struct namefix SVG_ATTRS[] = {  /* sorted by `from` */
    {"attributename","attributeName"}, {"attributetype","attributeType"},
    {"basefrequency","baseFrequency"}, {"baseprofile","baseProfile"},
    {"calcmode","calcMode"}, {"clippathunits","clipPathUnits"},
    {"diffuseconstant","diffuseConstant"}, {"edgemode","edgeMode"},
    {"filterunits","filterUnits"}, {"glyphref","glyphRef"},
    {"gradienttransform","gradientTransform"}, {"gradientunits","gradientUnits"},
    {"kernelmatrix","kernelMatrix"}, {"kernelunitlength","kernelUnitLength"},
    {"keypoints","keyPoints"}, {"keysplines","keySplines"}, {"keytimes","keyTimes"},
    {"lengthadjust","lengthAdjust"}, {"limitingconeangle","limitingConeAngle"},
    {"markerheight","markerHeight"}, {"markerunits","markerUnits"},
    {"markerwidth","markerWidth"}, {"maskcontentunits","maskContentUnits"},
    {"maskunits","maskUnits"}, {"numoctaves","numOctaves"}, {"pathlength","pathLength"},
    {"patterncontentunits","patternContentUnits"}, {"patterntransform","patternTransform"},
    {"patternunits","patternUnits"}, {"pointsatx","pointsAtX"}, {"pointsaty","pointsAtY"},
    {"pointsatz","pointsAtZ"}, {"preservealpha","preserveAlpha"},
    {"preserveaspectratio","preserveAspectRatio"}, {"primitiveunits","primitiveUnits"},
    {"refx","refX"}, {"refy","refY"}, {"repeatcount","repeatCount"},
    {"repeatdur","repeatDur"}, {"requiredextensions","requiredExtensions"},
    {"requiredfeatures","requiredFeatures"}, {"specularconstant","specularConstant"},
    {"specularexponent","specularExponent"}, {"spreadmethod","spreadMethod"},
    {"startoffset","startOffset"}, {"stddeviation","stdDeviation"},
    {"stitchtiles","stitchTiles"}, {"surfacescale","surfaceScale"},
    {"systemlanguage","systemLanguage"}, {"tablevalues","tableValues"},
    {"targetx","targetX"}, {"targety","targetY"}, {"textlength","textLength"},
    {"viewbox","viewBox"}, {"viewtarget","viewTarget"},
    {"xchannelselector","xChannelSelector"}, {"ychannelselector","yChannelSelector"},
    {"zoomandpan","zoomAndPan"}
};

/* The namespaced attributes.  html5lib serialises a namespaced attribute as
 * "prefix localname", and we store exactly that string as the attribute's name
 * (a space cannot occur in an HTML attribute name, so it stays unambiguous).
 * Carrying a real namespace field on every attribute would cost 8 bytes per
 * attribute document-wide to represent seven names that only appear inside
 * foreign content. */
static const struct namefix FOREIGN_ATTRS[] = {  /* sorted by `from` */
    {"xlink:actuate","xlink actuate"}, {"xlink:arcrole","xlink arcrole"},
    {"xlink:href","xlink href"}, {"xlink:role","xlink role"},
    {"xlink:show","xlink show"}, {"xlink:title","xlink title"},
    {"xlink:type","xlink type"}, {"xml:lang","xml lang"}, {"xml:space","xml space"},
    {"xmlns","xmlns"}, {"xmlns:xlink","xmlns xlink"}
};

static const char *fix_lookup(const struct namefix *tab, int n, const char *name, uint32_t len)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *f = tab[mid].from;
        uint32_t i = 0;
        int c = 0;
        for (i = 0; i < len && f[i]; i++) {
            if ((unsigned char)name[i] != (unsigned char)f[i]) {
                c = (unsigned char)name[i] < (unsigned char)f[i] ? -1 : 1;
                break;
            }
        }
        if (!c) {
            if (i == len && !f[i]) return tab[mid].to;
            c = (i == len) ? -1 : 1;
        }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return 0;
}

#define NFIX(t) ((int)(sizeof (t) / sizeof (t)[0]))

/* Attribute names are already lowercase (the tokenizer lowercases them), so the
 * fixups are a straight table lookup on the token's bytes. */
static void set_foreign_attrs(struct node *e, const struct html_token *t, int ns)
{
    for (int i = 0; i < t->nattr; i++) {
        const char *n = t->attrs[i].n;
        uint32_t nl = t->attrs[i].nl;
        const char *fixed = fix_lookup(FOREIGN_ATTRS, NFIX(FOREIGN_ATTRS), n, nl);
        if (!fixed && ns == NS_SVG) fixed = fix_lookup(SVG_ATTRS, NFIX(SVG_ATTRS), n, nl);
        if (!fixed && ns == NS_MATHML && nl == 13 && !memcmp(n, "definitionurl", 13))
            fixed = "definitionURL";
        if (fixed) dom_set_attr_raw(e, fixed, (int)strlen(fixed), t->attrs[i].v, (int)t->attrs[i].vl);
        else       dom_set_attr_raw(e, n, (int)nl, t->attrs[i].v, (int)t->attrs[i].vl);
    }
}

static struct node *insert_foreign_element(struct html_tb *tb, const struct html_token *t, int ns)
{
    const char *name = t->name;
    int namelen = (int)t->namelen;
    if (ns == NS_SVG) {
        const char *fixed = fix_lookup(SVG_TAGS, NFIX(SVG_TAGS), t->name, t->namelen);
        if (fixed) { name = fixed; namelen = (int)strlen(fixed); }
    }
    struct node *e = dom_create_element_ns(tb->doc, name, namelen, ns);
    if (!e) return 0;
    e->htag = t->tag;
    if (t->self_closing) e->flags |= NF_SELF_CLOSED;
    set_foreign_attrs(e, t, ns);
    return insert_element(tb, e);
}

static int is_html_integration_point(const struct node *n)
{
    if (!n || n->type != N_ELEM) return 0;
    if (n->ns == NS_SVG)
        return n->htag == HTAG_FOREIGNOBJECT || n->htag == HTAG_DESC || n->htag == HTAG_TITLE;
    if (n->ns == NS_MATHML && n->htag == HTAG_ANNOTATION_XML) {
        const char *enc = dom_attr(n, "encoding");
        return enc && (ci_eq_z(enc, "text/html") || ci_eq_z(enc, "application/xhtml+xml"));
    }
    return 0;
}

static int is_mathml_text_ip(const struct node *n)
{
    return n && n->type == N_ELEM && n->ns == NS_MATHML && is_mathml_text_ip_tag(n->htag);
}

static int tok_has_attr(const struct html_token *t, const char *lname)
{
    for (int i = 0; i < t->nattr; i++)
        if (ci_eq_n(t->attrs[i].n, (int)t->attrs[i].nl, lname)) return 1;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* quirks mode                                                               */
/* ------------------------------------------------------------------------ */

static const char *const QUIRKS_PREFIXES[] = {
    "+//Silmaril//dtd html Pro v0r11 19970101//",
    "-//AS//DTD HTML 3.0 asWedit + extensions//",
    "-//AdvaSoft Ltd//DTD HTML 3.0 asWedit + extensions//",
    "-//IETF//DTD HTML 2.0 Level 1//", "-//IETF//DTD HTML 2.0 Level 2//",
    "-//IETF//DTD HTML 2.0 Strict Level 1//", "-//IETF//DTD HTML 2.0 Strict Level 2//",
    "-//IETF//DTD HTML 2.0 Strict//", "-//IETF//DTD HTML 2.0//",
    "-//IETF//DTD HTML 2.1E//", "-//IETF//DTD HTML 3.0//",
    "-//IETF//DTD HTML 3.2 Final//", "-//IETF//DTD HTML 3.2//",
    "-//IETF//DTD HTML 3//", "-//IETF//DTD HTML Level 0//",
    "-//IETF//DTD HTML Level 1//", "-//IETF//DTD HTML Level 2//",
    "-//IETF//DTD HTML Level 3//", "-//IETF//DTD HTML Strict Level 0//",
    "-//IETF//DTD HTML Strict Level 1//", "-//IETF//DTD HTML Strict Level 2//",
    "-//IETF//DTD HTML Strict Level 3//", "-//IETF//DTD HTML Strict//",
    "-//IETF//DTD HTML//", "-//Metrius//DTD Metrius Presentational//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 2.0 Tables//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 3.0 Tables//",
    "-//Netscape Comm. Corp.//DTD HTML//", "-//Netscape Comm. Corp.//DTD Strict HTML//",
    "-//O'Reilly and Associates//DTD HTML 2.0//",
    "-//O'Reilly and Associates//DTD HTML Extended 1.0//",
    "-//O'Reilly and Associates//DTD HTML Extended Relaxed 1.0//",
    "-//SQ//DTD HTML 2.0 HoTMetaL + extensions//",
    "-//SoftQuad Software//DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0//",
    "-//SoftQuad//DTD HoTMetaL PRO 4.0::19971010::extensions to HTML 4.0//",
    "-//Spyglass//DTD HTML 2.0 Extended//",
    "-//Sun Microsystems Corp.//DTD HotJava HTML//",
    "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//",
    "-//W3C//DTD HTML 3 1995-03-24//", "-//W3C//DTD HTML 3.2 Draft//",
    "-//W3C//DTD HTML 3.2 Final//", "-//W3C//DTD HTML 3.2//",
    "-//W3C//DTD HTML 3.2S Draft//", "-//W3C//DTD HTML 4.0 Frameset//",
    "-//W3C//DTD HTML 4.0 Transitional//", "-//W3C//DTD HTML Experimental 19960712//",
    "-//W3C//DTD HTML Experimental 970421//", "-//W3C//DTD W3 HTML//",
    "-//W3O//DTD W3 HTML 3.0//", "-//WebTechs//DTD Mozilla HTML 2.0//",
    "-//WebTechs//DTD Mozilla HTML//"
};

static int doctype_quirks(const struct html_token *t, char *pub, char *sys)
{
    if (t->force_quirks) return QM_QUIRKS;
    if (!t->has_name || t->namelen != 4 || memcmp(t->name, "html", 4)) return QM_QUIRKS;

    if (t->has_pubid) {
        if (ci_eq_z(pub, "-//w3o//dtd w3 html strict 3.0//en//") ||
            ci_eq_z(pub, "-/w3c/dtd html 4.0 transitional/en") ||
            ci_eq_z(pub, "html"))
            return QM_QUIRKS;
        for (unsigned i = 0; i < sizeof QUIRKS_PREFIXES / sizeof QUIRKS_PREFIXES[0]; i++)
            if (ci_prefix(pub, QUIRKS_PREFIXES[i])) return QM_QUIRKS;
    }
    if (t->has_sysid &&
        ci_eq_z(sys, "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd"))
        return QM_QUIRKS;

    if (t->has_pubid) {
        int limited = ci_prefix(pub, "-//W3C//DTD XHTML 1.0 Frameset//") ||
                      ci_prefix(pub, "-//W3C//DTD XHTML 1.0 Transitional//");
        int h401 = ci_prefix(pub, "-//W3C//DTD HTML 4.01 Frameset//") ||
                   ci_prefix(pub, "-//W3C//DTD HTML 4.01 Transitional//");
        if (h401) return t->has_sysid ? QM_LIMITED_QUIRKS : QM_QUIRKS;
        if (limited) return QM_LIMITED_QUIRKS;
    }
    return QM_NO_QUIRKS;
}

/* ------------------------------------------------------------------------ */
/* the dispatcher                                                            */
/* ------------------------------------------------------------------------ */

static void tb_mode(struct html_tb *tb, struct html_token *t);
static void in_body(struct html_tb *tb, struct html_token *t);
static void in_head(struct html_tb *tb, struct html_token *t);
static void in_table(struct html_tb *tb, struct html_token *t);
static void mode_in_template(struct html_tb *tb, struct html_token *t);
static void foreign_content(struct html_tb *tb, struct html_token *t);
static void flush_table_text(struct html_tb *tb);

static void tb_process(struct html_tb *tb, struct html_token *t)
{
    /* Reprocessing chains are short (initial -> ... -> in body is the longest,
     * at five hops); the bound only exists so a future bug cannot wedge the
     * parser in an infinite loop on somebody's page. */
    for (int guard = 0; guard < 64; guard++) {
        tb->reprocess = 0;

        struct node *acn = adjusted_current(tb);
        int use_mode = 0;
        if (!tb->nopen || !acn || acn->ns == NS_HTML || t->type == TOK_EOF) {
            use_mode = 1;
        } else if (is_mathml_text_ip(acn)) {
            if (t->type == TOK_CHARS) use_mode = 1;
            else if (t->type == TOK_START && t->tag != HTAG_MGLYPH && t->tag != HTAG_MALIGNMARK)
                use_mode = 1;
        } else if (acn->ns == NS_MATHML && acn->htag == HTAG_ANNOTATION_XML &&
                   t->type == TOK_START && t->tag == HTAG_SVG) {
            use_mode = 1;
        } else if (is_html_integration_point(acn)) {
            if (t->type == TOK_CHARS || t->type == TOK_START) use_mode = 1;
        }

        if (use_mode) tb_mode(tb, t);
        else          foreign_content(tb, t);

        if (!tb->reprocess) return;
    }
}

/* ------------------------------------------------------------------------ */
/* character-token helpers                                                   */
/* ------------------------------------------------------------------------ */

static int ws_prefix_len(const char *d, int n)
{
    int i = 0;
    while (i < n && is_ws((unsigned char)d[i])) i++;
    return i;
}

static int all_ws(const char *d, int n)
{
    return ws_prefix_len(d, n) == n;
}

/* Insert every whitespace character of the run and drop the rest.  The frameset
 * modes say "insert the character" per whitespace character and "parse error,
 * ignore" for the others, so " te st" contributes BOTH spaces -- taking only
 * the leading whitespace run would silently eat the second one. */
static void insert_ws_only(struct html_tb *tb, const char *d, int n)
{
    int i = 0;
    while (i < n) {
        int s = i;
        while (i < n && is_ws((unsigned char)d[i])) i++;
        if (i > s) insert_chars(tb, d + s, i - s);
        while (i < n && !is_ws((unsigned char)d[i])) i++;
    }
}

/* Advance a character token past `k` bytes and ask for reprocessing of the
 * rest.  Modes like "after body" consume the whitespace and hand the first
 * non-whitespace character to a different mode; this is how. */
static void reprocess_rest(struct html_tb *tb, struct html_token *t, int k)
{
    t->data += k;
    t->datalen -= (uint32_t)k;
    tb->reprocess = 1;
}

/* "in body" character handling, also reached from several other modes. */
static void in_body_chars(struct html_tb *tb, const char *d, int n)
{
    int any = 0;
    for (int i = 0; i < n; i++) if (d[i]) { any = 1; break; }
    if (!any) return;                       /* U+0000 only: parse error, ignored */

    reconstruct_afe(tb);
    int i = 0;
    while (i < n) {
        int s = i;
        while (i < n && d[i]) i++;
        if (i > s) insert_chars(tb, d + s, i - s);
        while (i < n && !d[i]) i++;
    }
    for (int k = 0; k < n; k++)
        if (d[k] && !is_ws((unsigned char)d[k])) { tb->frameset_ok = 0; break; }
}

/* ------------------------------------------------------------------------ */
/* generic text element parsing                                              */
/* ------------------------------------------------------------------------ */

static void parse_text_element(struct html_tb *tb, struct html_token *t, int state)
{
    insert_html_element(tb, t);
    html_tok_set_state(&tb->tok, state);
    tb->orig_mode = tb->mode;
    tb->mode = M_TEXT;
}

/* ------------------------------------------------------------------------ */
/* "initial" / "before html" / "before head"                                 */
/* ------------------------------------------------------------------------ */

static void mode_initial(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS: {
        int k = ws_prefix_len(t->data, (int)t->datalen);
        if (k == (int)t->datalen) return;             /* whitespace: ignore */
        if (k) { reprocess_rest(tb, t, k); return; }
        break;
    }
    case TOK_COMMENT:
        insert_comment_at(tb, t, tb->docroot);
        return;
    case TOK_DOCTYPE: {
        /* dom_create_doctype takes NUL-terminated strings and distinguishes
         * "absent" (NULL) from "empty", which is exactly the has_* flags. */
        char *pub = 0, *sys = 0;
        if (t->has_pubid) { pub = (char *)malloc(t->pubidlen + 1);
                            if (pub) { memcpy(pub, t->pubid, t->pubidlen); pub[t->pubidlen] = 0; } }
        if (t->has_sysid) { sys = (char *)malloc(t->sysidlen + 1);
                            if (sys) { memcpy(sys, t->sysid, t->sysidlen); sys[t->sysidlen] = 0; } }
        char *nm = (char *)malloc(t->namelen + 1);
        if (nm) { memcpy(nm, t->name, t->namelen); nm[t->namelen] = 0; }
        struct node *dt = dom_create_doctype(tb->doc, nm ? nm : "", pub, sys);
        if (dt) dom_append_child(tb->docroot, dt);
        dom_doc_set_quirks(tb->doc, doctype_quirks(t, pub ? pub : (char *)"", sys ? sys : (char *)""));
        free(pub); free(sys); free(nm);
        tb->mode = M_BEFORE_HTML;
        return;
    }
    default: break;
    }
    dom_doc_set_quirks(tb->doc, QM_QUIRKS);
    tb->mode = M_BEFORE_HTML;
    tb->reprocess = 1;
}

static void mode_before_html(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_DOCTYPE: return;
    case TOK_COMMENT: insert_comment_at(tb, t, tb->docroot); return;
    case TOK_CHARS: {
        int k = ws_prefix_len(t->data, (int)t->datalen);
        if (k == (int)t->datalen) return;
        if (k) { reprocess_rest(tb, t, k); return; }
        break;
    }
    case TOK_START:
        if (t->tag == HTAG_HTML) {
            struct node *e = elem_for_token(tb, t, NS_HTML);
            if (e) { dom_append_child(tb->docroot, e); stack_push(tb, e); }
            tb->mode = M_BEFORE_HEAD;
            return;
        }
        break;
    case TOK_END:
        if (t->tag != HTAG_HEAD && t->tag != HTAG_BODY && t->tag != HTAG_HTML && t->tag != HTAG_BR)
            return;                                   /* parse error, ignore */
        break;
    default: break;
    }
    {
        struct node *e = dom_create_element_ns(tb->doc, "html", 4, NS_HTML);
        if (e) { e->htag = HTAG_HTML; dom_append_child(tb->docroot, e); stack_push(tb, e); }
    }
    tb->mode = M_BEFORE_HEAD;
    tb->reprocess = 1;
}

static void mode_before_head(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS: {
        int k = ws_prefix_len(t->data, (int)t->datalen);
        if (k == (int)t->datalen) return;
        if (k) { reprocess_rest(tb, t, k); return; }
        break;
    }
    case TOK_COMMENT: insert_comment_at(tb, t, 0); return;
    case TOK_DOCTYPE: return;
    case TOK_START:
        if (t->tag == HTAG_HTML) { in_body(tb, t); return; }
        if (t->tag == HTAG_HEAD) {
            tb->head_elem = insert_html_element(tb, t);
            tb->mode = M_IN_HEAD;
            return;
        }
        break;
    case TOK_END:
        if (t->tag != HTAG_HEAD && t->tag != HTAG_BODY && t->tag != HTAG_HTML && t->tag != HTAG_BR)
            return;
        break;
    default: break;
    }
    tb->head_elem = insert_fake(tb, "head", HTAG_HEAD);
    tb->mode = M_IN_HEAD;
    tb->reprocess = 1;
}

/* ------------------------------------------------------------------------ */
/* "in head" / "in head noscript" / "after head"                             */
/* ------------------------------------------------------------------------ */

static void in_head(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS: {
        /* Leading whitespace belongs to <head>; the first non-whitespace
         * character ends it, so trim what we inserted off the token and let it
         * fall through to "anything else" below. */
        int k = ws_prefix_len(t->data, (int)t->datalen);
        if (k) insert_chars(tb, t->data, k);
        if (k == (int)t->datalen) return;
        t->data += k;
        t->datalen -= (uint32_t)k;
        break;
    }
    case TOK_COMMENT: insert_comment_at(tb, t, 0); return;
    case TOK_DOCTYPE: return;
    case TOK_START:
        switch (t->tag) {
        case HTAG_HTML: in_body(tb, t); return;
        case HTAG_BASE: case HTAG_BASEFONT: case HTAG_BGSOUND: case HTAG_LINK:
        case HTAG_META:
            insert_html_element(tb, t);
            stack_pop(tb);
            return;
        case HTAG_TITLE:
            parse_text_element(tb, t, HTML_STATE_RCDATA);
            return;
        case HTAG_NOFRAMES: case HTAG_STYLE:
            parse_text_element(tb, t, HTML_STATE_RAWTEXT);
            return;
        case HTAG_NOSCRIPT:
            if (tb->scripting) parse_text_element(tb, t, HTML_STATE_RAWTEXT);
            else { insert_html_element(tb, t); tb->mode = M_IN_HEAD_NOSCRIPT; }
            return;
        case HTAG_SCRIPT:
            /* DEVIATION 3: the element is inserted and its text collected, but
             * never executed -- there is no script execution point in this
             * parser, so document.write() and parser-blocking scripts do not
             * exist.  browser.c runs the collected sources after the parse. */
            parse_text_element(tb, t, HTML_STATE_SCRIPT_DATA);
            return;
        case HTAG_TEMPLATE:
            insert_html_element(tb, t);
            afe_insert_marker(tb);
            tb->frameset_ok = 0;
            tb->mode = M_IN_TEMPLATE;
            if (tb->ntmpl == tb->tmplcap) {
                int c = tb->tmplcap ? tb->tmplcap * 2 : 8;
                int *p = (int *)realloc(tb->tmpl, (size_t)c * sizeof *p);
                if (p) { tb->tmpl = p; tb->tmplcap = c; }
            }
            if (tb->ntmpl < tb->tmplcap) tb->tmpl[tb->ntmpl++] = M_IN_TEMPLATE;
            return;
        case HTAG_HEAD: return;                       /* parse error, ignore */
        default: break;
        }
        break;
    case TOK_END:
        switch (t->tag) {
        case HTAG_HEAD:
            stack_pop(tb);
            tb->mode = M_AFTER_HEAD;
            return;
        case HTAG_BODY: case HTAG_HTML: case HTAG_BR:
            break;                                    /* -> anything else */
        case HTAG_TEMPLATE:
            if (!stack_has_html_tag(tb, HTAG_TEMPLATE)) return;
            generate_implied_end_tags_thoroughly(tb);
            pop_until_html_tag(tb, HTAG_TEMPLATE);
            afe_clear_to_marker(tb);
            if (tb->ntmpl) tb->ntmpl--;
            reset_insertion_mode(tb);
            return;
        default: return;                              /* parse error, ignore */
        }
        break;
    default: break;
    }
    stack_pop(tb);                                    /* pop the head element */
    tb->mode = M_AFTER_HEAD;
    tb->reprocess = 1;
}

static void mode_in_head_noscript(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_DOCTYPE: return;
    case TOK_COMMENT: in_head(tb, t); return;
    case TOK_CHARS: {
        int k = ws_prefix_len(t->data, (int)t->datalen);
        if (k) insert_chars(tb, t->data, k);
        if (k == (int)t->datalen) return;
        t->data += k; t->datalen -= (uint32_t)k;
        break;                                        /* -> anything else */
    }
    case TOK_START:
        switch (t->tag) {
        case HTAG_HTML: in_body(tb, t); return;
        case HTAG_BASEFONT: case HTAG_BGSOUND: case HTAG_LINK: case HTAG_META:
        case HTAG_NOFRAMES: case HTAG_STYLE:
            in_head(tb, t); return;
        case HTAG_HEAD: case HTAG_NOSCRIPT: return;
        default: break;
        }
        break;
    case TOK_END:
        if (t->tag == HTAG_NOSCRIPT) { stack_pop(tb); tb->mode = M_IN_HEAD; return; }
        if (t->tag == HTAG_BR) break;
        return;
    default: break;
    }
    stack_pop(tb);
    tb->mode = M_IN_HEAD;
    tb->reprocess = 1;
}

static void mode_after_head(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS: {
        int k = ws_prefix_len(t->data, (int)t->datalen);
        if (k) insert_chars(tb, t->data, k);
        if (k == (int)t->datalen) return;
        t->data += k; t->datalen -= (uint32_t)k;
        break;
    }
    case TOK_COMMENT: insert_comment_at(tb, t, 0); return;
    case TOK_DOCTYPE: return;
    case TOK_START:
        switch (t->tag) {
        case HTAG_HTML: in_body(tb, t); return;
        case HTAG_BODY:
            insert_html_element(tb, t);
            tb->frameset_ok = 0;
            tb->mode = M_IN_BODY;
            return;
        case HTAG_FRAMESET:
            insert_html_element(tb, t);
            tb->mode = M_IN_FRAMESET;
            return;
        case HTAG_BASE: case HTAG_BASEFONT: case HTAG_BGSOUND: case HTAG_LINK:
        case HTAG_META: case HTAG_NOFRAMES: case HTAG_SCRIPT: case HTAG_STYLE:
        case HTAG_TEMPLATE: case HTAG_TITLE: {
            /* Parse error: push head back so the in-head rules have somewhere
             * to insert, then take it out again wherever it ended up. */
            struct node *head = tb->head_elem;
            if (head) stack_push(tb, head);
            in_head(tb, t);
            if (head) stack_remove(tb, head);
            return;
        }
        case HTAG_HEAD: return;
        default: break;
        }
        break;
    case TOK_END:
        if (t->tag == HTAG_TEMPLATE) { in_head(tb, t); return; }
        if (t->tag == HTAG_BODY || t->tag == HTAG_HTML || t->tag == HTAG_BR) break;
        return;
    default: break;
    }
    insert_fake(tb, "body", HTAG_BODY);
    tb->mode = M_IN_BODY;
    tb->reprocess = 1;
}

/* ------------------------------------------------------------------------ */
/* "in body"                                                                 */
/* ------------------------------------------------------------------------ */

/* The block-level start tags that close an open <p> and nothing more. */
static int is_block_start(uint16_t h)
{
    switch (h) {
    case HTAG_ADDRESS: case HTAG_ARTICLE: case HTAG_ASIDE: case HTAG_BLOCKQUOTE:
    case HTAG_CENTER: case HTAG_DETAILS: case HTAG_DIALOG: case HTAG_DIR:
    case HTAG_DIV: case HTAG_DL: case HTAG_FIELDSET: case HTAG_FIGCAPTION:
    case HTAG_FIGURE: case HTAG_FOOTER: case HTAG_HEADER: case HTAG_HGROUP:
    case HTAG_MAIN: case HTAG_MENU: case HTAG_NAV: case HTAG_OL: case HTAG_P:
    case HTAG_SEARCH: case HTAG_SECTION: case HTAG_SUMMARY: case HTAG_UL:
        return 1;
    default: return 0;
    }
}

/* Same set plus the ones whose end tag also pops (button/listing/pre). */
static int is_block_end(uint16_t h)
{
    if (is_block_start(h) && h != HTAG_P) return 1;
    switch (h) {
    case HTAG_BUTTON: case HTAG_LISTING: case HTAG_PRE: return 1;
    default: return 0;
    }
}

/* Are we directly inside a <select>'s content, i.e. is the current node a
 * select, or an optgroup/option chain hanging off one?  That was the old "in
 * select" insertion mode's job; with the mode gone, the one rule that still
 * needs the answer (<input>, below) asks here.  In the fragment case the
 * context element stands in for the ancestors the stack does not have. */
static int in_select_content(struct html_tb *tb)
{
    if (tb->fragment && tb->nopen == 1 && tb->context) {
        struct node *c = tb->context;
        return c->ns == NS_HTML && (c->htag == HTAG_SELECT ||
                                    c->htag == HTAG_OPTGROUP || c->htag == HTAG_OPTION);
    }
    for (int i = tb->nopen - 1; i >= 0; i--) {
        struct node *n = tb->open[i];
        if (is_html_tag(n, HTAG_SELECT)) return 1;
        if (!(n->ns == NS_HTML && (n->htag == HTAG_OPTGROUP || n->htag == HTAG_OPTION)))
            return 0;
    }
    return 0;
}

static void in_body_any_other_end_tag(struct html_tb *tb, const struct html_token *t)
{
    for (int i = tb->nopen - 1; i >= 0; i--) {
        struct node *node = tb->open[i];
        if (node->ns == NS_HTML && node_matches_token(node, t)) {
            generate_implied_end_tags(tb, t->tag);
            /* Re-find: generate_implied_end_tags may have popped past i. */
            while (tb->nopen) {
                struct node *n = CURNODE(tb);
                stack_pop(tb);
                if (n == node) break;
            }
            return;
        }
        if (is_special(node)) return;                 /* parse error, ignore */
    }
}

static void in_body(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS:
        in_body_chars(tb, t->data, (int)t->datalen);
        return;

    case TOK_COMMENT: insert_comment_at(tb, t, 0); return;
    case TOK_DOCTYPE: return;

    case TOK_EOF:
        /* Straight to the in-template handler, NOT back through tb_mode(): the
         * insertion mode here is still "in body" (tmpl_switch left it that
         * way), so dispatching by mode would call this function again. */
        if (tb->ntmpl) { mode_in_template(tb, t); return; }
        tb->done = 1;
        return;

    case TOK_START:
        switch (t->tag) {
        case HTAG_HTML:
            if (stack_has_html_tag(tb, HTAG_TEMPLATE)) return;
            if (tb->nopen) {
                struct node *h = tb->open[0];
                for (int i = 0; i < t->nattr; i++) {
                    char nb[128];
                    uint32_t nl = t->attrs[i].nl;
                    if (nl >= sizeof nb) continue;
                    memcpy(nb, t->attrs[i].n, nl); nb[nl] = 0;
                    if (!dom_attr(h, nb))
                        dom_set_attr_raw(h, t->attrs[i].n, (int)nl, t->attrs[i].v, (int)t->attrs[i].vl);
                }
            }
            return;

        case HTAG_BASE: case HTAG_BASEFONT: case HTAG_BGSOUND: case HTAG_LINK:
        case HTAG_META: case HTAG_NOFRAMES: case HTAG_SCRIPT: case HTAG_STYLE:
        case HTAG_TEMPLATE: case HTAG_TITLE:
            in_head(tb, t);
            return;

        case HTAG_BODY:
            if (tb->nopen < 2 || !is_html_tag(tb->open[1], HTAG_BODY) ||
                stack_has_html_tag(tb, HTAG_TEMPLATE))
                return;
            tb->frameset_ok = 0;
            {
                struct node *b = tb->open[1];
                for (int i = 0; i < t->nattr; i++) {
                    char nb[128];
                    uint32_t nl = t->attrs[i].nl;
                    if (nl >= sizeof nb) continue;
                    memcpy(nb, t->attrs[i].n, nl); nb[nl] = 0;
                    if (!dom_attr(b, nb))
                        dom_set_attr_raw(b, t->attrs[i].n, (int)nl, t->attrs[i].v, (int)t->attrs[i].vl);
                }
            }
            return;

        case HTAG_FRAMESET:
            if (tb->nopen < 2 || !is_html_tag(tb->open[1], HTAG_BODY)) return;
            if (!tb->frameset_ok) return;
            if (tb->open[1]->parent) dom_remove_child(tb->open[1]->parent, tb->open[1]);
            while (tb->nopen > 1) stack_pop(tb);
            insert_html_element(tb, t);
            tb->mode = M_IN_FRAMESET;
            return;

        case HTAG_ADDRESS: case HTAG_ARTICLE: case HTAG_ASIDE: case HTAG_BLOCKQUOTE:
        case HTAG_CENTER: case HTAG_DETAILS: case HTAG_DIALOG: case HTAG_DIR:
        case HTAG_DIV: case HTAG_DL: case HTAG_FIELDSET: case HTAG_FIGCAPTION:
        case HTAG_FIGURE: case HTAG_FOOTER: case HTAG_HEADER: case HTAG_HGROUP:
        case HTAG_MAIN: case HTAG_MENU: case HTAG_NAV: case HTAG_OL: case HTAG_P:
        case HTAG_SEARCH: case HTAG_SECTION: case HTAG_SUMMARY: case HTAG_UL:
            if (has_in_scope(tb, HTAG_P, SK_BUTTON)) close_p_element(tb);
            insert_html_element(tb, t);
            return;

        case HTAG_H1: case HTAG_H2: case HTAG_H3:
        case HTAG_H4: case HTAG_H5: case HTAG_H6: {
            if (has_in_scope(tb, HTAG_P, SK_BUTTON)) close_p_element(tb);
            struct node *c = CURNODE(tb);
            if (c && c->ns == NS_HTML && (tagf(c->htag) & TF_HEADING)) stack_pop(tb);
            insert_html_element(tb, t);
            return;
        }

        case HTAG_PRE: case HTAG_LISTING:
            if (has_in_scope(tb, HTAG_P, SK_BUTTON)) close_p_element(tb);
            insert_html_element(tb, t);
            tb->ignore_lf = 1;
            tb->frameset_ok = 0;
            return;

        case HTAG_FORM: {
            int has_tmpl = stack_has_html_tag(tb, HTAG_TEMPLATE);
            if (tb->form_elem && !has_tmpl) return;
            if (has_in_scope(tb, HTAG_P, SK_BUTTON)) close_p_element(tb);
            struct node *f = insert_html_element(tb, t);
            if (!has_tmpl) tb->form_elem = f;
            return;
        }

        case HTAG_LI:
            tb->frameset_ok = 0;
            for (int i = tb->nopen - 1; i >= 0; i--) {
                struct node *node = tb->open[i];
                if (is_html_tag(node, HTAG_LI)) {
                    generate_implied_end_tags(tb, HTAG_LI);
                    pop_until_html_tag(tb, HTAG_LI);
                    break;
                }
                if (is_special(node) && node->htag != HTAG_ADDRESS &&
                    node->htag != HTAG_DIV && node->htag != HTAG_P) break;
            }
            if (has_in_scope(tb, HTAG_P, SK_BUTTON)) close_p_element(tb);
            insert_html_element(tb, t);
            return;

        case HTAG_DD: case HTAG_DT:
            tb->frameset_ok = 0;
            for (int i = tb->nopen - 1; i >= 0; i--) {
                struct node *node = tb->open[i];
                if (is_html_tag(node, HTAG_DD) || is_html_tag(node, HTAG_DT)) {
                    uint16_t h = node->htag;
                    generate_implied_end_tags(tb, h);
                    pop_until_html_tag(tb, h);
                    break;
                }
                if (is_special(node) && node->htag != HTAG_ADDRESS &&
                    node->htag != HTAG_DIV && node->htag != HTAG_P) break;
            }
            if (has_in_scope(tb, HTAG_P, SK_BUTTON)) close_p_element(tb);
            insert_html_element(tb, t);
            return;

        case HTAG_PLAINTEXT:
            if (has_in_scope(tb, HTAG_P, SK_BUTTON)) close_p_element(tb);
            insert_html_element(tb, t);
            html_tok_set_state(&tb->tok, HTML_STATE_PLAINTEXT);
            return;

        case HTAG_BUTTON:
            if (has_in_scope(tb, HTAG_BUTTON, SK_DEFAULT)) {
                generate_implied_end_tags(tb, HTAG_UNKNOWN);
                pop_until_html_tag(tb, HTAG_BUTTON);
            }
            reconstruct_afe(tb);
            insert_html_element(tb, t);
            tb->frameset_ok = 0;
            return;

        case HTAG_A: {
            for (int i = tb->nafe - 1; i >= 0; i--) {
                if (!tb->afe[i]) break;
                if (is_html_tag(tb->afe[i], HTAG_A)) {
                    struct node *old = tb->afe[i];
                    if (!adoption_agency(tb, t)) in_body_any_other_end_tag(tb, t);
                    afe_remove(tb, old);
                    stack_remove(tb, old);
                    break;
                }
            }
            reconstruct_afe(tb);
            afe_push(tb, insert_html_element(tb, t));
            return;
        }

        case HTAG_B: case HTAG_BIG: case HTAG_CODE: case HTAG_EM: case HTAG_FONT:
        case HTAG_I: case HTAG_S: case HTAG_SMALL: case HTAG_STRIKE:
        case HTAG_STRONG: case HTAG_TT: case HTAG_U:
            reconstruct_afe(tb);
            afe_push(tb, insert_html_element(tb, t));
            return;

        case HTAG_NOBR:
            reconstruct_afe(tb);
            if (has_in_scope(tb, HTAG_NOBR, SK_DEFAULT)) {
                /* The AAA's step 5 ("no such element in the list") falls back
                 * to the "any other end tag" rules -- which matters here even
                 * though the token is a START tag: <nobr><table><marquee>
                 * </table><nobr> puts the second nobr beside the first only
                 * because that fallback pops the first. */
                if (!adoption_agency(tb, t)) in_body_any_other_end_tag(tb, t);
                reconstruct_afe(tb);
            }
            afe_push(tb, insert_html_element(tb, t));
            return;

        case HTAG_APPLET: case HTAG_MARQUEE: case HTAG_OBJECT:
            reconstruct_afe(tb);
            insert_html_element(tb, t);
            afe_insert_marker(tb);
            tb->frameset_ok = 0;
            return;

        case HTAG_TABLE:
            if (dom_doc_quirks(tb->doc) != QM_QUIRKS && has_in_scope(tb, HTAG_P, SK_BUTTON))
                close_p_element(tb);
            insert_html_element(tb, t);
            tb->frameset_ok = 0;
            tb->mode = M_IN_TABLE;
            return;

        case HTAG_AREA: case HTAG_BR: case HTAG_EMBED: case HTAG_IMG:
        case HTAG_KEYGEN: case HTAG_WBR:
            reconstruct_afe(tb);
            insert_html_element(tb, t);
            stack_pop(tb);
            tb->frameset_ok = 0;
            return;

        case HTAG_INPUT: {
            /* The one rule that survived the deletion of the "in select"
             * insertion mode: an <input> is still not allowed in a select, and
             * closes it.  In the fragment case (context <select>, nothing on
             * the stack to close) the token is dropped instead -- the old rule's
             * "(fragment case) ignore the token", still what the corpus wants
             * (tests_innerHTML_1.dat "<input><option>" in a select). */
            if (in_select_content(tb)) {
                if (!stack_has_html_tag(tb, HTAG_SELECT)) return;
                pop_until_html_tag(tb, HTAG_SELECT);
                reset_insertion_mode(tb);
                tb->reprocess = 1;
                return;
            }
            reconstruct_afe(tb);
            struct node *e = insert_html_element(tb, t);
            stack_pop(tb);
            const char *ty = e ? dom_attr(e, "type") : 0;
            if (!ty || !ci_eq_z(ty, "hidden")) tb->frameset_ok = 0;
            return;
        }

        case HTAG_PARAM: case HTAG_SOURCE: case HTAG_TRACK:
            insert_html_element(tb, t);
            stack_pop(tb);
            return;

        case HTAG_HR:
            if (has_in_scope(tb, HTAG_P, SK_BUTTON)) close_p_element(tb);
            /* <hr> is a legal child of <select>, so it closes an open option or
             * optgroup rather than nesting inside one. */
            if (is_html_tag(CURNODE(tb), HTAG_OPTION))   stack_pop(tb);
            if (is_html_tag(CURNODE(tb), HTAG_OPTGROUP)) stack_pop(tb);
            insert_html_element(tb, t);
            stack_pop(tb);
            tb->frameset_ok = 0;
            return;

        case HTAG_IMAGE:
            /* "This is an error, but the spec says to do it anyway." */
            t->name = "img";
            t->namelen = 3;
            t->tag = HTAG_IMG;
            tb->reprocess = 1;
            return;

        case HTAG_TEXTAREA:
            insert_html_element(tb, t);
            tb->ignore_lf = 1;
            html_tok_set_state(&tb->tok, HTML_STATE_RCDATA);
            tb->orig_mode = tb->mode;
            tb->frameset_ok = 0;
            tb->mode = M_TEXT;
            return;

        case HTAG_XMP:
            if (has_in_scope(tb, HTAG_P, SK_BUTTON)) close_p_element(tb);
            reconstruct_afe(tb);
            tb->frameset_ok = 0;
            parse_text_element(tb, t, HTML_STATE_RAWTEXT);
            return;

        case HTAG_IFRAME:
            tb->frameset_ok = 0;
            parse_text_element(tb, t, HTML_STATE_RAWTEXT);
            return;

        case HTAG_NOEMBED:
            parse_text_element(tb, t, HTML_STATE_RAWTEXT);
            return;

        case HTAG_NOSCRIPT:
            if (tb->scripting) { parse_text_element(tb, t, HTML_STATE_RAWTEXT); return; }
            break;                                    /* -> any other start tag */

        /* <select> no longer has insertion modes of its own.  The spec's
         * customizable-select change deleted "in select" and "in select in
         * table" and made a select's contents parse with the ordinary in-body
         * rules -- which is why <select><div>x</div> now keeps the div instead
         * of dropping it, and why <hr>/<option>/<optgroup> only needed the
         * small pops above.  The corpus is the authority here: every
         * <select>-and-anything-else case in webkit02.dat expects the new
         * behaviour. */
        case HTAG_SELECT:
            if (has_in_scope(tb, HTAG_SELECT, SK_DEFAULT)) {
                /* A nested <select> closes the open one and is dropped. */
                pop_until_html_tag(tb, HTAG_SELECT);
                return;
            }
            reconstruct_afe(tb);
            insert_html_element(tb, t);
            tb->frameset_ok = 0;
            return;

        case HTAG_OPTION:
            if (is_html_tag(CURNODE(tb), HTAG_OPTION)) stack_pop(tb);
            reconstruct_afe(tb);
            insert_html_element(tb, t);
            return;

        case HTAG_OPTGROUP:
            if (is_html_tag(CURNODE(tb), HTAG_OPTION))   stack_pop(tb);
            if (is_html_tag(CURNODE(tb), HTAG_OPTGROUP)) stack_pop(tb);
            reconstruct_afe(tb);
            insert_html_element(tb, t);
            return;

        case HTAG_RB: case HTAG_RTC:
            if (has_in_scope(tb, HTAG_RUBY, SK_DEFAULT))
                generate_implied_end_tags(tb, HTAG_UNKNOWN);
            insert_html_element(tb, t);
            return;

        case HTAG_RP: case HTAG_RT:
            if (has_in_scope(tb, HTAG_RUBY, SK_DEFAULT))
                generate_implied_end_tags(tb, HTAG_RTC);
            insert_html_element(tb, t);
            return;

        case HTAG_MATH:
            reconstruct_afe(tb);
            insert_foreign_element(tb, t, NS_MATHML);
            if (t->self_closing) stack_pop(tb);
            return;

        case HTAG_SVG:
            reconstruct_afe(tb);
            if (!tb->svg_root) tb->svg_start = t->src_start;
            {
                struct node *e = insert_foreign_element(tb, t, NS_SVG);
                if (e && !tb->svg_root) tb->svg_root = e;
            }
            if (t->self_closing) stack_pop(tb);
            return;

        case HTAG_CAPTION: case HTAG_COL: case HTAG_COLGROUP: case HTAG_FRAME:
        case HTAG_HEAD: case HTAG_TBODY: case HTAG_TD: case HTAG_TFOOT:
        case HTAG_TH: case HTAG_THEAD: case HTAG_TR:
            return;                                   /* parse error, ignore */

        default: break;
        }
        /* any other start tag */
        reconstruct_afe(tb);
        insert_html_element(tb, t);
        return;

    case TOK_END:
        switch (t->tag) {
        case HTAG_TEMPLATE: in_head(tb, t); return;

        case HTAG_BODY:
            if (!has_in_scope(tb, HTAG_BODY, SK_DEFAULT)) return;
            tb->mode = M_AFTER_BODY;
            return;

        case HTAG_HTML:
            if (!has_in_scope(tb, HTAG_BODY, SK_DEFAULT)) return;
            tb->mode = M_AFTER_BODY;
            tb->reprocess = 1;
            return;

        case HTAG_FORM:
            if (!stack_has_html_tag(tb, HTAG_TEMPLATE)) {
                struct node *node = tb->form_elem;
                tb->form_elem = 0;
                if (!node || !has_node_in_scope(tb, node, SK_DEFAULT)) return;
                generate_implied_end_tags(tb, HTAG_UNKNOWN);
                stack_remove(tb, node);
            } else {
                if (!has_in_scope(tb, HTAG_FORM, SK_DEFAULT)) return;
                generate_implied_end_tags(tb, HTAG_UNKNOWN);
                pop_until_html_tag(tb, HTAG_FORM);
            }
            return;

        case HTAG_P:
            if (!has_in_scope(tb, HTAG_P, SK_BUTTON)) insert_fake(tb, "p", HTAG_P);
            close_p_element(tb);
            return;

        case HTAG_LI:
            if (!has_in_scope(tb, HTAG_LI, SK_LIST)) return;
            generate_implied_end_tags(tb, HTAG_LI);
            pop_until_html_tag(tb, HTAG_LI);
            return;

        case HTAG_DD: case HTAG_DT:
            if (!has_in_scope(tb, t->tag, SK_DEFAULT)) return;
            generate_implied_end_tags(tb, t->tag);
            pop_until_html_tag(tb, t->tag);
            return;

        case HTAG_H1: case HTAG_H2: case HTAG_H3:
        case HTAG_H4: case HTAG_H5: case HTAG_H6:
            if (!has_heading_in_scope(tb)) return;
            generate_implied_end_tags(tb, HTAG_UNKNOWN);
            pop_until_heading(tb);
            return;

        case HTAG_A: case HTAG_B: case HTAG_BIG: case HTAG_CODE: case HTAG_EM:
        case HTAG_FONT: case HTAG_I: case HTAG_NOBR: case HTAG_S:
        case HTAG_SMALL: case HTAG_STRIKE: case HTAG_STRONG: case HTAG_TT:
        case HTAG_U:
            if (adoption_agency(tb, t)) return;
            in_body_any_other_end_tag(tb, t);
            return;

        case HTAG_APPLET: case HTAG_MARQUEE: case HTAG_OBJECT:
            if (!has_in_scope(tb, t->tag, SK_DEFAULT)) return;
            generate_implied_end_tags(tb, HTAG_UNKNOWN);
            pop_until_html_tag(tb, t->tag);
            afe_clear_to_marker(tb);
            return;

        case HTAG_BR:
            /* "</br>": parse error, treated as "<br>" with no attributes. */
            t->type = TOK_START;
            t->nattr = 0;
            t->self_closing = 0;
            tb->reprocess = 1;
            return;

        case HTAG_SELECT:
            if (!has_in_scope(tb, HTAG_SELECT, SK_DEFAULT)) return;
            pop_until_html_tag(tb, HTAG_SELECT);
            return;

        case HTAG_OPTION:
            if (is_html_tag(CURNODE(tb), HTAG_OPTION)) stack_pop(tb);
            return;

        case HTAG_OPTGROUP:
            if (tb->nopen >= 2 && is_html_tag(tb->open[tb->nopen - 1], HTAG_OPTION) &&
                is_html_tag(tb->open[tb->nopen - 2], HTAG_OPTGROUP))
                stack_pop(tb);
            if (is_html_tag(CURNODE(tb), HTAG_OPTGROUP)) stack_pop(tb);
            return;

        default:
            if (is_block_end(t->tag)) {
                if (!has_in_scope(tb, t->tag, SK_DEFAULT)) return;
                generate_implied_end_tags(tb, HTAG_UNKNOWN);
                pop_until_html_tag(tb, t->tag);
                return;
            }
            break;
        }
        in_body_any_other_end_tag(tb, t);
        return;

    default: break;
    }
}

/* ------------------------------------------------------------------------ */
/* "text"                                                                    */
/* ------------------------------------------------------------------------ */

static void mode_text(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS:
        insert_chars(tb, t->data, (int)t->datalen);
        return;
    case TOK_EOF:
        stack_pop(tb);
        tb->mode = tb->orig_mode;
        tb->reprocess = 1;
        return;
    case TOK_END:
        stack_pop(tb);
        tb->mode = tb->orig_mode;
        return;
    default:
        return;
    }
}

/* ------------------------------------------------------------------------ */
/* table modes                                                               */
/* ------------------------------------------------------------------------ */

static void clear_stack_to_table_context(struct html_tb *tb)
{
    while (tb->nopen) {
        struct node *n = CURNODE(tb);
        if (n->ns == NS_HTML &&
            (n->htag == HTAG_TABLE || n->htag == HTAG_TEMPLATE || n->htag == HTAG_HTML)) break;
        stack_pop(tb);
    }
}

static void clear_stack_to_table_body_context(struct html_tb *tb)
{
    while (tb->nopen) {
        struct node *n = CURNODE(tb);
        if (n->ns == NS_HTML &&
            (n->htag == HTAG_TBODY || n->htag == HTAG_TFOOT || n->htag == HTAG_THEAD ||
             n->htag == HTAG_TEMPLATE || n->htag == HTAG_HTML)) break;
        stack_pop(tb);
    }
}

static void clear_stack_to_table_row_context(struct html_tb *tb)
{
    while (tb->nopen) {
        struct node *n = CURNODE(tb);
        if (n->ns == NS_HTML &&
            (n->htag == HTAG_TR || n->htag == HTAG_TEMPLATE || n->htag == HTAG_HTML)) break;
        stack_pop(tb);
    }
}

static void in_table(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS: {
        struct node *c = CURNODE(tb);
        if (c && c->ns == NS_HTML &&
            (c->htag == HTAG_TABLE || c->htag == HTAG_TBODY || c->htag == HTAG_TEMPLATE ||
             c->htag == HTAG_TFOOT || c->htag == HTAG_THEAD || c->htag == HTAG_TR)) {
            tb->tlen = 0;
            tb->orig_mode = tb->mode;
            tb->mode = M_IN_TABLE_TEXT;
            tb->reprocess = 1;
            return;
        }
        break;                                        /* -> anything else */
    }
    case TOK_COMMENT: insert_comment_at(tb, t, 0); return;
    case TOK_DOCTYPE: return;
    case TOK_EOF:     in_body(tb, t); return;

    case TOK_START:
        switch (t->tag) {
        case HTAG_CAPTION:
            clear_stack_to_table_context(tb);
            afe_insert_marker(tb);
            insert_html_element(tb, t);
            tb->mode = M_IN_CAPTION;
            return;
        case HTAG_COLGROUP:
            clear_stack_to_table_context(tb);
            insert_html_element(tb, t);
            tb->mode = M_IN_COLUMN_GROUP;
            return;
        case HTAG_COL:
            clear_stack_to_table_context(tb);
            insert_fake(tb, "colgroup", HTAG_COLGROUP);
            tb->mode = M_IN_COLUMN_GROUP;
            tb->reprocess = 1;
            return;
        case HTAG_TBODY: case HTAG_TFOOT: case HTAG_THEAD:
            clear_stack_to_table_context(tb);
            insert_html_element(tb, t);
            tb->mode = M_IN_TABLE_BODY;
            return;
        case HTAG_TD: case HTAG_TH: case HTAG_TR:
            clear_stack_to_table_context(tb);
            insert_fake(tb, "tbody", HTAG_TBODY);
            tb->mode = M_IN_TABLE_BODY;
            tb->reprocess = 1;
            return;
        case HTAG_TABLE:
            if (!has_in_scope(tb, HTAG_TABLE, SK_TABLE)) return;
            pop_until_html_tag(tb, HTAG_TABLE);
            reset_insertion_mode(tb);
            tb->reprocess = 1;
            return;
        case HTAG_STYLE: case HTAG_SCRIPT: case HTAG_TEMPLATE:
            in_head(tb, t);
            return;
        case HTAG_INPUT: {
            const char *v = 0; uint32_t vl = 0;
            for (int i = 0; i < t->nattr; i++)
                if (ci_eq_n(t->attrs[i].n, (int)t->attrs[i].nl, "type")) {
                    v = t->attrs[i].v; vl = t->attrs[i].vl; break;
                }
            if (!v || !ci_eq_n(v, (int)vl, "hidden")) break;   /* -> anything else */
            insert_html_element(tb, t);
            stack_pop(tb);
            return;
        }
        case HTAG_FORM: {
            if (stack_has_html_tag(tb, HTAG_TEMPLATE) || tb->form_elem) return;
            struct node *f = insert_html_element(tb, t);
            tb->form_elem = f;
            stack_pop(tb);
            return;
        }
        default: break;
        }
        break;

    case TOK_END:
        switch (t->tag) {
        case HTAG_TABLE:
            if (!has_in_scope(tb, HTAG_TABLE, SK_TABLE)) return;
            pop_until_html_tag(tb, HTAG_TABLE);
            reset_insertion_mode(tb);
            return;
        case HTAG_BODY: case HTAG_CAPTION: case HTAG_COL: case HTAG_COLGROUP:
        case HTAG_HTML: case HTAG_TBODY: case HTAG_TD: case HTAG_TFOOT:
        case HTAG_TH: case HTAG_THEAD: case HTAG_TR:
            return;                                   /* parse error, ignore */
        case HTAG_TEMPLATE:
            in_head(tb, t);
            return;
        default: break;
        }
        break;

    default: break;
    }

    /* Anything else: foster parenting.  This is the mechanism that keeps stray
     * content in a table from destroying the rest of the page -- the content is
     * lifted OUT of the table, before it, instead of being dropped. */
    tb->foster = 1;
    in_body(tb, t);
    tb->foster = 0;
}

static void mode_in_table_text(struct html_tb *tb, struct html_token *t)
{
    if (t->type == TOK_CHARS) {
        int n = (int)t->datalen;
        if (tb->tlen + n + 1 > tb->tcap) {
            int c = tb->tcap ? tb->tcap : 256;
            while (c < tb->tlen + n + 1) c *= 2;
            char *p = (char *)realloc(tb->tbuf, (size_t)c);
            if (!p) return;
            tb->tbuf = p; tb->tcap = c;
        }
        memcpy(tb->tbuf + tb->tlen, t->data, (size_t)n);
        tb->tlen += n;
        return;
    }
    flush_table_text(tb);
    tb->mode = tb->orig_mode;
    tb->reprocess = 1;
}

static void flush_table_text(struct html_tb *tb)
{
    if (!tb->tlen) return;
    if (all_ws(tb->tbuf, tb->tlen)) {
        insert_chars(tb, tb->tbuf, tb->tlen);
    } else {
        tb->foster = 1;
        in_body_chars(tb, tb->tbuf, tb->tlen);
        tb->foster = 0;
    }
    tb->tlen = 0;
}

static void mode_in_caption(struct html_tb *tb, struct html_token *t)
{
    int leave = 0;
    if (t->type == TOK_START) {
        switch (t->tag) {
        case HTAG_CAPTION: case HTAG_COL: case HTAG_COLGROUP: case HTAG_TBODY:
        case HTAG_TD: case HTAG_TFOOT: case HTAG_TH: case HTAG_THEAD: case HTAG_TR:
            leave = 1; break;
        default: break;
        }
    } else if (t->type == TOK_END) {
        if (t->tag == HTAG_CAPTION) {
            if (!has_in_scope(tb, HTAG_CAPTION, SK_TABLE)) return;
            generate_implied_end_tags(tb, HTAG_UNKNOWN);
            pop_until_html_tag(tb, HTAG_CAPTION);
            afe_clear_to_marker(tb);
            tb->mode = M_IN_TABLE;
            return;
        }
        if (t->tag == HTAG_TABLE) leave = 1;
        else switch (t->tag) {
        case HTAG_BODY: case HTAG_COL: case HTAG_COLGROUP: case HTAG_HTML:
        case HTAG_TBODY: case HTAG_TD: case HTAG_TFOOT: case HTAG_TH:
        case HTAG_THEAD: case HTAG_TR:
            return;                                   /* parse error, ignore */
        default: break;
        }
    }

    if (leave) {
        if (!has_in_scope(tb, HTAG_CAPTION, SK_TABLE)) return;
        generate_implied_end_tags(tb, HTAG_UNKNOWN);
        pop_until_html_tag(tb, HTAG_CAPTION);
        afe_clear_to_marker(tb);
        tb->mode = M_IN_TABLE;
        tb->reprocess = 1;
        return;
    }
    in_body(tb, t);
}

static void mode_in_column_group(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS: {
        int k = ws_prefix_len(t->data, (int)t->datalen);
        if (k) insert_chars(tb, t->data, k);
        if (k == (int)t->datalen) return;
        t->data += k; t->datalen -= (uint32_t)k;
        break;
    }
    case TOK_COMMENT: insert_comment_at(tb, t, 0); return;
    case TOK_DOCTYPE: return;
    case TOK_EOF:     in_body(tb, t); return;
    case TOK_START:
        if (t->tag == HTAG_HTML)     { in_body(tb, t); return; }
        if (t->tag == HTAG_COL)      { insert_html_element(tb, t); stack_pop(tb); return; }
        if (t->tag == HTAG_TEMPLATE) { in_head(tb, t); return; }
        break;
    case TOK_END:
        if (t->tag == HTAG_COLGROUP) {
            if (!is_html_tag(CURNODE(tb), HTAG_COLGROUP)) return;
            stack_pop(tb);
            tb->mode = M_IN_TABLE;
            return;
        }
        if (t->tag == HTAG_COL) return;
        if (t->tag == HTAG_TEMPLATE) { in_head(tb, t); return; }
        break;
    default: break;
    }
    if (!is_html_tag(CURNODE(tb), HTAG_COLGROUP)) return;
    stack_pop(tb);
    tb->mode = M_IN_TABLE;
    tb->reprocess = 1;
}

static void mode_in_table_body(struct html_tb *tb, struct html_token *t)
{
    if (t->type == TOK_START) {
        switch (t->tag) {
        case HTAG_TR:
            clear_stack_to_table_body_context(tb);
            insert_html_element(tb, t);
            tb->mode = M_IN_ROW;
            return;
        case HTAG_TD: case HTAG_TH:
            clear_stack_to_table_body_context(tb);
            insert_fake(tb, "tr", HTAG_TR);
            tb->mode = M_IN_ROW;
            tb->reprocess = 1;
            return;
        case HTAG_CAPTION: case HTAG_COL: case HTAG_COLGROUP:
        case HTAG_TBODY: case HTAG_TFOOT: case HTAG_THEAD:
            if (!has_in_scope(tb, HTAG_TBODY, SK_TABLE) &&
                !has_in_scope(tb, HTAG_THEAD, SK_TABLE) &&
                !has_in_scope(tb, HTAG_TFOOT, SK_TABLE)) return;
            clear_stack_to_table_body_context(tb);
            stack_pop(tb);
            tb->mode = M_IN_TABLE;
            tb->reprocess = 1;
            return;
        default: break;
        }
    } else if (t->type == TOK_END) {
        switch (t->tag) {
        case HTAG_TBODY: case HTAG_TFOOT: case HTAG_THEAD:
            if (!has_in_scope(tb, t->tag, SK_TABLE)) return;
            clear_stack_to_table_body_context(tb);
            stack_pop(tb);
            tb->mode = M_IN_TABLE;
            return;
        case HTAG_TABLE:
            if (!has_in_scope(tb, HTAG_TBODY, SK_TABLE) &&
                !has_in_scope(tb, HTAG_THEAD, SK_TABLE) &&
                !has_in_scope(tb, HTAG_TFOOT, SK_TABLE)) return;
            clear_stack_to_table_body_context(tb);
            stack_pop(tb);
            tb->mode = M_IN_TABLE;
            tb->reprocess = 1;
            return;
        case HTAG_BODY: case HTAG_CAPTION: case HTAG_COL: case HTAG_COLGROUP:
        case HTAG_HTML: case HTAG_TD: case HTAG_TH: case HTAG_TR:
            return;
        default: break;
        }
    }
    in_table(tb, t);
}

static void mode_in_row(struct html_tb *tb, struct html_token *t)
{
    if (t->type == TOK_START) {
        switch (t->tag) {
        case HTAG_TD: case HTAG_TH:
            clear_stack_to_table_row_context(tb);
            insert_html_element(tb, t);
            tb->mode = M_IN_CELL;
            afe_insert_marker(tb);
            return;
        case HTAG_CAPTION: case HTAG_COL: case HTAG_COLGROUP: case HTAG_TBODY:
        case HTAG_TFOOT: case HTAG_THEAD: case HTAG_TR:
            if (!has_in_scope(tb, HTAG_TR, SK_TABLE)) return;
            clear_stack_to_table_row_context(tb);
            stack_pop(tb);
            tb->mode = M_IN_TABLE_BODY;
            tb->reprocess = 1;
            return;
        default: break;
        }
    } else if (t->type == TOK_END) {
        switch (t->tag) {
        case HTAG_TR:
            if (!has_in_scope(tb, HTAG_TR, SK_TABLE)) return;
            clear_stack_to_table_row_context(tb);
            stack_pop(tb);
            tb->mode = M_IN_TABLE_BODY;
            return;
        case HTAG_TABLE:
            if (!has_in_scope(tb, HTAG_TR, SK_TABLE)) return;
            clear_stack_to_table_row_context(tb);
            stack_pop(tb);
            tb->mode = M_IN_TABLE_BODY;
            tb->reprocess = 1;
            return;
        case HTAG_TBODY: case HTAG_TFOOT: case HTAG_THEAD:
            if (!has_in_scope(tb, t->tag, SK_TABLE)) return;
            if (!has_in_scope(tb, HTAG_TR, SK_TABLE)) return;
            clear_stack_to_table_row_context(tb);
            stack_pop(tb);
            tb->mode = M_IN_TABLE_BODY;
            tb->reprocess = 1;
            return;
        case HTAG_BODY: case HTAG_CAPTION: case HTAG_COL: case HTAG_COLGROUP:
        case HTAG_HTML: case HTAG_TD: case HTAG_TH:
            return;
        default: break;
        }
    }
    in_table(tb, t);
}

static void close_the_cell(struct html_tb *tb)
{
    generate_implied_end_tags(tb, HTAG_UNKNOWN);
    while (tb->nopen) {
        struct node *n = CURNODE(tb);
        stack_pop(tb);
        if (n->ns == NS_HTML && (n->htag == HTAG_TD || n->htag == HTAG_TH)) break;
    }
    afe_clear_to_marker(tb);
    tb->mode = M_IN_ROW;
}

static void mode_in_cell(struct html_tb *tb, struct html_token *t)
{
    if (t->type == TOK_END) {
        switch (t->tag) {
        case HTAG_TD: case HTAG_TH:
            if (!has_in_scope(tb, t->tag, SK_TABLE)) return;
            generate_implied_end_tags(tb, HTAG_UNKNOWN);
            pop_until_html_tag(tb, t->tag);
            afe_clear_to_marker(tb);
            tb->mode = M_IN_ROW;
            return;
        case HTAG_BODY: case HTAG_CAPTION: case HTAG_COL: case HTAG_COLGROUP:
        case HTAG_HTML:
            return;
        case HTAG_TABLE: case HTAG_TBODY: case HTAG_TFOOT: case HTAG_THEAD:
        case HTAG_TR:
            if (!has_in_scope(tb, t->tag, SK_TABLE)) return;
            close_the_cell(tb);
            tb->reprocess = 1;
            return;
        default: break;
        }
    } else if (t->type == TOK_START) {
        switch (t->tag) {
        case HTAG_CAPTION: case HTAG_COL: case HTAG_COLGROUP: case HTAG_TBODY:
        case HTAG_TD: case HTAG_TFOOT: case HTAG_TH: case HTAG_THEAD: case HTAG_TR:
            if (!has_in_scope(tb, HTAG_TD, SK_TABLE) && !has_in_scope(tb, HTAG_TH, SK_TABLE))
                return;
            close_the_cell(tb);
            tb->reprocess = 1;
            return;
        default: break;
        }
    }
    in_body(tb, t);
}

/* ------------------------------------------------------------------------ */
/* "in template"                                                             */
/* ------------------------------------------------------------------------ */

static void tmpl_switch(struct html_tb *tb, int mode)
{
    if (tb->ntmpl) tb->tmpl[tb->ntmpl - 1] = mode;
    tb->mode = mode;
    tb->reprocess = 1;
}

static void mode_in_template(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS: case TOK_COMMENT: case TOK_DOCTYPE:
        in_body(tb, t);
        return;
    case TOK_EOF:
        if (!stack_has_html_tag(tb, HTAG_TEMPLATE)) { tb->done = 1; return; }
        pop_until_html_tag(tb, HTAG_TEMPLATE);
        afe_clear_to_marker(tb);
        if (tb->ntmpl) tb->ntmpl--;
        reset_insertion_mode(tb);
        tb->reprocess = 1;
        return;
    case TOK_START:
        switch (t->tag) {
        case HTAG_BASE: case HTAG_BASEFONT: case HTAG_BGSOUND: case HTAG_LINK:
        case HTAG_META: case HTAG_NOFRAMES: case HTAG_SCRIPT: case HTAG_STYLE:
        case HTAG_TEMPLATE: case HTAG_TITLE:
            in_head(tb, t);
            return;
        case HTAG_CAPTION: case HTAG_COLGROUP: case HTAG_TBODY: case HTAG_TFOOT:
        case HTAG_THEAD:
            tmpl_switch(tb, M_IN_TABLE); return;
        case HTAG_COL: tmpl_switch(tb, M_IN_COLUMN_GROUP); return;
        case HTAG_TR:  tmpl_switch(tb, M_IN_TABLE_BODY); return;
        case HTAG_TD: case HTAG_TH: tmpl_switch(tb, M_IN_ROW); return;
        default: tmpl_switch(tb, M_IN_BODY); return;
        }
    case TOK_END:
        if (t->tag == HTAG_TEMPLATE) { in_head(tb, t); return; }
        return;                                       /* parse error, ignore */
    default: return;
    }
}

/* ------------------------------------------------------------------------ */
/* "after body" / frameset modes                                             */
/* ------------------------------------------------------------------------ */

static void mode_after_body(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS: {
        int k = ws_prefix_len(t->data, (int)t->datalen);
        if (k) in_body_chars(tb, t->data, k);
        if (k == (int)t->datalen) return;
        t->data += k; t->datalen -= (uint32_t)k;
        break;
    }
    case TOK_COMMENT:
        insert_comment_at(tb, t, tb->nopen ? tb->open[0] : tb->docroot);
        return;
    case TOK_DOCTYPE: return;
    case TOK_EOF: tb->done = 1; return;
    case TOK_START:
        if (t->tag == HTAG_HTML) { in_body(tb, t); return; }
        break;
    case TOK_END:
        if (t->tag == HTAG_HTML) {
            if (tb->fragment) return;
            tb->mode = M_AFTER_AFTER_BODY;
            return;
        }
        break;
    default: break;
    }
    tb->mode = M_IN_BODY;
    tb->reprocess = 1;
}

static void mode_after_after_body(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_COMMENT: insert_comment_at(tb, t, tb->docroot); return;
    case TOK_DOCTYPE: in_body(tb, t); return;
    case TOK_EOF: tb->done = 1; return;
    case TOK_CHARS: {
        int k = ws_prefix_len(t->data, (int)t->datalen);
        if (k) in_body_chars(tb, t->data, k);
        if (k == (int)t->datalen) return;
        t->data += k; t->datalen -= (uint32_t)k;
        break;
    }
    case TOK_START:
        if (t->tag == HTAG_HTML) { in_body(tb, t); return; }
        break;
    default: break;
    }
    tb->mode = M_IN_BODY;
    tb->reprocess = 1;
}

static void mode_in_frameset(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS:
        insert_ws_only(tb, t->data, (int)t->datalen);
        return;
    case TOK_COMMENT: insert_comment_at(tb, t, 0); return;
    case TOK_DOCTYPE: return;
    case TOK_EOF: tb->done = 1; return;
    case TOK_START:
        if (t->tag == HTAG_HTML)     { in_body(tb, t); return; }
        if (t->tag == HTAG_FRAMESET) { insert_html_element(tb, t); return; }
        if (t->tag == HTAG_FRAME)    { insert_html_element(tb, t); stack_pop(tb); return; }
        if (t->tag == HTAG_NOFRAMES) { in_head(tb, t); return; }
        return;
    case TOK_END:
        if (t->tag == HTAG_FRAMESET) {
            if (is_html_tag(CURNODE(tb), HTAG_HTML)) return;   /* fragment case */
            stack_pop(tb);
            if (!tb->fragment && !is_html_tag(CURNODE(tb), HTAG_FRAMESET))
                tb->mode = M_AFTER_FRAMESET;
        }
        return;
    default: return;
    }
}

static void mode_after_frameset(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS:
        insert_ws_only(tb, t->data, (int)t->datalen);
        return;
    case TOK_COMMENT: insert_comment_at(tb, t, 0); return;
    case TOK_DOCTYPE: return;
    case TOK_EOF: tb->done = 1; return;
    case TOK_START:
        if (t->tag == HTAG_HTML)     { in_body(tb, t); return; }
        if (t->tag == HTAG_NOFRAMES) { in_head(tb, t); return; }
        return;
    case TOK_END:
        if (t->tag == HTAG_HTML) tb->mode = M_AFTER_AFTER_FRAMESET;
        return;
    default: return;
    }
}

static void mode_after_after_frameset(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_COMMENT: insert_comment_at(tb, t, tb->docroot); return;
    case TOK_DOCTYPE: in_body(tb, t); return;
    case TOK_EOF: tb->done = 1; return;
    case TOK_CHARS:
        insert_ws_only(tb, t->data, (int)t->datalen);
        return;
    case TOK_START:
        if (t->tag == HTAG_HTML)     { in_body(tb, t); return; }
        if (t->tag == HTAG_NOFRAMES) { in_head(tb, t); return; }
        return;
    default: return;
    }
}

/* ------------------------------------------------------------------------ */
/* foreign content                                                           */
/* ------------------------------------------------------------------------ */

/* The breakout: an HTML start tag (or "</br>" / "</p>") that has no business
 * inside SVG/MathML closes the foreign subtree and is reprocessed as HTML.
 * This is what stops one stray <svg> from swallowing the rest of the page.
 *
 * The token goes STRAIGHT to the insertion mode -- the spec says "reprocess the
 * token according to the rules given in the section corresponding to the
 * current insertion mode in HTML content", not "re-run the dispatcher".  The
 * difference is only visible in the fragment case (context <svg path>, stack
 * holding just the root): there the adjusted current node is still the foreign
 * context element, so re-running the dispatcher would route the token back to
 * foreign content and loop until the reprocess guard ate it. */
static void foreign_break_out(struct html_tb *tb, struct html_token *t)
{
    while (tb->nopen) {
        struct node *c = CURNODE(tb);
        if (is_mathml_text_ip(c) || is_html_integration_point(c) || c->ns == NS_HTML) break;
        stack_pop(tb);
    }
    tb_mode(tb, t);
}

static void foreign_content(struct html_tb *tb, struct html_token *t)
{
    switch (t->type) {
    case TOK_CHARS: {
        /* U+0000 becomes U+FFFD here rather than being dropped -- foreign
         * content has no "ignore the character" rule. */
        int i = 0, n = (int)t->datalen;
        while (i < n) {
            int s = i;
            while (i < n && t->data[i]) i++;
            if (i > s) insert_chars(tb, t->data + s, i - s);
            while (i < n && !t->data[i]) { insert_chars(tb, "\xEF\xBF\xBD", 3); i++; }
        }
        for (int k = 0; k < n; k++)
            if (!is_ws((unsigned char)t->data[k])) { tb->frameset_ok = 0; break; }
        return;
    }
    case TOK_COMMENT: insert_comment_at(tb, t, 0); return;
    case TOK_DOCTYPE: return;

    case TOK_START: {
        int breakout = (tagf(t->tag) & TF_BREAKOUT) != 0;
        if (!breakout && t->tag == HTAG_FONT)
            breakout = tok_has_attr(t, "color") || tok_has_attr(t, "face") ||
                       tok_has_attr(t, "size");
        if (breakout) { foreign_break_out(tb, t); return; }
        struct node *acn = adjusted_current(tb);
        int ns = acn ? acn->ns : NS_HTML;
        if (ns == NS_SVG) {
            if (!tb->svg_root) tb->svg_start = t->src_start;
            struct node *e = insert_foreign_element(tb, t, NS_SVG);
            if (e && !tb->svg_root) tb->svg_root = e;
        } else {
            insert_foreign_element(tb, t, ns == NS_MATHML ? NS_MATHML : NS_HTML);
        }
        if (t->self_closing) {
            /* An SVG <script/> would run here in a real browser; DEVIATION 3
             * means it is only popped. */
            stack_pop(tb);
        }
        return;
    }

    case TOK_END: {
        struct node *cur = CURNODE(tb);
        if (t->tag == HTAG_SCRIPT && cur && cur->ns == NS_SVG && cur->htag == HTAG_SCRIPT) {
            stack_pop(tb);
            return;
        }
        /* "</br>" and "</p>" break out exactly like the HTML start tags above. */
        if (t->tag == HTAG_BR || t->tag == HTAG_P) { foreign_break_out(tb, t); return; }

        /* The spec's order matters and is easy to get backwards: the name is
         * only compared against the node we are AT, and the HTML-namespace test
         * happens after stepping up.  Comparing the name on an HTML node too
         * would let "</div>" close a <div> that is an ANCESTOR of the foreign
         * subtree instead of being handed to the insertion mode. */
        int i = tb->nopen - 1;
        for (;;) {
            if (i <= 0) return;                       /* topmost: fragment case */
            if (node_matches_token(tb->open[i], t)) {
                while (tb->nopen > i) stack_pop(tb);
                return;
            }
            i--;
            if (tb->open[i]->ns == NS_HTML) { tb_mode(tb, t); return; }
        }
    }

    default: return;
    }
}

/* ------------------------------------------------------------------------ */
/* mode dispatch                                                             */
/* ------------------------------------------------------------------------ */

static void tb_mode(struct html_tb *tb, struct html_token *t)
{
    switch (tb->mode) {
    case M_INITIAL:             mode_initial(tb, t); break;
    case M_BEFORE_HTML:         mode_before_html(tb, t); break;
    case M_BEFORE_HEAD:         mode_before_head(tb, t); break;
    case M_IN_HEAD:             in_head(tb, t); break;
    case M_IN_HEAD_NOSCRIPT:    mode_in_head_noscript(tb, t); break;
    case M_AFTER_HEAD:          mode_after_head(tb, t); break;
    case M_IN_BODY:             in_body(tb, t); break;
    case M_TEXT:                mode_text(tb, t); break;
    case M_IN_TABLE:            in_table(tb, t); break;
    case M_IN_TABLE_TEXT:       mode_in_table_text(tb, t); break;
    case M_IN_CAPTION:          mode_in_caption(tb, t); break;
    case M_IN_COLUMN_GROUP:     mode_in_column_group(tb, t); break;
    case M_IN_TABLE_BODY:       mode_in_table_body(tb, t); break;
    case M_IN_ROW:              mode_in_row(tb, t); break;
    case M_IN_CELL:             mode_in_cell(tb, t); break;
    case M_IN_TEMPLATE:         mode_in_template(tb, t); break;
    case M_AFTER_BODY:          mode_after_body(tb, t); break;
    case M_IN_FRAMESET:         mode_in_frameset(tb, t); break;
    case M_AFTER_FRAMESET:      mode_after_frameset(tb, t); break;
    case M_AFTER_AFTER_BODY:    mode_after_after_body(tb, t); break;
    case M_AFTER_AFTER_FRAMESET:mode_after_after_frameset(tb, t); break;
    default:                    in_body(tb, t); break;
    }
}

/* ------------------------------------------------------------------------ */
/* driver                                                                    */
/* ------------------------------------------------------------------------ */

static void tb_run(struct html_tb *tb)
{
    struct html_token t;
    for (;;) {
        struct node *acn = adjusted_current(tb);
        html_tok_set_foreign(&tb->tok, acn && acn->ns != NS_HTML);

        int r = html_tok_next(&tb->tok, &t);
        if (r <= 0) break;

        if (tb->ignore_lf) {
            tb->ignore_lf = 0;
            if (t.type == TOK_CHARS && t.datalen && t.data[0] == '\n') {
                t.data++;
                t.datalen--;
                if (!t.datalen) continue;
            }
        }
        tb->cur_src_end = t.src_end;
        tb_process(tb, &t);
        if (t.type == TOK_EOF || tb->done) break;
    }
    /* An unclosed <svg> still owes layout its source span. */
    if (tb->svg_root && tb->src) {
        uint32_t end = (uint32_t)tb->srclen;
        if (end > tb->svg_start)
            dom_set_raw(tb->svg_root, tb->src + tb->svg_start, (int)(end - tb->svg_start));
        tb->svg_root = 0;
    }
}

static void tb_free(struct html_tb *tb)
{
    html_tok_free(&tb->tok);
    free(tb->open);
    free(tb->afe);
    free(tb->tmpl);
    free(tb->tbuf);
}

static int tb_init(struct html_tb *tb, const char *src, int len)
{
    memset(tb, 0, sizeof *tb);
    tb->doc = dom_doc_new();
    if (!tb->doc) return 0;
    tb->docroot = dom_doc_root(tb->doc);
    tb->src = src;
    tb->srclen = len;
    tb->scripting = 1;              /* we run page scripts, so <noscript> is raw text */
    tb->frameset_ok = 1;
    tb->mode = M_INITIAL;
    html_tok_init(&tb->tok, src ? src : "", (size_t)(len > 0 ? len : 0), 1);
    return 1;
}

struct node *html_parse(struct dom_doc **out_doc, const char *src, int len)
{
    struct html_tb tb;
    if (!tb_init(&tb, src, len)) { if (out_doc) *out_doc = 0; return 0; }
    tb_run(&tb);
    struct node *root = tb.docroot;
    if (out_doc) *out_doc = tb.doc;
    tb_free(&tb);
    return root;
}

/* ------------------------------------------------------------------------ */
/* fragment parsing                                                          */
/* ------------------------------------------------------------------------ */

struct node *html_parse_fragment(struct dom_doc **out_doc, const char *src, int len,
                                 const char *context, int ctxlen, int ctx_ns)
{
    struct html_tb tb;
    if (out_doc) *out_doc = 0;
    if (!tb_init(&tb, src, len)) return 0;
    if (out_doc) *out_doc = tb.doc;

    if (!context || ctxlen <= 0) { context = "div"; ctxlen = 3; ctx_ns = NS_HTML; }

    /* The context element is created but NEVER put in the tree: it exists so
     * "adjusted current node" has something to report while the stack holds
     * only the synthetic root. */
    struct node *ctx = dom_create_element_ns(tb.doc, context, ctxlen, ctx_ns);
    if (ctx) {
        /* The NAME keeps its case ("foreignObject"), but the tag id comes from
         * the lowercased form -- html_tag_id's table is lowercase, and a
         * foreignObject context whose htag came out 0 stops being recognised as
         * an HTML integration point. */
        char low[64];
        int n = ctxlen < (int)sizeof low ? ctxlen : (int)sizeof low - 1;
        for (int i = 0; i < n; i++) low[i] = (char)lc((unsigned char)context[i]);
        low[n] = 0;
        ctx->htag = html_tag_id(low, (uint32_t)n);
    }
    tb.context = ctx;
    tb.fragment = 1;

    if (ctx_ns == NS_HTML && ctx) {
        switch (ctx->htag) {
        case HTAG_TITLE: case HTAG_TEXTAREA:
            html_tok_set_state(&tb.tok, HTML_STATE_RCDATA); break;
        case HTAG_STYLE: case HTAG_XMP: case HTAG_IFRAME:
        case HTAG_NOEMBED: case HTAG_NOFRAMES:
            html_tok_set_state(&tb.tok, HTML_STATE_RAWTEXT); break;
        case HTAG_SCRIPT:
            html_tok_set_state(&tb.tok, HTML_STATE_SCRIPT_DATA); break;
        case HTAG_NOSCRIPT:
            if (tb.scripting) html_tok_set_state(&tb.tok, HTML_STATE_RAWTEXT);
            break;
        case HTAG_PLAINTEXT:
            html_tok_set_state(&tb.tok, HTML_STATE_PLAINTEXT); break;
        default: break;
        }
        /* Deliberately NOT html_tok_set_last_start_tag(): the fragment parser
         * has emitted no start tag, so no end tag is "appropriate" yet.  That
         * is why innerHTML on a <script> keeps a literal "</script>" in the
         * text instead of ending the element (tests4.dat case 8). */
    }

    struct node *root = dom_create_element_ns(tb.doc, "html", 4, NS_HTML);
    if (!root) { tb_free(&tb); return 0; }
    root->htag = HTAG_HTML;
    dom_append_child(tb.docroot, root);
    stack_push(&tb, root);

    if (ctx && ctx_ns == NS_HTML && ctx->htag == HTAG_TEMPLATE) {
        if (tb.ntmpl == tb.tmplcap) {
            int c = tb.tmplcap ? tb.tmplcap * 2 : 8;
            int *p = (int *)realloc(tb.tmpl, (size_t)c * sizeof *p);
            if (p) { tb.tmpl = p; tb.tmplcap = c; }
        }
        if (tb.ntmpl < tb.tmplcap) tb.tmpl[tb.ntmpl++] = M_IN_TEMPLATE;
    }

    reset_insertion_mode(&tb);
    /* The form element pointer would be the nearest <form> ancestor of the
     * context element; a detached context has none, so it stays NULL. */

    tb_run(&tb);
    tb_free(&tb);
    return root;
}
