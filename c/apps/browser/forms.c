/* forms.c -- what a form control HOLDS, and what a keystroke does to it.
 *
 * The state that makes a text field a text field. Read forms.h first for the
 * value-vs-content-attribute rule; it is the source of most of the subtlety
 * here and all of the subtlety in the bugs.
 *
 * WHY THE STATE IS A SIDE TABLE AND NOT A FIELD ON `struct node`. dom.h belongs
 * to another line and `struct node` is already 200-odd bytes times ten thousand
 * nodes on a real page -- adding six fields that only 0.1% of elements ever use
 * is the wrong trade even if the header were mine to edit. So: a hash table
 * keyed by node pointer, entries created on demand, every entry carrying the
 * node's `serial` so a recycled slot can never be read as the control that used
 * to live there. Same discipline as focus.c, for the same reason.
 *
 * WHAT THE TABLE COSTS. One malloc per control the user or the page actually
 * touches, freed wholesale by fc_reset() on navigation. A page with a hundred
 * inputs that nobody types into allocates a hundred small entries the first
 * time it paints -- fc_paint_state creates them -- and that is the honest cost
 * of not putting the fields on the node.
 */

#include "forms.h"
#include "focus.h"
#include "css.h"

#include <stdlib.h>
#include <string.h>

/* Resolved by browser_rt.c in the app and by a stub in the host tests. Declared
 * rather than included for the same reason layout.c declares it: forms.c must
 * not acquire a dependency on logit.h, which issues `int 0x80`. */
int text_measure(const char *s, int len, int px, int mono);

/* ============================================================ the table === */

#define FC_NBUCKET 257

struct fctl {
    struct node *node;
    uint32_t     serial;
    struct fctl *next;

    char *val; int vlen, vcap;      /* the CURRENT value */
    unsigned char dirty_value;      /* the spec's dirty value flag */
    unsigned char dirty_checked;    /* the spec's dirty checkedness flag */
    unsigned char checked;
    unsigned char dirty_sel;        /* <select>: selectedness has been set */
    unsigned char open;             /* <select>: the dropdown is showing */
    int  sel_index;                 /* <select> */

    int  sel0, sel1;                /* text selection / caret, BYTES */
    int  anchor;                    /* selection anchor for Shift+Arrow */
    int  scroll_x;                  /* px the field is scrolled */

    char *fval; int fvlen;          /* value when focus arrived -> `change` */
};

static struct fctl *g_buck[FC_NBUCKET];
static int g_count;

static unsigned hash_ptr(const void *p)
{
    unsigned long v = (unsigned long)p;
    v >>= 4;
    return (unsigned)(v % FC_NBUCKET);
}

static void ent_free(struct fctl *c)
{
    free(c->val);
    free(c->fval);
    free(c);
}

void fc_ce_clear(void);

void fc_reset(void)
{
    fc_ce_clear();
    for (int i = 0; i < FC_NBUCKET; i++) {
        struct fctl *c = g_buck[i];
        while (c) { struct fctl *nx = c->next; ent_free(c); c = nx; }
        g_buck[i] = 0;
    }
    g_count = 0;
}

/* Find (create) the entry for `n`. A hit whose serial disagrees is a RECYCLED
 * slot: the element we knew is gone and this is a different one, so the old
 * entry is dropped rather than handed back. That single check is what keeps a
 * single-page application from inheriting the previous view's typing. */
static struct fctl *ent(struct node *n, int create)
{
    if (!n || n->type != N_ELEM) return 0;
    unsigned h = hash_ptr(n);
    struct fctl **pp = &g_buck[h];
    while (*pp) {
        struct fctl *c = *pp;
        if (c->node == n) {
            if (c->serial == n->serial) return c;
            *pp = c->next; ent_free(c); g_count--;
            break;
        }
        pp = &c->next;
    }
    if (!create) return 0;
    struct fctl *c = (struct fctl *)calloc(1, sizeof *c);
    if (!c) return 0;
    c->node = n; c->serial = n->serial;
    c->sel_index = -1;
    c->next = g_buck[h];
    g_buck[h] = c;
    g_count++;
    return c;
}

/* ============================================================== helpers === */

static int tag_is(const char *t, const char *lit)
{ int i = 0; for (; lit[i]; i++) if (t[i] != lit[i]) return 0; return t[i] == 0; }

static int slen(const char *s) { int i = 0; if (!s) return 0; while (s[i]) i++; return i; }

/* Store `len` bytes into c->val, growing the buffer. NUL-terminated so callers
 * can treat it as a C string; `vlen` is the truth. */
static int set_val(struct fctl *c, const char *s, int len)
{
    if (len < 0) len = slen(s);
    if (len + 1 > c->vcap) {
        int cap = c->vcap ? c->vcap : 32;
        while (cap < len + 1) cap *= 2;
        char *nb = (char *)realloc(c->val, (size_t)cap);
        if (!nb) return 0;
        c->val = nb; c->vcap = cap;
    }
    for (int i = 0; i < len; i++) c->val[i] = s[i];
    c->val[len] = 0;
    c->vlen = len;
    if (c->sel0 > len) c->sel0 = len;
    if (c->sel1 > len) c->sel1 = len;
    if (c->anchor > len) c->anchor = len;
    return 1;
}

/* Gather an element's descendant text into `buf`. Used for <textarea>'s
 * default value and an <option>'s label -- both of which are "the element's
 * child text content", and both of which arrive as several text nodes when the
 * markup contains an entity. */
static int text_content(const struct node *n, char *buf, int max)
{
    int o = 0;
    if (!n) { if (max > 0) buf[0] = 0; return 0; }
    const struct node *stack[64];
    int sp = 0;
    stack[sp++] = n->first_child;
    /* Iterative pre-order over the subtree; `stack` holds the sibling to
     * resume at, so a deep DOM cannot blow the ring-3 stack here. */
    while (sp > 0) {
        const struct node *c = stack[--sp];
        while (c) {
            if (c->type == N_TEXT && c->text) {
                for (int i = 0; i < c->textlen && o < max - 1; i++) buf[o++] = c->text[i];
            } else if (c->type == N_ELEM && c->first_child && sp < 63) {
                stack[sp++] = c->next;
                c = c->first_child;
                continue;
            }
            c = c->next;
        }
    }
    if (max > 0) buf[o] = 0;
    return o;
}

int fc_disabled(struct node *n)
{
    if (!n || n->type != N_ELEM) return 0;
    if (dom_attr(n, "disabled")) return 1;
    for (struct node *p = n->parent; p && p->type == N_ELEM; p = p->parent)
        if (tag_is(p->tag, "fieldset") && dom_attr(p, "disabled")) return 1;
    return 0;
}

int fc_readonly(struct node *n)
{
    if (!n || n->type != N_ELEM) return 0;
    return dom_attr(n, "readonly") != 0;
}

struct node *fc_form_of(struct node *n)
{
    if (!n || n->type != N_ELEM) return 0;
    const char *fid = dom_attr(n, "form");
    if (fid && fid[0]) {
        /* The `form` attribute wins over the ancestor, and it may point OUT of
         * the form entirely -- that is what it is for. */
        struct node *root = n;
        while (root->parent) root = root->parent;
        struct node *f = root->doc ? dom_get_element_by_id(root->doc, fid) : 0;
        if (f && f->type == N_ELEM && tag_is(f->tag, "form")) return f;
        return 0;
    }
    for (struct node *p = n->parent; p && p->type == N_ELEM; p = p->parent)
        if (tag_is(p->tag, "form")) return p;
    return 0;
}

struct node *fc_label_target(struct node *label)
{
    if (!label || label->type != N_ELEM || !tag_is(label->tag, "label")) return 0;
    const char *f = dom_attr(label, "for");
    if (f && f[0]) {
        struct node *root = label;
        while (root->parent) root = root->parent;
        struct node *t = root->doc ? dom_get_element_by_id(root->doc, f) : 0;
        return (t && fc_kind(t) != FC_NONE) ? t : 0;
    }
    /* No `for`: the first labelable descendant. */
    const struct node *stack[64];
    int sp = 0;
    stack[sp++] = label->first_child;
    while (sp > 0) {
        const struct node *c = stack[--sp];
        while (c) {
            if (c->type == N_ELEM) {
                if (fc_kind(c) != FC_NONE) return (struct node *)c;
                if (c->first_child && sp < 63) { stack[sp++] = c->next; c = c->first_child; continue; }
            }
            c = c->next;
        }
    }
    return 0;
}

/* ============================================================== options === */

/* <option>s in tree order, flattening <optgroup> -- which is exactly what
 * select.options is defined to be. */
static struct node *option_walk(struct node *sel, int want, int *count)
{
    int seen = 0;
    for (struct node *g = sel ? sel->first_child : 0; g; g = g->next) {
        if (g->type != N_ELEM) continue;
        if (tag_is(g->tag, "option")) {
            if (want == seen) { if (count) *count = seen + 1; return g; }
            seen++;
        } else if (tag_is(g->tag, "optgroup")) {
            for (struct node *o = g->first_child; o; o = o->next) {
                if (o->type != N_ELEM || !tag_is(o->tag, "option")) continue;
                if (want == seen) { if (count) *count = seen + 1; return o; }
                seen++;
            }
        }
    }
    if (count) *count = seen;
    return 0;
}

struct node *fc_option_at(struct node *sel, int i)
{ return i < 0 ? 0 : option_walk(sel, i, 0); }

int fc_option_count(struct node *sel)
{ int n = 0; option_walk(sel, -1, &n); return n; }

int fc_option_label(struct node *opt, char *buf, int max)
{
    if (!opt || max <= 0) { if (max > 0) buf[0] = 0; return 0; }
    const char *l = dom_attr(opt, "label");
    if (l && l[0]) {
        int i = 0; while (l[i] && i < max - 1) { buf[i] = l[i]; i++; }
        buf[i] = 0; return i;
    }
    int n = text_content(opt, buf, max);
    /* Collapse the leading/trailing whitespace the markup's indentation adds;
     * a <option> label is stripped in every real UA and an un-stripped one
     * makes every dropdown row start with a newline. */
    int a = 0, b = n;
    while (a < b && (buf[a] == ' ' || buf[a] == '\n' || buf[a] == '\t' || buf[a] == '\r')) a++;
    while (b > a && (buf[b-1] == ' ' || buf[b-1] == '\n' || buf[b-1] == '\t' || buf[b-1] == '\r')) b--;
    if (a > 0) for (int i = a; i < b; i++) buf[i - a] = buf[i];
    n = b - a;
    buf[n] = 0;
    return n;
}

int fc_option_value(struct node *opt, char *buf, int max)
{
    if (!opt || max <= 0) { if (max > 0) buf[0] = 0; return 0; }
    const char *v = dom_attr(opt, "value");
    if (v) {                       /* value="" IS a value, so test for NULL */
        int i = 0; while (v[i] && i < max - 1) { buf[i] = v[i]; i++; }
        buf[i] = 0; return i;
    }
    return fc_option_label(opt, buf, max);
}

int fc_selected_index(struct node *n)
{
    if (!n || fc_kind(n) != FC_SELECT) return -1;
    struct fctl *c = ent(n, 1);
    int cnt = fc_option_count(n);
    if (c && c->dirty_sel) {
        if (c->sel_index >= cnt) return cnt > 0 ? cnt - 1 : -1;
        return c->sel_index;
    }
    for (int i = 0; i < cnt; i++) {
        struct node *o = fc_option_at(n, i);
        if (o && dom_attr(o, "selected")) return i;
    }
    /* A single-line <select> with no explicit selection shows its first option.
     * A `multiple` or sized list shows none, which is why this is not just
     * "return 0". */
    if (dom_attr(n, "multiple") || dom_attr(n, "size")) return -1;
    return cnt > 0 ? 0 : -1;
}

void fc_set_selected_index(struct node *n, int i)
{
    struct fctl *c = ent(n, 1);
    if (!c) return;
    int cnt = fc_option_count(n);
    if (i < -1) i = -1;
    if (i >= cnt) i = cnt - 1;
    c->sel_index = i;
    c->dirty_sel = 1;
}

/* ============================================================ the value === */

const char *fc_default_value(struct node *n, int *len)
{
    static char buf[8192];
    int k = fc_kind(n);
    if (k == FC_TEXTAREA) {
        int l = text_content(n, buf, (int)sizeof buf);
        if (len) *len = l;
        return buf;
    }
    if (k == FC_SELECT) {
        int i = fc_selected_index(n);
        struct node *o = fc_option_at(n, i);
        int l = o ? fc_option_value(o, buf, (int)sizeof buf) : 0;
        if (!o) buf[0] = 0;
        if (len) *len = l;
        return buf;
    }
    const char *v = dom_attr(n, "value");
    if (!v) v = "";
    if (len) *len = slen(v);
    return v;
}

const char *fc_value(struct node *n, int *len)
{
    int k = fc_kind(n);
    if (k == FC_NONE) { if (len) *len = 0; return ""; }
    struct fctl *c = ent(n, 1);
    if (!c) { return fc_default_value(n, len); }
    if (!c->dirty_value || k == FC_SELECT) {
        /* Not dirty: the value TRACKS the content attribute, so re-derive it.
         * <select> always re-derives, because its value is a function of which
         * option is selected and that is tracked separately. */
        int dl = 0;
        const char *d = fc_default_value(n, &dl);
        if (dl != c->vlen || (dl && memcmp(c->val ? c->val : "", d, (size_t)dl) != 0))
            set_val(c, d, dl);
    }
    if (len) *len = c->vlen;
    return c->val ? c->val : "";
}

int fc_set_value(struct node *n, const char *s, int len)
{
    struct fctl *c = ent(n, 1);
    if (!c) return 0;
    if (fc_kind(n) == FC_SELECT) {
        /* select.value = x selects the option whose value is x, or deselects. */
        int cnt = fc_option_count(n), want = len < 0 ? slen(s) : len;
        char b[512];
        for (int i = 0; i < cnt; i++) {
            struct node *o = fc_option_at(n, i);
            int bl = fc_option_value(o, b, (int)sizeof b);
            if (bl == want && (want == 0 || memcmp(b, s, (size_t)want) == 0)) {
                fc_set_selected_index(n, i);
                return 1;
            }
        }
        fc_set_selected_index(n, -1);
        return 1;
    }
    if (!set_val(c, s, len)) return 0;
    c->dirty_value = 1;
    /* Per the spec, setting `value` moves the caret to the end and drops the
     * selection. Pages depend on it: an input-mask handler rewrites the value
     * on every keystroke and expects to keep typing at the end. */
    c->sel0 = c->sel1 = c->anchor = c->vlen;
    c->scroll_x = 0;
    return 1;
}

void fc_reset_control(struct node *n)
{
    struct fctl *c = ent(n, 0);
    if (!c) return;
    c->dirty_value = 0;
    c->dirty_checked = 0;
    c->dirty_sel = 0;
    c->sel0 = c->sel1 = c->anchor = 0;
    c->scroll_x = 0;
    int dl = 0;
    const char *d = fc_default_value(n, &dl);
    set_val(c, d, dl);
}

static void reset_walk(struct node *n, struct node *form)
{
    for (struct node *c = n->first_child; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (fc_kind(c) != FC_NONE && fc_form_of(c) == form) fc_reset_control(c);
        reset_walk(c, form);
    }
}

void fc_reset_form(struct node *form)
{
    if (!form) return;
    struct node *root = form;
    while (root->parent) root = root->parent;
    reset_walk(root, form);
}

/* =============================================================== checked === */

int fc_default_checked(struct node *n)
{ return n && n->type == N_ELEM && dom_attr(n, "checked") != 0; }

int fc_checked(struct node *n)
{
    int k = fc_kind(n);
    if (!FC_IS_TOGGLE(k)) return 0;
    struct fctl *c = ent(n, 1);
    if (!c) return fc_default_checked(n);
    if (!c->dirty_checked) return fc_default_checked(n);
    return c->checked;
}

/* The radio group: same form owner, same `name`. Walking the whole document
 * and filtering by fc_form_of is deliberate -- a radio can be pulled out of its
 * form's subtree by the `form` attribute, and the group has to follow. */
static void uncheck_group(struct node *radio)
{
    const char *nm = dom_attr(radio, "name");
    if (!nm || !nm[0]) return;              /* an unnamed radio is its own group */
    struct node *form = fc_form_of(radio);
    struct node *root = radio;
    while (root->parent) root = root->parent;

    struct node *stack[128];
    int sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        struct node *e = stack[--sp];
        for (struct node *c = e->first_child; c; c = c->next) {
            if (c->type != N_ELEM) continue;
            if (sp < 128) stack[sp++] = c;
            if (c == radio || fc_kind(c) != FC_RADIO) continue;
            const char *n2 = dom_attr(c, "name");
            if (!n2 || slen(n2) != slen(nm) || memcmp(n2, nm, (size_t)slen(nm)) != 0) continue;
            if (fc_form_of(c) != form) continue;
            struct fctl *o = ent(c, 1);
            if (o) { o->checked = 0; o->dirty_checked = 1; }
        }
    }
}

void fc_set_checked(struct node *n, int on)
{
    int k = fc_kind(n);
    if (!FC_IS_TOGGLE(k)) return;
    struct fctl *c = ent(n, 1);
    if (!c) return;
    if (k == FC_RADIO && on) uncheck_group(n);
    c->checked = (unsigned char)!!on;
    c->dirty_checked = 1;
}

/* ============================================================= selection === */

void fc_selection(struct node *n, int *start, int *end)
{
    struct fctl *c = ent(n, 1);
    int vl = 0;
    fc_value(n, &vl);
    int a = c ? c->sel0 : 0, b = c ? c->sel1 : 0;
    if (a > vl) a = vl;
    if (b > vl) b = vl;
    if (start) *start = a;
    if (end) *end = b;
}

void fc_set_selection(struct node *n, int start, int end)
{
    struct fctl *c = ent(n, 1);
    if (!c) return;
    int vl = 0;
    fc_value(n, &vl);
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if (start > vl) start = vl;
    if (end > vl) end = vl;
    if (end < start) end = start;
    c->sel0 = start; c->sel1 = end; c->anchor = start;
}

/* ========================================================== text editing === */

/* UTF-8 is stepped by CHARACTER, not by byte: a caret that lands in the middle
 * of a multi-byte sequence produces a text run the rasterizer cannot decode,
 * and backspacing one byte off a CJK character turns it into mojibake rather
 * than deleting it. The rule is the usual one -- continuation bytes are
 * 10xxxxxx, so skip while (b & 0xC0) == 0x80. */
static int step_left(const char *s, int i)
{
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static int step_right(const char *s, int len, int i)
{
    if (i >= len) return len;
    i++;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static int is_wordch(unsigned char c)
{ return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80; }

/* maxlength counts CHARACTERS here; the spec counts UTF-16 code units. They
 * agree for everything below U+10000, which is everything a form field
 * realistically holds, and disagreeing by an astral character is a better
 * failure than not enforcing the attribute at all. */
static int char_count(const char *s, int len)
{
    int n = 0;
    for (int i = 0; i < len; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

static int max_length(struct node *n)
{
    const char *v = dom_attr(n, "maxlength");
    if (!v) return -1;
    int x = 0, any = 0;
    for (const char *p = v; *p >= '0' && *p <= '9'; p++) { x = x * 10 + (*p - '0'); any = 1; if (x > 1000000) break; }
    return any ? x : -1;
}

static int editable(struct node *n)
{
    int k = fc_kind(n);
    if (!FC_IS_TEXTUAL(k)) return 0;
    if (fc_disabled(n) || fc_readonly(n)) return 0;
    return 1;
}

/* Replace [a,b) with `s`. The one mutation primitive; every editing operation
 * below is a call to this with a different range, which is also why they all
 * fire the same events in the same order. */
static int splice(struct node *n, int a, int b, const char *s, int len,
                  const char *itype)
{
    if (!itype) itype = "insertText";
    struct fctl *c = ent(n, 1);
    if (!c) return 0;
    int vl = 0;
    fc_value(n, &vl);
    if (a < 0) a = 0;
    if (b > vl) b = vl;
    if (b < a) b = a;
    if (len < 0) len = slen(s);
    if (len == 0 && a == b) return 0;

    if (len > 0) {
        int ml = max_length(n);
        if (ml >= 0) {
            int have = char_count(c->val, vl) - char_count(c->val + a, b - a);
            int room = ml - have;
            if (room <= 0) return 0;
            /* Truncate the insertion on a character boundary. */
            int used = 0, cut = 0;
            while (cut < len) {
                int nx = step_right(s, len, cut);
                if (used + 1 > room) break;
                used++; cut = nx;
            }
            len = cut;
            if (len == 0) return 0;
        }
    }

    /* beforeinput is cancelable and this is the only place it can be honoured:
     * a page that returns false from it expects the value NOT to change.
     *
     * The inputType goes out with it and with the `input` below. It is the
     * field a framework-managed control actually reads -- see focus.h -- and
     * the two events must agree on it, which is why it is one parameter
     * threaded through the single mutation primitive rather than a decision
     * made twice. `data` is the inserted text and NULL for a deletion, which is
     * what InputEvent.data is defined to be. */
    const char *data = (len > 0) ? s : 0;
    static char dbuf[512];
    if (data) {
        int dl = len < (int)sizeof dbuf - 1 ? len : (int)sizeof dbuf - 1;
        for (int i = 0; i < dl; i++) dbuf[i] = s[i];
        dbuf[dl] = 0;
        data = dbuf;
    }
    if (!fc_dispatch_input(n, "beforeinput", itype, data, 1, 1)) return 0;

    int nl = vl - (b - a) + len;
    char *nb = (char *)malloc((size_t)nl + 1);
    if (!nb) return 0;
    for (int i = 0; i < a; i++) nb[i] = c->val[i];
    for (int i = 0; i < len; i++) nb[a + i] = s[i];
    for (int i = b; i < vl; i++) nb[a + len + (i - b)] = c->val[i];
    nb[nl] = 0;
    int ok = set_val(c, nb, nl);
    free(nb);
    if (!ok) return 0;
    c->dirty_value = 1;
    c->sel0 = c->sel1 = c->anchor = a + len;
    fc_dispatch_input(n, "input", itype, data, 1, 0);
    return 1;
}

int fc_edit_replace(struct node *n, const char *s, int len, const char *itype)
{
    if (!editable(n)) return 0;
    struct fctl *c = ent(n, 1);
    if (!c) return 0;
    /* <input> is single-line: a pasted newline becomes a space, which is what
     * every browser does and what stops a paste from silently losing text. */
    if (fc_kind(n) != FC_TEXTAREA) {
        static char buf[4096];
        int o = 0;
        if (len < 0) len = slen(s);
        for (int i = 0; i < len && o < (int)sizeof buf; i++)
            buf[o++] = (s[i] == '\n' || s[i] == '\r') ? ' ' : s[i];
        return splice(n, c->sel0, c->sel1, buf, o, itype);
    }
    return splice(n, c->sel0, c->sel1, s, len, itype);
}

int fc_edit_insert(struct node *n, const char *s, int len)
{ return fc_edit_replace(n, s, len, "insertText"); }

int fc_edit_backspace(struct node *n)
{
    if (!editable(n)) return 0;
    struct fctl *c = ent(n, 1);
    if (!c) return 0;
    if (c->sel0 != c->sel1) return splice(n, c->sel0, c->sel1, "", 0, "deleteContentBackward");
    if (c->sel0 == 0) return 0;
    int a = step_left(c->val ? c->val : "", c->sel0);
    return splice(n, a, c->sel0, "", 0, "deleteContentBackward");
}

int fc_edit_delete(struct node *n)
{
    if (!editable(n)) return 0;
    struct fctl *c = ent(n, 1);
    if (!c) return 0;
    int vl = 0;
    fc_value(n, &vl);
    if (c->sel0 != c->sel1) return splice(n, c->sel0, c->sel1, "", 0, "deleteContentForward");
    if (c->sel1 >= vl) return 0;
    int b = step_right(c->val ? c->val : "", vl, c->sel1);
    return splice(n, c->sel1, b, "", 0, "deleteContentForward");
}

/* Collapse or extend, then normalise. The anchor is the fixed end of a
 * shift-selection; without it, shift-left then shift-right would grow the
 * selection in both directions instead of shrinking it. */
static int move_to(struct fctl *c, int pos, int extend)
{
    int o0 = c->sel0, o1 = c->sel1;
    if (extend) {
        if (c->sel0 == c->sel1) c->anchor = c->sel0;
        if (pos < c->anchor) { c->sel0 = pos; c->sel1 = c->anchor; }
        else                 { c->sel0 = c->anchor; c->sel1 = pos; }
    } else {
        c->sel0 = c->sel1 = c->anchor = pos;
    }
    return c->sel0 != o0 || c->sel1 != o1;
}

int fc_edit_move(struct node *n, int dir, int word, int extend)
{
    int k = fc_kind(n);
    if (!FC_IS_TEXTUAL(k)) return 0;
    struct fctl *c = ent(n, 1);
    if (!c) return 0;
    int vl = 0;
    const char *v = fc_value(n, &vl);
    /* A plain arrow with a selection collapses to that end rather than moving
     * one character from it -- the behaviour every editor has and the one a
     * caret model gets wrong by default. */
    int cur = (dir < 0) ? c->sel0 : c->sel1;
    if (!extend && c->sel0 != c->sel1) return move_to(c, cur, 0);
    int pos = (c->sel0 == c->sel1) ? c->sel0 : cur;
    if (dir < 0) {
        pos = step_left(v, pos);
        if (word) {
            while (pos > 0 && !is_wordch((unsigned char)v[pos])) pos = step_left(v, pos);
            while (pos > 0 && is_wordch((unsigned char)v[step_left(v, pos)])) pos = step_left(v, pos);
        }
    } else {
        pos = step_right(v, vl, pos);
        if (word) {
            while (pos < vl && !is_wordch((unsigned char)v[pos])) pos = step_right(v, vl, pos);
            while (pos < vl && is_wordch((unsigned char)v[pos])) pos = step_right(v, vl, pos);
        }
    }
    return move_to(c, pos, extend);
}

/* Home/End are per-LINE in a textarea and per-value in an input, which is the
 * same code once "the line" is found. */
static void line_bounds(const char *v, int vl, int pos, int *a, int *b)
{
    int s = pos;
    while (s > 0 && v[s - 1] != '\n') s--;
    int e = pos;
    while (e < vl && v[e] != '\n') e++;
    *a = s; *b = e;
}

int fc_edit_home(struct node *n, int extend)
{
    if (!FC_IS_TEXTUAL(fc_kind(n))) return 0;
    struct fctl *c = ent(n, 1);
    if (!c) return 0;
    int vl = 0;
    const char *v = fc_value(n, &vl);
    int a, b;
    line_bounds(v, vl, c->sel0, &a, &b);
    return move_to(c, a, extend);
}

int fc_edit_end(struct node *n, int extend)
{
    if (!FC_IS_TEXTUAL(fc_kind(n))) return 0;
    struct fctl *c = ent(n, 1);
    if (!c) return 0;
    int vl = 0;
    const char *v = fc_value(n, &vl);
    int a, b;
    line_bounds(v, vl, c->sel1, &a, &b);
    return move_to(c, b, extend);
}

int fc_edit_select_all(struct node *n)
{
    if (!FC_IS_TEXTUAL(fc_kind(n))) return 0;
    struct fctl *c = ent(n, 1);
    if (!c) return 0;
    int vl = 0;
    fc_value(n, &vl);
    c->sel0 = 0; c->sel1 = vl; c->anchor = 0;
    return 1;
}

int fc_mark_focus(struct node *n)
{
    struct fctl *c = ent(n, 1);
    if (!c) return 0;
    int vl = 0;
    const char *v = fc_value(n, &vl);
    char *nb = (char *)realloc(c->fval, (size_t)vl + 1);
    if (!nb) return 0;
    for (int i = 0; i < vl; i++) nb[i] = v[i];
    nb[vl] = 0;
    c->fval = nb; c->fvlen = vl;
    return 1;
}

int fc_commit(struct node *n)
{
    struct fctl *c = ent(n, 0);
    if (!c) return 0;
    int vl = 0;
    const char *v = fc_value(n, &vl);
    if (c->fval && c->fvlen == vl && (vl == 0 || memcmp(c->fval, v, (size_t)vl) == 0)) return 0;
    if (!c->fval && vl == 0) return 0;
    fc_mark_focus(n);
    fc_dispatch(n, "change", 1, 0);
    return 1;
}

/* ============================================================ the popup === */

int fc_select_open(struct node *n)
{ struct fctl *c = ent(n, 0); return c ? c->open : 0; }

void fc_select_set_open(struct node *n, int on)
{ struct fctl *c = ent(n, 1); if (c) c->open = (unsigned char)!!on; }

/* ============================================================ submission === */

static fc_submit_fn g_submit;
void fc_set_submit(fc_submit_fn fn) { g_submit = fn; }
int  fc_submit(struct node *form, struct node *submitter, int fire_event)
{ return g_submit ? g_submit(form, submitter, fire_event) : 0; }

static int urlenc(char *out, int o, int max, const char *s, int len)
{
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '*' || ch == '-' || ch == '.' || ch == '_') {
            if (o >= max) return -1;
            out[o++] = (char)ch;
        } else if (ch == ' ') {
            if (o >= max) return -1;
            out[o++] = '+';
        } else {
            if (o + 3 > max) return -1;
            out[o++] = '%'; out[o++] = hex[ch >> 4]; out[o++] = hex[ch & 15];
        }
    }
    return o;
}

/* The `application/x-www-form-urlencoded` serialiser. Newlines in a textarea
 * are normalised to CRLF first, which is in the spec and which servers that
 * split on \r\n depend on. */
static int enc_pair(char *out, int o, int max, const char *nm, int nl,
                    const char *v, int vl, int first, int crlf)
{
    if (!first) { if (o >= max) return -1; out[o++] = '&'; }
    o = urlenc(out, o, max, nm, nl);
    if (o < 0) return -1;
    if (o >= max) return -1;
    out[o++] = '=';
    if (!crlf) return urlenc(out, o, max, v, vl);
    for (int i = 0; i < vl; i++) {
        if (v[i] == '\r') continue;                     /* re-emitted with the \n */
        if (v[i] == '\n') { o = urlenc(out, o, max, "\r\n", 2); }
        else              { o = urlenc(out, o, max, v + i, 1); }
        if (o < 0) return -1;
    }
    return o;
}

struct enc_ctx { char *out; int o, max, first, err; struct node *form, *submitter; };

static void enc_one(struct enc_ctx *e, struct node *n)
{
    int k = fc_kind(n);
    if (k == FC_NONE) return;
    if (fc_form_of(n) != e->form) return;
    if (fc_disabled(n)) return;
    const char *nm = dom_attr(n, "name");
    int nl = slen(nm);
    if (!nm || nl == 0) return;
    if (FC_IS_BUTTON(k)) {
        /* Only the button that was actually activated contributes, and a reset
         * button never submits anything at all. */
        if (n != e->submitter || k == FC_RESET) return;
        const char *v = dom_attr(n, "value");
        int o = enc_pair(e->out, e->o, e->max, nm, nl, v ? v : "", v ? slen(v) : 0, e->first, 0);
        if (o < 0) { e->err = 1; return; }
        e->o = o; e->first = 0;
        return;
    }
    if (FC_IS_TOGGLE(k)) {
        if (!fc_checked(n)) return;
        const char *v = dom_attr(n, "value");
        if (!v) v = "on";                       /* the HTML default */
        int o = enc_pair(e->out, e->o, e->max, nm, nl, v, slen(v), e->first, 0);
        if (o < 0) { e->err = 1; return; }
        e->o = o; e->first = 0;
        return;
    }
    if (k == FC_FILE) {
        /* No file picker on this machine, so the field is empty rather than
         * absent -- which is what a real browser sends for an untouched file
         * input under urlencoded, and keeps the field count right. */
        int o = enc_pair(e->out, e->o, e->max, nm, nl, "", 0, e->first, 0);
        if (o < 0) { e->err = 1; return; }
        e->o = o; e->first = 0;
        return;
    }
    if (k == FC_SELECT) {
        /* `multiple` submits every selected option; we track one selection, so
         * this is the single-select case and a multiple select contributes its
         * one current index. Stated as a limitation in the report. */
        int i = fc_selected_index(n);
        struct node *opt = fc_option_at(n, i);
        if (!opt) return;
        char b[512];
        int bl = fc_option_value(opt, b, (int)sizeof b);
        int o = enc_pair(e->out, e->o, e->max, nm, nl, b, bl, e->first, 0);
        if (o < 0) { e->err = 1; return; }
        e->o = o; e->first = 0;
        return;
    }
    int vl = 0;
    const char *v = fc_value(n, &vl);
    int o = enc_pair(e->out, e->o, e->max, nm, nl, v, vl, e->first, k == FC_TEXTAREA);
    if (o < 0) { e->err = 1; return; }
    e->o = o; e->first = 0;
}

static void enc_walk(struct enc_ctx *e, struct node *n)
{
    for (struct node *c = n->first_child; c && !e->err; c = c->next) {
        if (c->type != N_ELEM) continue;
        enc_one(e, c);
        enc_walk(e, c);
    }
}

int fc_encode(struct node *form, struct node *submitter, char *out, int max)
{
    if (!form || max <= 0) return -1;
    struct node *root = form;
    while (root->parent) root = root->parent;
    struct enc_ctx e = { out, 0, max - 1, 1, 0, form, submitter };
    /* Document-wide, filtered by fc_form_of: the `form` attribute lets a
     * control live anywhere in the document and still belong to this form, and
     * walking the form's subtree alone would silently drop it. */
    enc_walk(&e, root);
    if (e.err) return -1;
    out[e.o] = 0;
    return e.o;
}

int fc_method_post(struct node *form)
{
    const char *m = form ? dom_attr(form, "method") : 0;
    return fc__ieq(m, "post");
}

const char *fc_action(struct node *form)
{
    const char *a = form ? dom_attr(form, "action") : 0;
    return a ? a : "";
}

/* ============================================================== painting === */

/* Password masking. U+2022 BULLET, the character every UA uses; three bytes of
 * UTF-8 each, so a 200-character password needs 600 bytes and anything longer
 * is truncated for display only -- the VALUE is untouched. */
static char g_mask[3 * 256 + 1];

static const char *mask_of(const char *s, int len, int *out_len)
{
    int chars = char_count(s, len);
    if (chars > 256) chars = 256;
    int o = 0;
    for (int i = 0; i < chars; i++) {
        g_mask[o++] = (char)0xE2; g_mask[o++] = (char)0x80; g_mask[o++] = (char)0xA2;
    }
    g_mask[o] = 0;
    *out_len = o;
    return g_mask;
}

/* The caret's x, in px from the content origin, for a byte offset into `text`.
 * For a textarea that is measured within the caret's own LINE. */
static int caret_px(const char *text, int off, int font_px, int mono, int *line_out)
{
    int line = 0, ls = 0;
    for (int i = 0; i < off; i++) if (text[i] == '\n') { line++; ls = i + 1; }
    if (line_out) *line_out = line;
    return text_measure(text + ls, off - ls, font_px, mono);
}

int fc_paint_state(struct node *n, int font_px, int mono, int content_w, struct fpaint *out)
{
    int k = fc_kind(n);
    if (k == FC_NONE || !out) return 0;
    memset(out, 0, sizeof *out);
    out->kind = k;
    out->caret_x = -1;
    out->pad_x = FC_PAD_X;
    out->pad_y = FC_PAD_Y;
    out->disabled = fc_disabled(n);
    out->readonly = fc_readonly(n);
    out->focused = (focus_current() == n);
    out->line_h = font_px + font_px / 4;
    out->nline = 1;

    if (FC_IS_TOGGLE(k)) { out->checked = fc_checked(n); return 1; }

    if (FC_IS_BUTTON(k) || k == FC_FILE) {
        /* A <button>'s label is its CHILD CONTENT, and layout.c lays that out
         * inside the box as real inline flow -- icons, spans and all. So the
         * painter must draw no text for one, or every <button> would show its
         * label twice, once from each. An <input type=submit> is the opposite
         * case: it has no children and its label is an attribute, so the
         * painter is the only thing that can draw it. */
        if (tag_is(n->tag, "button")) return 1;
        static char lbl[256];
        const char *v = dom_attr(n, "value");
        if (!v) v = (k == FC_SUBMIT) ? "Submit" :
                    (k == FC_RESET)  ? "Reset"  :
                    (k == FC_FILE)   ? "Choose File" : "";
        int l = slen(v);
        if (l > (int)sizeof lbl - 1) l = (int)sizeof lbl - 1;
        for (int i = 0; i < l; i++) lbl[i] = v[i];
        lbl[l] = 0;
        out->text = lbl; out->len = l;
        out->text_w = text_measure(lbl, l, font_px, mono);
        return 1;
    }

    if (k == FC_SELECT) {
        static char lbl[256];
        struct node *opt = fc_option_at(n, fc_selected_index(n));
        int l = opt ? fc_option_label(opt, lbl, (int)sizeof lbl) : 0;
        if (!opt) lbl[0] = 0;
        out->text = lbl; out->len = l;
        out->text_w = text_measure(lbl, l, font_px, mono);
        return 1;
    }

    if (k == FC_HIDDEN || k == FC_RANGE || k == FC_COLOR) return 1;

    /* --- textual --- */
    struct fctl *c = ent(n, 1);
    int vl = 0;
    const char *v = fc_value(n, &vl);
    const char *shown = v;
    int shown_len = vl;
    if (k == FC_PASSWORD) shown = mask_of(v, vl, &shown_len);

    if (vl == 0) {
        const char *ph = dom_attr(n, "placeholder");
        if (ph && ph[0]) {
            out->text = ph; out->len = slen(ph);
            out->placeholder = 1;
            out->text_w = text_measure(ph, out->len, font_px, mono);
            /* The caret still belongs at the start; a placeholder does not
             * displace it. */
            if (out->focused) out->caret_x = 0;
            return 1;
        }
    }
    out->text = shown; out->len = shown_len;
    out->text_w = text_measure(shown, shown_len, font_px, mono);

    int nl = 1;
    for (int i = 0; i < shown_len; i++) if (shown[i] == '\n') nl++;
    out->nline = nl;

    if (c) {
        /* The caret and the selection are offsets into the REAL value; on a
         * password field they have to be mapped onto the mask, where every
         * character is three bytes. Measuring the real text would leak the
         * password's glyph widths through the caret position. */
        int s0 = c->sel0, s1 = c->sel1;
        if (k == FC_PASSWORD) {
            s0 = char_count(v, s0) * 3;
            s1 = char_count(v, s1) * 3;
            if (s0 > shown_len) s0 = shown_len;
            if (s1 > shown_len) s1 = shown_len;
        }
        int line = 0;
        int cx = caret_px(shown, s1, font_px, mono, &line);
        out->caret_line = line;
        if (out->focused) out->caret_x = cx;
        if (s0 != s1) {
            int l0 = 0;
            out->sel_x0 = caret_px(shown, s0, font_px, mono, &l0);
            out->sel_x1 = cx;
            /* A selection spanning lines is drawn only on the caret's line;
             * multi-line selection painting is not built. */
            if (l0 != line) out->sel_x0 = 0;
        }
        /* Keep the caret in view. Scrolling is stored so it survives repaints
         * -- recomputing it per frame would make a long value jump back to the
         * start every time the page re-laid out. */
        if (content_w > 0 && k != FC_TEXTAREA) {
            if (cx - c->scroll_x > content_w - 2) c->scroll_x = cx - content_w + 2;
            if (cx < c->scroll_x) c->scroll_x = cx;
            int maxs = out->text_w - content_w;
            if (maxs < 0) maxs = 0;
            if (c->scroll_x > maxs) c->scroll_x = maxs;
            if (c->scroll_x < 0) c->scroll_x = 0;
        }
        out->scroll_x = c->scroll_x;
    }
    return 1;
}

/* The byte offset in the value nearest to `px` from the content origin. Used to
 * place the caret from a click. Linear because a form field is short and a
 * binary search over a measurement that is not monotone in bytes (it is
 * monotone in characters) is a bug waiting to happen. */
int fc_offset_at_px(struct node *n, int px, int font_px, int mono)
{
    int k = fc_kind(n);
    if (!FC_IS_TEXTUAL(k)) return 0;
    int vl = 0;
    const char *v = fc_value(n, &vl);
    struct fctl *c = ent(n, 1);
    int x0 = px + (c ? c->scroll_x : 0);
    if (x0 <= 0) return 0;
    int best = 0, bestd = -1;
    int i = 0;
    for (;;) {
        int w = text_measure(v, i, font_px, mono);
        int d = w > x0 ? w - x0 : x0 - w;
        if (bestd < 0 || d < bestd) { bestd = d; best = i; }
        if (i >= vl) break;
        i = step_right(v, vl, i);
    }
    return best;
}

/* ==================================================== contenteditable ===== *
 *
 * Read the block comment in forms.h first: it says what a position is and why
 * none of the state above can be reused. What follows is, in order: the host
 * test, tree walking, the position primitives, the mutation primitives, and
 * then the six editing operations built out of them.
 *
 * THE ONE RULE THAT KEEPS THIS SAFE. Every stored position carries the node's
 * `serial`, and every read re-checks it AND re-checks that the node is still
 * attached to a document. A composer on a React page is rebuilt from scratch on
 * a keystroke; without that check this file would hold a pointer into a
 * recycled slot and type into whatever element landed there. Same discipline as
 * focus.c, for the same reason, and it is the difference between "the caret was
 * lost" (what a real browser does too) and silent corruption.
 */

/* A stored position. See forms.h: `n` is a text node with a byte offset, or an
 * element with a child index. */
struct cepos { struct node *n; uint32_t serial; int off; };

static struct cepos g_ce_a, g_ce_f;     /* anchor, focus */

/* The editing events fire through fc_dispatch_input (focus.h), which carries
 * the inputType. CE_NO_INPUT_EVENTS is THE NEGATIVE CONTROL: it removes the
 * events and NOTHING else, so the characters still land in the DOM and still
 * paint. A human looking at the screen cannot tell the two builds apart; every
 * framework-managed composer stays empty in the second one, because React never
 * learns the value changed. If the test suite passes against this build, the
 * suite is measuring the pixels and not the feature. */
#ifdef CE_NO_INPUT_EVENTS
static int ce_fire(struct node *t, const char *type, const char *itype,
                   const char *data, int cancelable)
{ (void)t; (void)type; (void)itype; (void)data; (void)cancelable; return 1; }
#else
static int ce_fire(struct node *t, const char *type, const char *itype,
                   const char *data, int cancelable)
{ return fc_dispatch_input(t, type, itype, data, 1, cancelable); }
#endif

/* ------------------------------------------------------- the host test -- */

/* -1 absent/inherit, 0 false, 1 true. `contenteditable=""` is true (the empty
 * string is the attribute's "true" keyword), and that spelling is common enough
 * in shipped markup that treating it as absent breaks real pages. */
static int ce_attr(const struct node *n)
{
    const char *v = dom_attr(n, "contenteditable");
    if (!v) return -1;
    if (!v[0]) return 1;
    if (v[0] == 't' || v[0] == 'T') return 1;
    if (v[0] == 'f' || v[0] == 'F') return 0;
    if (v[0] == 'p' || v[0] == 'P') return 1;   /* plaintext-only */
    return -1;                                   /* inherit, or a typo */
}

struct node *fc_ce_host(struct node *n)
{
    if (!n) return 0;
    if (n->type != N_ELEM) n = n->parent;
    for (; n && n->type == N_ELEM; n = n->parent) {
        int a = ce_attr(n);
        if (a == 0) return 0;       /* an explicit false shadows the host above */
        if (a == 1) return n;
    }
    return 0;
}

int fc_ce_editable(struct node *n) { return fc_ce_host(n) != 0; }

/* ---------------------------------------------------------- tree walk --- */

static int ce_attached(struct node *n)
{
    if (!n) return 0;
    struct node *p = n;
    while (p->parent) p = p->parent;
    return p->type == N_DOCUMENT;
}

static int pos_live(const struct cepos *p)
{ return p->n && p->n->serial == p->serial && ce_attached(p->n); }

static int ce_nchild(const struct node *n);

/* Offsets are CLAMPED here and nowhere else. A caller that computes one from a
 * click, from a path handed over by JavaScript, or from a length it read before
 * a mutation can all be one past the end; an unclamped offset then reads past
 * the node's text and the failure surfaces somewhere else entirely. */
static void pos_set(struct cepos *p, struct node *n, int off)
{
    if (n) {
        int max = (n->type == N_TEXT) ? n->textlen : ce_nchild(n);
        if (off > max) off = max;
        if (off < 0) off = 0;
    }
    p->n = n; p->serial = n ? n->serial : 0; p->off = off;
}

/* A subtree a caret never enters and text traversal never descends into. The
 * contenteditable="false" test is the interesting one: a page marks a mention
 * chip or an emoji non-editable and expects the caret to step OVER it, not
 * into it. */
static int ce_skip(const struct node *n)
{
    if (!n || n->type != N_ELEM) return 0;
    if (tag_is(n->tag, "script") || tag_is(n->tag, "style") ||
        tag_is(n->tag, "template") || tag_is(n->tag, "select") ||
        tag_is(n->tag, "textarea") || tag_is(n->tag, "noscript")) return 1;
    if (ce_attr(n) == 0) return 1;
    const struct cstyle *st = (const struct cstyle *)n->style;
    if (st && st->display == DISP_NONE) return 1;
    return 0;
}

/* The next / previous TEXT node inside `host`, in document order. `from` may be
 * the host itself, which asks for the first / last. */
static struct node *ce_next_text(struct node *host, struct node *from)
{
    struct node *n = from;
    if (!n || !host) return 0;
    for (;;) {
        if (n->type == N_ELEM && n->first_child && !ce_skip(n)) {
            n = n->first_child;
        } else {
            while (n && n != host && !n->next) n = n->parent;
            if (!n || n == host) return 0;
            n = n->next;
        }
        if (!n) return 0;
        if (n->type == N_TEXT) return n;
    }
}

static struct node *ce_prev_text(struct node *host, struct node *from)
{
    struct node *n = from;
    if (!n || !host) return 0;
    if (n == host) {                       /* "the last text node in the host" */
        while (n->type == N_ELEM && n->last_child && !ce_skip(n)) n = n->last_child;
        if (n->type == N_TEXT) return n;
    }
    for (;;) {
        if (n == host) return 0;
        if (n->prev) {
            n = n->prev;
            while (n->type == N_ELEM && n->last_child && !ce_skip(n)) n = n->last_child;
        } else {
            n = n->parent;
            if (!n || n == host) return 0;
            continue;
        }
        if (n->type == N_TEXT) return n;
    }
}

static int ce_index(const struct node *n)
{ int i = 0; for (const struct node *p = n->prev; p; p = p->prev) i++; return i; }

static int ce_nchild(const struct node *n)
{ int i = 0; for (const struct node *c = n->first_child; c; c = c->next) i++; return i; }

static struct node *ce_child_at(struct node *n, int i)
{
    if (i < 0) return 0;
    for (struct node *c = n->first_child; c; c = c->next) if (i-- == 0) return c;
    return 0;
}

static int ce_is_ancestor(const struct node *a, const struct node *b)
{ for (const struct node *p = b; p; p = p->parent) if (p == a) return 1; return 0; }

static int ce_depth(const struct node *n)
{ int d = 0; while (n->parent) { n = n->parent; d++; } return d; }

/* Document order over two positions: -1 a before b, 0 equal, +1 after. Needed
 * because a Shift+Arrow selection can be built in either direction and every
 * consumer -- the painter, the deletion, Selection.toString -- wants the pair
 * ordered. */
static int ce_cmp(struct node *a, int ao, struct node *b, int bo)
{
    if (a == b) return ao < bo ? -1 : ao > bo ? 1 : 0;
    int da = ce_depth(a), db = ce_depth(b);
    int a_deeper = da > db;
    struct node *x = a, *y = b;
    while (da > db) { x = x->parent; da--; }
    while (db > da) { y = y->parent; db--; }
    if (x == y) {
        /* One is an ancestor of the other; compare the ancestor's offset
         * against the child index the descendant hangs off. */
        if (a_deeper) {                       /* b is the ancestor */
            struct node *c = a;
            while (c->parent != b) c = c->parent;
            return ce_index(c) < bo ? -1 : 1;
        }
        struct node *c = b;
        while (c->parent != a) c = c->parent;
        return ao <= ce_index(c) ? -1 : 1;
    }
    while (x && y && x->parent != y->parent) { x = x->parent; y = y->parent; }
    if (!x || !y) return 0;                   /* different trees: no order */
    return ce_index(x) < ce_index(y) ? -1 : 1;
}

/* ------------------------------------------------------------ blocks ---- */

static int ce_block_tag(const char *t)
{
    static const char *B[] = { "div", "p", "li", "ul", "ol", "h1", "h2", "h3",
        "h4", "h5", "h6", "blockquote", "pre", "section", "article", "header",
        "footer", "main", "aside", "figure", "figcaption", "dd", "dt", "dl",
        "td", "th", "tr", "table", "form", "fieldset", "address", "hr", 0 };
    for (int i = 0; B[i]; i++) if (tag_is(t, B[i])) return 1;
    return 0;
}

/* An element the caret treats as a paragraph boundary. The computed style wins
 * when there is one; a node created by an edit a moment ago has none yet, and
 * the tag is the right answer for it until the cascade next runs. */
static int ce_is_block(const struct node *n)
{
    if (!n || n->type != N_ELEM) return 0;
    const struct cstyle *st = (const struct cstyle *)n->style;
    if (st) {
        if (st->display == DISP_BLOCK || st->display == DISP_FLEX ||
            st->display == DISP_GRID) return 1;
        if (st->display == DISP_INLINE || st->display == DISP_INLINE_BLOCK) return 0;
    }
    return ce_block_tag(n->tag);
}

/* The paragraph the position is in, STRICTLY inside the host. NULL means the
 * caret sits in the host's own inline content -- a bare `<div contenteditable>`
 * with nothing but text in it, which is what an empty composer usually is. */
static struct node *ce_block_of(struct node *n, struct node *host)
{
    if (!n) return 0;
    if (n->type != N_ELEM) n = n->parent;
    for (; n && n != host; n = n->parent)
        if (ce_is_block(n)) return n;
    return 0;
}

/* ------------------------------------------------- the text primitive --- */

/* Replace [a,b) of a text node with `s`. dom.c has no "set the text" entry
 * point, so this rebuilds through dom_text_append -- which grows in the
 * document's own arena, which is what keeps the node's storage owned by the
 * document rather than by this file. The old block is abandoned, which is the
 * arena's bargain and is bounded by the length of the run. */
static int ce_text_splice(struct node *t, int a, int b, const char *s, int len)
{
    if (!t || t->type != N_TEXT) return 0;
    int tl = t->textlen;
    if (a < 0) a = 0;
    if (a > tl) a = tl;
    if (b > tl) b = tl;
    if (b < a) b = a;
    if (len < 0) len = slen(s);
    int nl = tl - (b - a) + len;
    if (nl == 0) {
        t->textlen = 0;
        if (t->text && t->textcap > 0) t->text[0] = 0;
        return 1;
    }
    char *nb = (char *)malloc((size_t)nl + 1);
    if (!nb) return 0;
    for (int i = 0; i < a; i++) nb[i] = t->text[i];
    for (int i = 0; i < len; i++) nb[a + i] = s[i];
    for (int i = b; i < tl; i++) nb[a + len + (i - b)] = t->text[i];
    nb[nl] = 0;
    t->textlen = 0;
    int ok = dom_text_append(t, nb, nl);
    free(nb);
    return ok;
}

/* ------------------------------------------------------- normalisation -- */

/* Turn an element position into a text position when one exists, because every
 * operation below is simpler over text and the two are the same place. An
 * element position that cannot be normalised is a genuinely empty container,
 * which is the case insertion has to handle specially and nothing else does. */
static void ce_norm(struct node **np, int *op)
{
    struct node *n = *np;
    if (!n || n->type != N_ELEM) return;
    struct node *at = ce_child_at(n, *op);
    if (at && at->type == N_TEXT) { *np = at; *op = 0; return; }
    struct node *before = *op > 0 ? ce_child_at(n, *op - 1) : 0;
    if (before && before->type == N_TEXT) { *np = before; *op = before->textlen; return; }
}

/* ------------------------------------------------------ the public caret */

int fc_ce_anchor_focus(struct node **an, int *ao, struct node **fn, int *fo)
{
    if (!pos_live(&g_ce_a) || !pos_live(&g_ce_f)) return 0;
    if (an) *an = g_ce_a.n;
    if (ao) *ao = g_ce_a.off;
    if (fn) *fn = g_ce_f.n;
    if (fo) *fo = g_ce_f.off;
    return 1;
}

int fc_ce_selection(struct node **sn, int *so, struct node **en, int *eo)
{
    if (!pos_live(&g_ce_a) || !pos_live(&g_ce_f)) return 0;
    int c = ce_cmp(g_ce_a.n, g_ce_a.off, g_ce_f.n, g_ce_f.off);
    const struct cepos *s = c <= 0 ? &g_ce_a : &g_ce_f;
    const struct cepos *e = c <= 0 ? &g_ce_f : &g_ce_a;
    if (sn) *sn = s->n;
    if (so) *so = s->off;
    if (en) *en = e->n;
    if (eo) *eo = e->off;
    return 1;
}

int fc_ce_collapsed(void)
{
    if (!pos_live(&g_ce_a) || !pos_live(&g_ce_f)) return 1;
    return g_ce_a.n == g_ce_f.n && g_ce_a.off == g_ce_f.off;
}

struct node *fc_ce_caret_host(void)
{
    if (!pos_live(&g_ce_f)) return 0;
    return fc_ce_host(g_ce_f.n);
}

void fc_ce_clear(void) { pos_set(&g_ce_a, 0, 0); pos_set(&g_ce_f, 0, 0); }

void fc_ce_set_caret(struct node *n, int off)
{
    if (!n) { fc_ce_clear(); return; }
    pos_set(&g_ce_a, n, off);
    pos_set(&g_ce_f, n, off);
}

void fc_ce_set_range(struct node *an, int ao, struct node *fn, int fo)
{
    if (!an || !fn) { fc_ce_clear(); return; }
    pos_set(&g_ce_a, an, ao);
    pos_set(&g_ce_f, fn, fo);
}

/* Place the caret inside `el` when a click found no glyph to aim at. THIS IS
 * THE EMPTY COMPOSER, and it is the state every chat page is in at the moment
 * the user first clicks it: a box with a border, a placeholder drawn by CSS,
 * and not one text node to put an offset into. A caret model that can only be
 * placed from a text run cannot be placed there at all.
 *
 * `<p><br></p>` -- the shape an editor leaves behind, and the shape a page's
 * own initialiser usually writes -- resolves to "before the <br>", so the first
 * character lands ahead of it and ce_drop_filler_br then takes the <br> away.
 * Aiming after it instead leaves a permanent blank first line. */
void fc_ce_caret_in(struct node *el, int at_end)
{
    if (!el || el->type != N_ELEM) return;
    struct node *t = at_end ? ce_prev_text(el, el) : ce_next_text(el, el);
    if (t) { fc_ce_set_caret(t, at_end ? t->textlen : 0); return; }
    struct node *d = el;
    while (d->last_child && d->last_child->type == N_ELEM &&
           !ce_skip(d->last_child) && !tag_is(d->last_child->tag, "br"))
        d = d->last_child;
    int nc = ce_nchild(d);
    if (nc == 1 && d->first_child->type == N_ELEM && tag_is(d->first_child->tag, "br"))
        nc = 0;
    fc_ce_set_caret(d, at_end ? nc : 0);
}

/* ---------------------------------------------------- range deletion ---- */

/* Everything between two positions, removed, with no events raised -- the
 * callers own the beforeinput/input pair because a selection replaced by typing
 * is ONE edit and must not report two.
 *
 * The shape is Range.deleteContents': truncate the two end text nodes, remove
 * every whole node between them, then merge the two paragraphs, because a
 * selection that spanned a paragraph break must not leave the break behind. */
static void ce_unlink(struct node *n)
{
    if (n && n->parent) dom_remove_child(n->parent, n);
}

/* Move every child of `src` to the end of `dst`, then drop `src`. */
static void ce_merge_into(struct node *dst, struct node *src)
{
    if (!dst || !src || dst == src) return;
    if (ce_is_ancestor(src, dst) || ce_is_ancestor(dst, src)) return;
    for (struct node *c = src->first_child, *nx; c; c = nx) {
        nx = c->next;
        dom_remove_child(src, c);
        dom_append_child(dst, c);
    }
    ce_unlink(src);
}

/* A <br> that only existed to give an empty paragraph height. An empty block
 * has no line box at all, so every editor puts one there and takes it away
 * again the moment the block gains content -- without that, a merge and the
 * first keystroke in a fresh paragraph each leave a blank line behind. */
static void ce_drop_filler_br(struct node *blk)
{
    if (!blk) return;
    struct node *last = blk->last_child;
    if (last && last->type == N_ELEM && tag_is(last->tag, "br") && last->prev)
        ce_unlink(last);
}

static void ce_fill_if_empty(struct node *blk)
{
    if (!blk || blk->first_child) return;
    struct node *br = dom_create_element(blk->doc, "br", 2);
    if (br) dom_append_child(blk, br);
}

static int ce_delete_raw(struct node *host, struct node *sn, int so,
                         struct node *en, int eo)
{
    if (!sn || !en) return 0;
    ce_norm(&sn, &so);
    ce_norm(&en, &eo);
    if (sn == en && so == eo) return 0;

    if (sn == en && sn->type == N_TEXT) {
        if (!ce_text_splice(sn, so, eo, "", 0)) return 0;
        fc_ce_set_caret(sn, so);
        return 1;
    }
    if (sn->type != N_TEXT || en->type != N_TEXT) {
        /* One end is an empty container. Nothing between them can be addressed
         * safely, so refuse rather than guess -- the caret stays where it is. */
        return 0;
    }

    struct node *bs = ce_block_of(sn, host);
    struct node *be = ce_block_of(en, host);

    /* the common ancestor */
    struct node *ca = sn;
    while (ca && !ce_is_ancestor(ca, en)) ca = ca->parent;
    if (!ca) return 0;

    /* Everything after sn, up to the common ancestor. */
    for (struct node *p = sn; p && p != ca; p = p->parent) {
        for (struct node *s = p->next, *nx; s; s = nx) {
            nx = s->next;
            if (s == en || ce_is_ancestor(s, en)) break;
            ce_unlink(s);
        }
    }
    /* Everything before en, up to the common ancestor. */
    for (struct node *p = en; p && p != ca; p = p->parent) {
        for (struct node *s = p->prev, *pv; s; s = pv) {
            pv = s->prev;
            if (s == sn || ce_is_ancestor(s, sn)) break;
            ce_unlink(s);
        }
    }
    ce_text_splice(en, 0, eo, "", 0);
    ce_text_splice(sn, so, sn->textlen, "", 0);

    if (bs && be && bs != be) {
        ce_drop_filler_br(bs);
        ce_merge_into(bs, be);
    }
    fc_ce_set_caret(sn, so);
    return 1;
}

/* ------------------------------------------------------------- insert --- */

static int ce_insert_raw(struct node *host, const char *s, int len)
{
    if (len < 0) len = slen(s);
    if (len <= 0) return 0;
    struct node *n = 0;
    int off = 0;
    if (!fc_ce_selection(&n, &off, 0, 0)) return 0;
    if (!fc_ce_collapsed()) {
        struct node *en; int eo;
        fc_ce_selection(&n, &off, &en, &eo);
        ce_delete_raw(host, n, off, en, eo);
        if (!fc_ce_selection(&n, &off, 0, 0)) return 0;
    }
    ce_norm(&n, &off);
    if (n->type == N_TEXT) {
        if (!ce_text_splice(n, off, off, s, len)) return 0;
        fc_ce_set_caret(n, off + len);
        ce_drop_filler_br(ce_block_of(n, host));
        return 1;
    }
    /* An empty container: the composer nobody has typed into yet. Give it a
     * text node -- this is the single most important line in the file, because
     * it is the state every chat page is in when the user starts typing. */
    if (n->type != N_ELEM) return 0;
    struct node *t = dom_create_text(n->doc, s, len);
    if (!t) return 0;
    struct node *ref = ce_child_at(n, off);
    if (ref) dom_insert_before(n, t, ref);
    else     dom_append_child(n, t);
    fc_ce_set_caret(t, len);
    ce_drop_filler_br(ce_block_of(t, host));
    return 1;
}

int fc_ce_insert(const char *s, int len)
{
    struct node *host = fc_ce_caret_host();
    if (!host) return 0;
    if (len < 0) len = slen(s);
    if (len <= 0) return 0;
    static char dbuf[512];
    int dl = len < (int)sizeof dbuf - 1 ? len : (int)sizeof dbuf - 1;
    for (int i = 0; i < dl; i++) dbuf[i] = s[i];
    dbuf[dl] = 0;
    if (!ce_fire(host, "beforeinput", "insertText", dbuf, 1)) return 0;
    if (!ce_insert_raw(host, s, len)) return 0;
    ce_fire(host, "input", "insertText", dbuf, 0);
    return 1;
}

/* ---------------------------------------------------------- deletion ---- */

/* A <br> strictly between two positions in the same block. Backspace over
 * "abc<br>|def" must remove the <br>, not the "c" -- getting this wrong deletes
 * a character the user can see on a line they did not touch. */
static struct node *ce_br_before(struct node *host, struct node *at)
{
    struct node *n = at;
    for (;;) {
        if (!n || n == host) return 0;
        if (n->prev) {
            n = n->prev;
            while (n->type == N_ELEM && n->last_child && !ce_skip(n)) n = n->last_child;
        } else {
            n = n->parent;
            if (!n || n == host) return 0;
            continue;
        }
        if (n->type == N_TEXT) { if (n->textlen > 0) return 0; continue; }
        if (n->type == N_ELEM && tag_is(n->tag, "br")) return n;
        if (n->type == N_ELEM && !n->first_child) continue;   /* an empty inline */
        return 0;
    }
}

static int ce_backspace_raw(struct node *host)
{
    struct node *sn, *en;
    int so, eo;
    if (!fc_ce_selection(&sn, &so, &en, &eo)) return 0;
    if (!fc_ce_collapsed()) return ce_delete_raw(host, sn, so, en, eo);

    struct node *n = sn;
    int off = so;
    ce_norm(&n, &off);
    if (n->type == N_TEXT && off > 0) {
        int a = step_left(n->text, off);
        if (!ce_text_splice(n, a, off, "", 0)) return 0;
        fc_ce_set_caret(n, a);
        return 1;
    }
    /* At the start of this node. A <br> immediately before is what goes. */
    struct node *br = ce_br_before(host, n->type == N_TEXT ? n : n);
    if (br) {
        struct node *blk = ce_block_of(n, host);
        ce_unlink(br);
        fc_ce_set_caret(n, 0);
        (void)blk;
        return 1;
    }
    struct node *p = ce_prev_text(host, n);
    if (!p) {
        /* Nothing before us in the host, but our paragraph may still be a
         * later one that should merge upward (an empty paragraph). */
        struct node *blk = ce_block_of(n, host);
        if (blk && blk->prev) {
            struct node *prevblk = blk->prev;
            while (prevblk && prevblk->type != N_ELEM) prevblk = prevblk->prev;
            if (prevblk && ce_is_block(prevblk)) {
                ce_drop_filler_br(prevblk);
                struct node *last = ce_prev_text(host, blk);
                ce_merge_into(prevblk, blk);
                if (last) fc_ce_set_caret(last, last->textlen);
                else      fc_ce_set_caret(prevblk, 0);
                return 1;
            }
        }
        return 0;
    }
    struct node *bp = ce_block_of(p, host);
    struct node *bn = ce_block_of(n, host);
    if (bp == bn) {
        if (p->textlen == 0) { ce_unlink(p); fc_ce_set_caret(n, 0); return 1; }
        int a = step_left(p->text, p->textlen);
        int had = p->textlen;
        if (!ce_text_splice(p, a, had, "", 0)) return 0;
        fc_ce_set_caret(p, a);
        return 1;
    }
    /* Different paragraphs: the paragraph break itself is what backspace
     * deletes, and the two paragraphs join. */
    if (bn) {
        ce_drop_filler_br(bp);
        int at = p->textlen;
        if (bp) ce_merge_into(bp, bn);
        else    { /* the previous text is in the host's inline content */
                  for (struct node *c = bn->first_child, *nx; c; c = nx) {
                      nx = c->next; dom_remove_child(bn, c);
                      dom_insert_before(host, c, bn);
                  }
                  ce_unlink(bn); }
        fc_ce_set_caret(p, at);
        return 1;
    }
    return 0;
}

static int ce_delete_fwd_raw(struct node *host)
{
    struct node *sn, *en;
    int so, eo;
    if (!fc_ce_selection(&sn, &so, &en, &eo)) return 0;
    if (!fc_ce_collapsed()) return ce_delete_raw(host, sn, so, en, eo);

    struct node *n = en;
    int off = eo;
    ce_norm(&n, &off);
    if (n->type == N_TEXT && off < n->textlen) {
        int b = step_right(n->text, n->textlen, off);
        if (!ce_text_splice(n, off, b, "", 0)) return 0;
        fc_ce_set_caret(n, off);
        return 1;
    }
    struct node *nx = ce_next_text(host, n);
    if (!nx) return 0;
    struct node *bn = ce_block_of(n, host);
    struct node *bx = ce_block_of(nx, host);
    if (bn == bx) {
        if (nx->textlen == 0) { ce_unlink(nx); return 1; }
        int b = step_right(nx->text, nx->textlen, 0);
        if (!ce_text_splice(nx, 0, b, "", 0)) return 0;
        fc_ce_set_caret(n, off);
        return 1;
    }
    if (bn && bx) {
        ce_drop_filler_br(bn);
        ce_merge_into(bn, bx);
        fc_ce_set_caret(n, off);
        return 1;
    }
    return 0;
}

int fc_ce_backspace(void)
{
    struct node *host = fc_ce_caret_host();
    if (!host) return 0;
    if (!ce_fire(host, "beforeinput", "deleteContentBackward", 0, 1)) return 0;
    if (!ce_backspace_raw(host)) return 0;
    ce_fire(host, "input", "deleteContentBackward", 0, 0);
    return 1;
}

int fc_ce_delete(void)
{
    struct node *host = fc_ce_caret_host();
    if (!host) return 0;
    if (!ce_fire(host, "beforeinput", "deleteContentForward", 0, 1)) return 0;
    if (!ce_delete_fwd_raw(host)) return 0;
    ce_fire(host, "input", "deleteContentForward", 0, 0);
    return 1;
}

/* ------------------------------------------------------------- Enter ---- */

/* WHICH OF THE TWO BEHAVIOURS THIS IS. The spec permits either; browsers
 * differ, and pages are written against both. Plain Enter here SPLITS THE BLOCK
 * -- the caret's paragraph becomes two, the second one a fresh element of the
 * same tag -- which is Chrome's and Safari's default (`defaultParagraphSeparator
 * = "div"`), and it is the one that leaves the DOM in a shape a page's own
 * serialiser can read back. Shift+Enter inserts a <br>, which is the other half
 * of the same convention.
 *
 * NOTE, because it is the case that actually matters here: a chat composer
 * almost always cancels Enter in its own keydown handler and sends the message
 * instead, so on such a page this function never runs. browser.c honours the
 * cancellation (the page's keydown has already had its turn before the control
 * sees the key), which is why that is a feature rather than a race. */

/* Split the ancestor chain from `first`'s parent up to `stop`, so that `first`
 * and everything after it ends up inside `into`, with the intervening inline
 * elements re-created around it. Without this, splitting a line inside a
 * <b> would move the bold text out of it and the second half would lose its
 * formatting. */
static void ce_split_chain(struct node *stop, struct node *into, struct node *first)
{
    struct node *cur_first = first;
    struct node *p = first ? first->parent : 0;
    while (p && p != stop) {
        struct node *shell = dom_create_element(p->doc, p->tag, -1);
        if (!shell) return;
        for (struct node *s = cur_first, *nx; s; s = nx) {
            nx = s->next;
            dom_remove_child(p, s);
            dom_append_child(shell, s);
        }
        dom_insert_before(p->parent, shell, p->next);
        cur_first = shell;
        p = p->parent;
    }
    if (!p) return;
    for (struct node *s = cur_first, *nx; s; s = nx) {
        nx = s->next;
        /* `into` is a SIBLING of what we are moving when the split is at the
         * host's own level -- it was appended to the host to get it into the
         * tree. Walking into it would detach it from the document and take the
         * new paragraph with it, which looks like "Enter deleted half the
         * line". */
        if (s == into) break;
        dom_remove_child(p, s);
        dom_append_child(into, s);
    }
}

/* The first node of the right-hand side of a split at (n,off), creating the
 * tail text node when the split falls inside a run. NULL means "nothing follows
 * the caret", i.e. the new paragraph starts empty. */
static struct node *ce_split_point(struct node *n, int off)
{
    if (n->type == N_TEXT) {
        if (off >= n->textlen) return n->next;
        struct node *tail = dom_create_text(n->doc, n->text + off, n->textlen - off);
        if (!tail) return 0;
        ce_text_splice(n, off, n->textlen, "", 0);
        if (n->next) dom_insert_before(n->parent, tail, n->next);
        else         dom_append_child(n->parent, tail);
        return tail;
    }
    return ce_child_at(n, off);
}

static int ce_enter_raw(struct node *host, int shift)
{
    struct node *sn, *en;
    int so, eo;
    if (!fc_ce_selection(&sn, &so, &en, &eo)) return 0;
    if (!fc_ce_collapsed()) {
        ce_delete_raw(host, sn, so, en, eo);
        if (!fc_ce_selection(&sn, &so, 0, 0)) return 0;
    }
    struct node *n = sn;
    int off = so;
    ce_norm(&n, &off);

    if (shift) {
        struct node *br = dom_create_element(host->doc, "br", 2);
        if (!br) return 0;
        struct node *right = ce_split_point(n, off);
        struct node *parent = (n->type == N_TEXT) ? n->parent : n;
        if (right && right->parent == parent) dom_insert_before(parent, br, right);
        else                                  dom_append_child(parent, br);
        /* The caret needs a text node to live in, and the tail may not exist
         * (Shift+Enter at the very end of a line). An empty one costs nothing
         * and lays out as nothing. */
        if (right && right->type == N_TEXT) fc_ce_set_caret(right, 0);
        else {
            struct node *t = dom_create_text(host->doc, "", 0);
            if (t) {
                if (br->next) dom_insert_before(parent, t, br->next);
                else          dom_append_child(parent, t);
                fc_ce_set_caret(t, 0);
            }
        }
        return 1;
    }

    struct node *blk = ce_block_of(n, host);
    struct node *right = ce_split_point(n, off);
    if (blk) {
        struct node *nb = dom_create_element(host->doc, blk->tag, -1);
        if (!nb) return 0;
        if (blk->next) dom_insert_before(blk->parent, nb, blk->next);
        else           dom_append_child(blk->parent, nb);
        ce_split_chain(blk, nb, right);
        ce_fill_if_empty(blk);
        ce_fill_if_empty(nb);
        struct node *t = ce_next_text(nb, nb);
        if (t) fc_ce_set_caret(t, 0);
        else   fc_ce_set_caret(nb, 0);
        return 1;
    }
    /* No paragraph yet: the host's own inline content becomes two of them,
     * which is what a real browser does on the first Enter in a bare
     * <div contenteditable>. */
    struct node *d2 = dom_create_element(host->doc, "div", 3);
    if (!d2) return 0;
    dom_append_child(host, d2);
    ce_split_chain(host, d2, right);
    struct node *d1 = dom_create_element(host->doc, "div", 3);
    if (d1) {
        dom_insert_before(host, d1, host->first_child);
        for (struct node *c = d1->next, *nx; c && c != d2; c = nx) {
            nx = c->next;
            dom_remove_child(host, c);
            dom_append_child(d1, c);
        }
        ce_fill_if_empty(d1);
    }
    ce_fill_if_empty(d2);
    struct node *t = ce_next_text(d2, d2);
    if (t) fc_ce_set_caret(t, 0);
    else   fc_ce_set_caret(d2, 0);
    return 1;
}

int fc_ce_enter(int shift)
{
    struct node *host = fc_ce_caret_host();
    if (!host) return 0;
    const char *it = shift ? "insertLineBreak" : "insertParagraph";
    if (!ce_fire(host, "beforeinput", it, 0, 1)) return 0;
    if (!ce_enter_raw(host, shift)) return 0;
    ce_fire(host, "input", it, 0, 0);
    return 1;
}

/* --------------------------------------------------------- movement ----- */

static void ce_apply_move(struct node *n, int off, int extend)
{
    if (extend) pos_set(&g_ce_f, n, off);
    else        fc_ce_set_caret(n, off);
}

static int ce_wordch_at(const struct node *t, int i)
{ return i >= 0 && i < t->textlen && is_wordch((unsigned char)t->text[i]); }

int fc_ce_move(int dir, int word, int extend)
{
    struct node *host = fc_ce_caret_host();
    if (!host) return 0;
    struct node *n = g_ce_f.n;
    int off = g_ce_f.off;

    /* A plain arrow over a selection collapses to that end. Same rule as the
     * text field above; the caret model gets it wrong by default. */
    if (!extend && !fc_ce_collapsed()) {
        struct node *sn, *en; int so, eo;
        fc_ce_selection(&sn, &so, &en, &eo);
        if (dir < 0) fc_ce_set_caret(sn, so);
        else         fc_ce_set_caret(en, eo);
        return 1;
    }
    ce_norm(&n, &off);
    if (!n) return 0;

    for (int step = 0; ; ) {
        if (n->type == N_TEXT) {
            if (dir < 0 && off > 0)            { off = step_left(n->text, off); }
            else if (dir > 0 && off < n->textlen) { off = step_right(n->text, n->textlen, off); }
            else {
                struct node *o = dir < 0 ? ce_prev_text(host, n) : ce_next_text(host, n);
                if (!o) { if (step == 0) return 0; break; }
                /* WHETHER CROSSING IS ITSELF A STEP DEPENDS ON WHAT IS CROSSED,
                 * and getting this wrong makes the arrow keys asymmetric -- one
                 * press right and two presses left to get back, which reads as
                 * a stuck caret.
                 *
                 * Crossing a run boundary INSIDE a paragraph ("a<b>B</b>c"):
                 * the end of one run and the start of the next are the SAME
                 * PLACE on screen, so landing there is not a move and the caret
                 * must consume a character as well.
                 *
                 * Crossing a PARAGRAPH boundary: the end of one line and the
                 * start of the next are two different places, so the crossing
                 * IS the move and no character is consumed. */
                int same = ce_block_of(o, host) == ce_block_of(n, host);
                n = o;
                if (dir < 0) {
                    off = o->textlen;
                    if (same && off > 0) off = step_left(o->text, off);
                } else {
                    off = 0;
                    if (same && off < o->textlen) off = step_right(o->text, o->textlen, off);
                }
            }
        } else {
            struct node *o = dir < 0 ? ce_prev_text(host, n) : ce_next_text(host, n);
            if (!o) return 0;
            n = o;
            off = dir < 0 ? o->textlen : 0;
        }
        step++;
        if (!word) break;
        /* Word motion: keep going while the character we would cross next is
         * still inside the same word class. */
        int probe = dir < 0 ? off - 1 : off;
        if (dir < 0) { if (off == 0 || !ce_wordch_at(n, probe)) break; }
        else         { if (off >= n->textlen || !ce_wordch_at(n, probe)) break; }
        if (step > 4096) break;
    }
    ce_apply_move(n, off, extend);
    return 1;
}

/* Home/End are PARAGRAPH-scoped, not visual-line-scoped, and that is a stated
 * limitation rather than an oversight: a visual line is a layout fact and this
 * file deliberately holds no layout dependency (see the note in forms.h about
 * why the geometry lives in the caller). On a wrapped paragraph Home therefore
 * goes to the start of the paragraph, not the start of the screen line. */
int fc_ce_home(int extend)
{
    struct node *host = fc_ce_caret_host();
    if (!host) return 0;
    struct node *n = g_ce_f.n; int off = g_ce_f.off;
    ce_norm(&n, &off);
    struct node *blk = ce_block_of(n, host);
    struct node *scope = blk ? blk : host;
    struct node *t = ce_next_text(scope, scope);
    if (t) ce_apply_move(t, 0, extend);
    else   ce_apply_move(scope, 0, extend);
    return 1;
}

int fc_ce_end(int extend)
{
    struct node *host = fc_ce_caret_host();
    if (!host) return 0;
    struct node *n = g_ce_f.n; int off = g_ce_f.off;
    ce_norm(&n, &off);
    struct node *blk = ce_block_of(n, host);
    struct node *scope = blk ? blk : host;
    struct node *t = ce_prev_text(scope, scope);
    if (t) ce_apply_move(t, t->textlen, extend);
    else   ce_apply_move(scope, ce_nchild(scope), extend);
    return 1;
}

int fc_ce_select_all(struct node *host)
{
    if (!host) host = fc_ce_caret_host();
    if (!host) return 0;
    struct node *f = ce_next_text(host, host);
    struct node *l = ce_prev_text(host, host);
    if (!f || !l) { fc_ce_set_caret(host, 0); return 1; }
    fc_ce_set_range(f, 0, l, l->textlen);
    return 1;
}

int fc_ce_selection_text(char *buf, int max)
{
    if (!buf || max <= 0) return 0;
    buf[0] = 0;
    struct node *sn, *en;
    int so, eo;
    if (!fc_ce_selection(&sn, &so, &en, &eo)) return 0;
    ce_norm(&sn, &so);
    ce_norm(&en, &eo);
    struct node *host = fc_ce_host(sn);
    if (!host) return 0;
    int o = 0;
    if (sn == en) {
        if (sn->type != N_TEXT) return 0;
        for (int i = so; i < eo && i < sn->textlen && o < max - 1; i++) buf[o++] = sn->text[i];
        buf[o] = 0;
        return o;
    }
    if (sn->type == N_TEXT)
        for (int i = so; i < sn->textlen && o < max - 1; i++) buf[o++] = sn->text[i];
    for (struct node *t = ce_next_text(host, sn); t && t != en; t = ce_next_text(host, t))
        for (int i = 0; i < t->textlen && o < max - 1; i++) buf[o++] = t->text[i];
    if (en->type == N_TEXT)
        for (int i = 0; i < eo && i < en->textlen && o < max - 1; i++) buf[o++] = en->text[i];
    buf[o] = 0;
    return o;
}

int fc_ce_run_range(struct node *t, int *a, int *b)
{
    if (!t || t->type != N_TEXT) return 0;
    struct node *sn, *en;
    int so, eo;
    if (!fc_ce_selection(&sn, &so, &en, &eo)) return 0;
    ce_norm(&sn, &so);
    ce_norm(&en, &eo);
    if (sn == en && so == eo) return 0;
    int x0 = 0, x1 = t->textlen;
    if (t == sn) x0 = so;
    else if (ce_cmp(t, 0, sn, so) < 0) return 0;               /* entirely before */
    if (t == en) x1 = eo;
    else if (ce_cmp(t, t->textlen, en, eo) > 0) return 0;      /* entirely after */
    if (x1 <= x0) return 0;
    if (a) *a = x0;
    if (b) *b = x1;
    return 1;
}

/* ------------------------------------------------------------- paths ---- */

static struct node *ce_doc_elem(struct node *n)
{
    while (n && n->parent) n = n->parent;
    if (!n) return 0;
    if (n->type != N_DOCUMENT) return n;
    for (struct node *c = n->first_child; c; c = c->next)
        if (c->type == N_ELEM) return c;
    return 0;
}

int fc_ce_path(int which, int *out, int max, int *off_out)
{
    const struct cepos *p = which ? &g_ce_f : &g_ce_a;
    if (!pos_live(p)) return -1;
    struct node *root = ce_doc_elem(p->n);
    if (!root) return -1;
    int tmp[64], d = 0;
    for (struct node *n = p->n; n && n != root; n = n->parent) {
        if (d >= 64) return -1;
        tmp[d++] = ce_index(n);
    }
    if (d > max) return -1;
    for (int i = 0; i < d; i++) out[i] = tmp[d - 1 - i];
    if (off_out) *off_out = p->off;
    return d;
}

static struct node *ce_walk_path(struct node *root, const int *path, int depth)
{
    struct node *n = root;
    for (int i = 0; i < depth && n; i++) n = ce_child_at(n, path[i]);
    return n;
}

/* The document element, found without a caret to start from. The caret is the
 * usual anchor and is absent exactly when the page is placing one for the first
 * time -- which is the call that matters. */
static struct node *g_ce_root_hint;
void fc_ce_set_root(struct node *root);
void fc_ce_set_root(struct node *root) { g_ce_root_hint = root ? ce_doc_elem(root) : 0; }

static struct node *ce_root(void)
{
    if (pos_live(&g_ce_f)) return ce_doc_elem(g_ce_f.n);
    if (pos_live(&g_ce_a)) return ce_doc_elem(g_ce_a.n);
    if (g_ce_root_hint && ce_attached(g_ce_root_hint)) return g_ce_root_hint;
    return 0;
}

int fc_ce_set_path(int which, const int *path, int depth, int off)
{
    struct node *root = ce_root();
    if (!root) return 0;
    struct node *n = ce_walk_path(root, path, depth);
    if (!n) return 0;
    pos_set(which ? &g_ce_f : &g_ce_a, n, off);
    return 1;
}

int fc_ce_set_paths(const int *apath, int adepth, int aoff,
                    const int *fpath, int fdepth, int foff)
{
    struct node *root = ce_root();
    if (!root) return 0;
    struct node *a = ce_walk_path(root, apath, adepth);
    struct node *f = ce_walk_path(root, fpath, fdepth);
    if (!a || !f) return 0;
    pos_set(&g_ce_a, a, aoff);
    pos_set(&g_ce_f, f, foff);
    return 1;
}
