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

void fc_reset(void)
{
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
static int splice(struct node *n, int a, int b, const char *s, int len)
{
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
     * a page that returns false from it expects the value NOT to change. */
    if (!fc_dispatch(n, "beforeinput", 1, 1)) return 0;

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
    fc_dispatch(n, "input", 1, 0);
    return 1;
}

int fc_edit_insert(struct node *n, const char *s, int len)
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
        return splice(n, c->sel0, c->sel1, buf, o);
    }
    return splice(n, c->sel0, c->sel1, s, len);
}

int fc_edit_backspace(struct node *n)
{
    if (!editable(n)) return 0;
    struct fctl *c = ent(n, 1);
    if (!c) return 0;
    if (c->sel0 != c->sel1) return splice(n, c->sel0, c->sel1, "", 0);
    if (c->sel0 == 0) return 0;
    int a = step_left(c->val ? c->val : "", c->sel0);
    return splice(n, a, c->sel0, "", 0);
}

int fc_edit_delete(struct node *n)
{
    if (!editable(n)) return 0;
    struct fctl *c = ent(n, 1);
    if (!c) return 0;
    int vl = 0;
    fc_value(n, &vl);
    if (c->sel0 != c->sel1) return splice(n, c->sel0, c->sel1, "", 0);
    if (c->sel1 >= vl) return 0;
    int b = step_right(c->val ? c->val : "", vl, c->sel1);
    return splice(n, c->sel1, b, "", 0);
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
