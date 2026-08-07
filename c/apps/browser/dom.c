/* S1b: the DOM core -- arena-allocated nodes, interned names, no hard caps.
 *
 * This file is now purely the data model and its API: the arena, the interned
 * name ledger, the id index, node creation/mutation and attribute access. The
 * tolerant single-pass scanner that used to sit at the bottom is gone --
 * html_tree.c (over html_tokenizer.c) is the WHATWG tree construction
 * algorithm and dom_parse() is a one-line call into it. See dom.h for the
 * arena and interning rationale.
 */
#include "dom.h"
#include "html_tree.h"

#include <libwapcaplet/libwapcaplet.h>

void *kmalloc(unsigned long);
void  kfree(void *);
void *memcpy(void *, const void *, unsigned long);
void *memset(void *, int, unsigned long);

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */
static int nmatch(const char *a, const char *b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int sp(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }
static size_t zlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* pointer hash (splitmix-style finaliser): the id index and the interned-string
 * ledger are keyed by a pointer (an lwc_string or a node), never by bytes. */
static uint32_t ptr_hash(const void *p)
{
    uint64_t v = (uint64_t)(uintptr_t)p;
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return (uint32_t)v;
}

/* css_engine registers this so dom.c never has to know what a
 * css_computed_style is (and so the host tests that do not link LibCSS still
 * link dom.c on its own). */
static void (*g_computed_free)(void *);
void dom_set_computed_free(void (*fn)(void *)) { g_computed_free = fn; }

/* ------------------------------------------------------------------ */
/* interned atoms (process-global, interned once, never released)      */
/* ------------------------------------------------------------------ */
struct dom_atoms dom_atoms;
static int g_atoms_ready;

/* Attribute names the hot paths ask for by string literal. dom_attr() screens
 * a caller's name against these by length + first character, so the common
 * dom_attr(n,"class") never touches the global intern table at all. */
static const char *const ATTR_ATOM_TEXT[] = {
    "class", "id", "style", "href", "src", "rel", "width", "height",
    "type", "name", "data-src", "alt", "title"
};
#define NATTR_ATOM ((int)(sizeof ATTR_ATOM_TEXT / sizeof ATTR_ATOM_TEXT[0]))
static lwc_string *g_attr_atom[NATTR_ATOM];
static unsigned char g_attr_atom_len[NATTR_ATOM];

/* Well-known element names -> TAG_*. Interned once and looked up by pointer, so
 * setting node->tag_id costs one hash probe per element. */
static const struct { const char *n; unsigned short id; } TAG_TEXT[] = {
    {"html",TAG_HTML},{"head",TAG_HEAD},{"body",TAG_BODY},{"title",TAG_TITLE},
    {"meta",TAG_META},{"link",TAG_LINK},{"base",TAG_BASE},{"style",TAG_STYLE},
    {"script",TAG_SCRIPT},{"noscript",TAG_NOSCRIPT},{"template",TAG_TEMPLATE},
    {"div",TAG_DIV},{"span",TAG_SPAN},{"p",TAG_P},{"a",TAG_A},{"img",TAG_IMG},
    {"br",TAG_BR},{"hr",TAG_HR},{"wbr",TAG_WBR},
    {"h1",TAG_H1},{"h2",TAG_H2},{"h3",TAG_H3},{"h4",TAG_H4},{"h5",TAG_H5},{"h6",TAG_H6},
    {"ul",TAG_UL},{"ol",TAG_OL},{"li",TAG_LI},{"dl",TAG_DL},{"dt",TAG_DT},{"dd",TAG_DD},
    {"table",TAG_TABLE},{"thead",TAG_THEAD},{"tbody",TAG_TBODY},{"tfoot",TAG_TFOOT},
    {"tr",TAG_TR},{"td",TAG_TD},{"th",TAG_TH},{"caption",TAG_CAPTION},
    {"col",TAG_COL},{"colgroup",TAG_COLGROUP},
    {"form",TAG_FORM},{"input",TAG_INPUT},{"button",TAG_BUTTON},{"select",TAG_SELECT},
    {"option",TAG_OPTION},{"textarea",TAG_TEXTAREA},{"label",TAG_LABEL},
    {"fieldset",TAG_FIELDSET},{"legend",TAG_LEGEND},
    {"b",TAG_B},{"i",TAG_I},{"em",TAG_EM},{"strong",TAG_STRONG},{"code",TAG_CODE},
    {"pre",TAG_PRE},{"small",TAG_SMALL},{"sub",TAG_SUB},{"sup",TAG_SUP},
    {"u",TAG_U},{"s",TAG_S},{"q",TAG_Q},{"blockquote",TAG_BLOCKQUOTE},
    {"header",TAG_HEADER},{"footer",TAG_FOOTER},{"section",TAG_SECTION},
    {"article",TAG_ARTICLE},{"nav",TAG_NAV},{"main",TAG_MAIN},{"aside",TAG_ASIDE},
    {"figure",TAG_FIGURE},{"figcaption",TAG_FIGCAPTION},
    {"svg",TAG_SVG},{"math",TAG_MATH},{"canvas",TAG_CANVAS},{"video",TAG_VIDEO},
    {"audio",TAG_AUDIO},{"source",TAG_SOURCE},{"iframe",TAG_IFRAME},
    {"embed",TAG_EMBED},{"object",TAG_OBJECT},{"param",TAG_PARAM},
    {"track",TAG_TRACK},{"area",TAG_AREA},{"picture",TAG_PICTURE},
    {"frame",TAG_FRAME},{"frameset",TAG_FRAMESET},{"plaintext",TAG_PLAINTEXT},
    {"xmp",TAG_XMP},{"image",TAG_IMAGE},
};
#define NTAG_TEXT ((int)(sizeof TAG_TEXT / sizeof TAG_TEXT[0]))

/* open-addressed lwc_string* -> tag id map (power of two, load < 50%) */
#define TAGMAP_CAP 256
static lwc_string   *g_tagmap_key[TAGMAP_CAP];
static unsigned short g_tagmap_val[TAGMAP_CAP];

void dom_atoms_init(void)
{
    if (g_atoms_ready) return;
    g_atoms_ready = 1;                      /* set first: intern failures must
                                             * not turn this into a retry loop */
    for (int i = 0; i < NATTR_ATOM; i++) {
        size_t l = zlen(ATTR_ATOM_TEXT[i]);
        g_attr_atom_len[i] = (unsigned char)l;
        if (lwc_intern_string(ATTR_ATOM_TEXT[i], l, &g_attr_atom[i]) != lwc_error_ok)
            g_attr_atom[i] = NULL;
    }
    dom_atoms.a_class = g_attr_atom[0];  dom_atoms.a_id       = g_attr_atom[1];
    dom_atoms.a_style = g_attr_atom[2];  dom_atoms.a_href     = g_attr_atom[3];
    dom_atoms.a_src   = g_attr_atom[4];  dom_atoms.a_rel      = g_attr_atom[5];
    dom_atoms.a_width = g_attr_atom[6];  dom_atoms.a_height   = g_attr_atom[7];
    dom_atoms.a_type  = g_attr_atom[8];  dom_atoms.a_name     = g_attr_atom[9];
    dom_atoms.a_data_src = g_attr_atom[10];
    dom_atoms.a_alt   = g_attr_atom[11]; dom_atoms.a_title    = g_attr_atom[12];

    for (int i = 0; i < NTAG_TEXT; i++) {
        lwc_string *s = NULL;
        if (lwc_intern_string(TAG_TEXT[i].n, zlen(TAG_TEXT[i].n), &s) != lwc_error_ok || !s)
            continue;
        uint32_t k = ptr_hash(s) & (TAGMAP_CAP - 1);
        while (g_tagmap_key[k]) k = (k + 1) & (TAGMAP_CAP - 1);
        g_tagmap_key[k] = s;                /* the atom table keeps this ref */
        g_tagmap_val[k] = TAG_TEXT[i].id;
    }
}

static uint16_t tag_id_of(lwc_string *name)
{
    if (!name) return TAG_UNKNOWN;
    uint32_t k = ptr_hash(name) & (TAGMAP_CAP - 1);
    while (g_tagmap_key[k]) {
        if (g_tagmap_key[k] == name) return g_tagmap_val[k];
        k = (k + 1) & (TAGMAP_CAP - 1);
    }
    return TAG_UNKNOWN;
}

/* `s` is already lowercased and `len` its length: find the matching attribute
 * atom, screening on length then first byte before the full compare. */
static lwc_string *attr_atom(const char *s, size_t len)
{
    for (int i = 0; i < NATTR_ATOM; i++) {
        if (g_attr_atom_len[i] != len) continue;
        if (ATTR_ATOM_TEXT[i][0] != s[0]) continue;
        if (nmatch(ATTR_ATOM_TEXT[i], s, (int)len)) return g_attr_atom[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* the document arena                                                  */
/* ------------------------------------------------------------------ */
#define DOM_CHUNK_BYTES (256u * 1024u)

struct dom_chunk {                  /* bump arena for bytes (strings, arrays) */
    struct dom_chunk *next;
    size_t used, cap;
};
struct node_chunk {                 /* dense array of node slots */
    struct node_chunk *next;
    uint32_t used, cap;
};

struct dom_doc {
    struct node_chunk *nchunks;     /* newest first; every slot ever handed out */
    struct dom_chunk  *schunks;     /* newest first; head is the bump target */
    struct node *freelist;          /* recycled slots, linked through ->next */
    struct node *root;
    struct node *wrapped;           /* chain (wrap_next) of nodes that ever had
                                     * a JS wrapper */
    uint32_t next_serial;
    size_t   bytes;                 /* allocator bytes this doc holds */

    lwc_string **iset;              /* ledger: one ref per DISTINCT lwc string */
    uint32_t icap, icount;

    struct node **idb;              /* id index buckets (id_next chains) */
    uint32_t idcap, idcount;

    int quirks;
    int oom;                        /* an allocation failed; tree is truncated */
};

static struct node *nc_nodes(struct node_chunk *c) { return (struct node *)(void *)(c + 1); }

static void *arena_alloc(struct dom_doc *d, size_t n)
{
    n = (n + 7u) & ~(size_t)7u;
    if (!n) n = 8;
    struct dom_chunk *head = d->schunks;
    if (head && head->cap - head->used >= n) {
        void *p = (unsigned char *)(head + 1) + head->used;
        head->used += n;
        return p;
    }
    size_t payload = DOM_CHUNK_BYTES - sizeof(struct dom_chunk);
    int oversize = 0;
    if (n > payload) { payload = n; oversize = 1; }
    struct dom_chunk *c = kmalloc((unsigned long)(sizeof *c + payload));
    if (!c) { d->oom = 1; return 0; }
    c->cap = payload; c->used = n;
    d->bytes += sizeof *c + payload;
    /* A single huge allocation (an 8 KiB attribute value, a 2 MiB <svg> span)
     * gets its own chunk, spliced in *behind* the head so the head's remaining
     * space stays available for the small allocations that follow. */
    if (oversize && head) { c->next = head->next; head->next = c; }
    else                  { c->next = head; d->schunks = c; }
    return (unsigned char *)(c + 1);
}

static char *arena_dup(struct dom_doc *d, const char *s, size_t len)
{
    char *p = arena_alloc(d, len + 1);
    if (!p) return 0;
    if (len) memcpy(p, s, (unsigned long)len);
    p[len] = 0;
    return p;
}

/* ------------------------------------------------------------------ */
/* interned-string ledger                                              */
/* ------------------------------------------------------------------ */
static int iset_place(lwc_string **tab, uint32_t cap, lwc_string *s)
{
    uint32_t m = cap - 1, i = ptr_hash(s) & m;
    while (tab[i]) {
        if (tab[i] == s) return 0;
        i = (i + 1) & m;
    }
    tab[i] = s;
    return 1;
}

/* 1 = the doc now owns a ref it did not before, 0 = already held, -1 = OOM. */
static int iset_add(struct dom_doc *d, lwc_string *s)
{
    if (d->icount * 4 >= d->icap * 3) {
        uint32_t ncap = d->icap ? d->icap * 2 : 256;
        lwc_string **nt = kmalloc((unsigned long)ncap * sizeof *nt);
        if (!nt) { d->oom = 1; return -1; }
        memset(nt, 0, (unsigned long)ncap * sizeof *nt);
        for (uint32_t i = 0; i < d->icap; i++)
            if (d->iset[i]) iset_place(nt, ncap, d->iset[i]);
        if (d->iset) { d->bytes -= (size_t)d->icap * sizeof *nt; kfree(d->iset); }
        d->iset = nt; d->icap = ncap;
        d->bytes += (size_t)ncap * sizeof *nt;
    }
    if (iset_place(d->iset, d->icap, s)) { d->icount++; return 1; }
    return 0;
}

/* Intern `s` and make sure the document holds exactly one reference to it.
 * Recording per *distinct* string rather than per use keeps the ledger tiny
 * (a page has thousands of distinct names, not hundreds of thousands). */
static lwc_string *doc_intern(struct dom_doc *d, const char *s, size_t len)
{
    lwc_string *r = 0;
    if (!len) return 0;
    if (lwc_intern_string(s, len, &r) != lwc_error_ok || !r) return 0;
    int add = iset_add(d, r);
    if (add == 0) lwc_string_unref(r);      /* already held: drop the extra ref */
    /* add < 0: the ledger could not grow. Keeping the ref leaks it at document
     * teardown, which is strictly better than unref'ing a string the DOM still
     * points at. */
    return r;
}

static lwc_string *doc_hold(struct dom_doc *d, lwc_string *s)
{
    if (!s) return 0;
    if (iset_add(d, s) == 1) lwc_string_ref(s);
    return s;
}

/* ------------------------------------------------------------------ */
/* id index                                                            */
/* ------------------------------------------------------------------ */
static void id_unindex(struct dom_doc *d, struct node *n)
{
    if (!(n->flags & NF_ID_INDEXED)) return;
    n->flags = (uint16_t)(n->flags & ~(unsigned)NF_ID_INDEXED);
    if (!d->idb || !n->id) { n->id_next = 0; return; }
    uint32_t i = ptr_hash(n->id) & (d->idcap - 1);
    struct node **pp = &d->idb[i];
    while (*pp && *pp != n) pp = &(*pp)->id_next;
    if (*pp) { *pp = n->id_next; d->idcount--; }
    n->id_next = 0;
}

static void id_index(struct dom_doc *d, struct node *n)
{
    if (!n->id || (n->flags & NF_ID_INDEXED)) return;
    if (d->idcount * 4 >= d->idcap * 3) {
        uint32_t ncap = d->idcap ? d->idcap * 2 : 128;
        struct node **nt = kmalloc((unsigned long)ncap * sizeof *nt);
        if (!nt) { d->oom = 1; return; }
        memset(nt, 0, (unsigned long)ncap * sizeof *nt);
        for (uint32_t i = 0; i < d->idcap; i++) {
            struct node *e = d->idb[i];
            while (e) {
                struct node *nx = e->id_next;
                uint32_t k = ptr_hash(e->id) & (ncap - 1);
                e->id_next = nt[k]; nt[k] = e;
                e = nx;
            }
        }
        if (d->idb) { d->bytes -= (size_t)d->idcap * sizeof *nt; kfree(d->idb); }
        d->idb = nt; d->idcap = ncap;
        d->bytes += (size_t)ncap * sizeof *nt;
    }
    uint32_t k = ptr_hash(n->id) & (d->idcap - 1);
    n->id_next = d->idb[k];
    d->idb[k] = n;
    d->idcount++;
    n->flags |= NF_ID_INDEXED;
}

/* ------------------------------------------------------------------ */
/* node slots                                                          */
/* ------------------------------------------------------------------ */
static struct node *node_alloc(struct dom_doc *d)
{
    struct node *n = d->freelist;
    if (n) {
        d->freelist = n->next;
    } else {
        struct node_chunk *c = d->nchunks;
        if (!c || c->used >= c->cap) {
            size_t payload = DOM_CHUNK_BYTES - sizeof(struct node_chunk);
            uint32_t cap = (uint32_t)(payload / sizeof(struct node));
            if (cap < 1) cap = 1;
            size_t sz = sizeof *c + (size_t)cap * sizeof(struct node);
            c = kmalloc((unsigned long)sz);
            if (!c) { d->oom = 1; return 0; }
            /* Zero the whole slab: node_alloc/node_recycle preserve wrap_next
             * across a memset, so a never-used slot must already read NULL
             * there rather than kmalloc garbage. */
            memset(c, 0, (unsigned long)sz);
            c->used = 0; c->cap = cap;
            c->next = d->nchunks; d->nchunks = c;
            d->bytes += sz;
        }
        n = &nc_nodes(c)[c->used++];
    }
    /* wrap_next + NF_WRAPLISTED are the one pair of fields that survive a
     * recycle: they thread the document's "ever had a JS wrapper" chain, and
     * clearing them would cut that chain in half (see dom_clear_wrappers). */
    struct node *wn = n->wrap_next;
    uint16_t wf = (uint16_t)(n->flags & NF_WRAPLISTED);
    memset(n, 0, sizeof *n);
    n->wrap_next = wn;
    n->flags = wf;
    n->doc = d;
    n->tag = "";
    n->serial = ++d->next_serial;   /* never 0 -- 0 marks a dead slot */
    return n;
}

static void node_recycle(struct dom_doc *d, struct node *n)
{
    id_unindex(d, n);
    if (n->style) { kfree(n->style); n->style = 0; }
    if (n->computed && g_computed_free) { g_computed_free(n->computed); n->computed = 0; }
    /* Interned names/ids/classes are NOT unref'd here: the document's ledger
     * owns exactly one ref per distinct string for the document's lifetime.
     * Arena bytes (text, attribute values, the attrs[]/classes[] arrays) are
     * likewise abandoned -- they die with the chunk chain. */
    struct node *wn = n->wrap_next;
    uint16_t wf = (uint16_t)(n->flags & NF_WRAPLISTED);
    memset(n, 0, sizeof *n);
    n->wrap_next = wn;
    n->flags = wf;
    n->doc = d;
    n->tag = "";
    n->serial = 0;                  /* every {node,serial} handle now fails */
    n->next = d->freelist;
    d->freelist = n;
}

/* ------------------------------------------------------------------ */
/* document lifetime                                                   */
/* ------------------------------------------------------------------ */
struct dom_doc *dom_doc_new(void)
{
    dom_atoms_init();
    struct dom_doc *d = kmalloc(sizeof *d);
    if (!d) return 0;
    memset(d, 0, sizeof *d);
    d->bytes = sizeof *d;
    struct node *r = node_alloc(d);
    if (!r) { kfree(d); return 0; }
    r->type = N_DOCUMENT;
    r->tag = "#document";
    d->root = r;
    return d;
}

struct node *dom_doc_root(struct dom_doc *d) { return d ? d->root : 0; }

/* documentElement / body. Both are one short sibling walk each -- the document
 * node has a handful of children (doctype, comments, <html>) and <html> has
 * two or three -- so they are computed rather than cached: a cached pointer
 * would have to be invalidated by every dom_remove_child() a script makes. */
struct node *dom_doc_element(struct dom_doc *d)
{
    if (!d || !d->root) return 0;
    for (struct node *c = d->root->first_child; c; c = c->next)
        if (c->type == N_ELEM && c->tag_id == TAG_HTML && c->ns == NS_HTML) return c;
    return 0;
}

struct node *dom_doc_body(struct dom_doc *d)
{
    struct node *html = dom_doc_element(d);
    if (!html) return 0;
    /* Per the DOM spec, document.body is the first <body> OR <frameset> child
     * of the document element -- a frameset document has no <body> at all. */
    for (struct node *c = html->first_child; c; c = c->next)
        if (c->type == N_ELEM && c->ns == NS_HTML &&
            (c->tag_id == TAG_BODY || c->tag_id == TAG_FRAMESET)) return c;
    return 0;
}

size_t dom_doc_bytes(const struct dom_doc *d) { return d ? d->bytes : 0; }
int  dom_doc_quirks(const struct dom_doc *d) { return d ? d->quirks : QM_NO_QUIRKS; }
void dom_doc_set_quirks(struct dom_doc *d, int mode) { if (d) d->quirks = mode; }

static void doc_destroy(struct dom_doc *d)
{
    /* Walk every slot ever handed out rather than the tree: styles and computed
     * styles live outside the arena, and a node detached by a script (or left
     * on the free list) would otherwise leak them. Dense chunk arrays make this
     * a flat scan -- no recursion, no stack depth. */
    for (struct node_chunk *c = d->nchunks; c; c = c->next) {
        struct node *ns = nc_nodes(c);
        for (uint32_t i = 0; i < c->used; i++) {
            if (ns[i].style) { kfree(ns[i].style); ns[i].style = 0; }
            if (ns[i].computed && g_computed_free) { g_computed_free(ns[i].computed); ns[i].computed = 0; }
            ns[i].jsw = 0;
        }
    }
    for (struct node_chunk *c = d->nchunks; c; ) { struct node_chunk *nx = c->next; kfree(c); c = nx; }
    for (struct dom_chunk  *c = d->schunks; c; ) { struct dom_chunk  *nx = c->next; kfree(c); c = nx; }
    if (d->iset) {
        for (uint32_t i = 0; i < d->icap; i++) {
            lwc_string *s = d->iset[i];
            if (s) { lwc_string_unref(s); }
        }
        kfree(d->iset);
    }
    if (d->idb) kfree(d->idb);
    kfree(d);
}

/* Recycle `root` and everything under it without recursing: children are pushed
 * onto a work list threaded through ->next, which node_recycle then reuses as
 * the free-list link. */
static void recycle_tree(struct dom_doc *d, struct node *root)
{
    struct node *stack = root;
    root->next = 0;
    while (stack) {
        struct node *n = stack;
        stack = n->next;
        for (struct node *c = n->first_child; c; ) {
            struct node *nx = c->next;
            c->next = stack; stack = c;
            c = nx;
        }
        node_recycle(d, n);
    }
}

static void unlink_from_parent(struct node *c)
{
    struct node *p = c->parent;
    if (!p) { c->prev = c->next = 0; return; }
    if (c->prev) c->prev->next = c->next; else p->first_child = c->next;
    if (c->next) c->next->prev = c->prev; else p->last_child  = c->prev;
    c->parent = 0; c->prev = 0; c->next = 0;
}

void dom_destroy_subtree(struct node *n)
{
    if (!n || !n->doc || n == n->doc->root) return;
    unlink_from_parent(n);
    recycle_tree(n->doc, n);
}

void dom_destroy_children(struct node *n)
{
    if (!n || !n->doc) return;
    struct node *c = n->first_child;
    n->first_child = n->last_child = 0;
    while (c) {
        struct node *nx = c->next;
        c->parent = 0; c->prev = 0; c->next = 0;
        recycle_tree(n->doc, c);
        c = nx;
    }
}

void dom_free(struct node *n)
{
    if (!n || !n->doc) return;
    if (n == n->doc->root) { doc_destroy(n->doc); return; }
    dom_destroy_subtree(n);
}

/* ------------------------------------------------------------------ */
/* tree mutation                                                       */
/* ------------------------------------------------------------------ */
void dom_append_child(struct node *p, struct node *c)
{
    if (!p || !c || p == c || c == c->doc->root) return;
    if (c->parent) unlink_from_parent(c);        /* DOM move semantics */
    c->parent = p;
    c->prev = p->last_child;
    c->next = 0;
    if (p->last_child) p->last_child->next = c; else p->first_child = c;
    p->last_child = c;
}

void dom_remove_child(struct node *p, struct node *c)
{
    if (!p || !c || c->parent != p) return;
    unlink_from_parent(c);                       /* O(1): the node knows its prev */
}

void dom_insert_before(struct node *p, struct node *c, struct node *ref)
{
    if (!p || !c || p == c || c == c->doc->root) return;
    if (!ref || ref->parent != p) { dom_append_child(p, c); return; }
    if (ref == c) return;
    if (c->parent) unlink_from_parent(c);        /* DOM move semantics */
    c->parent = p;
    c->next = ref;
    c->prev = ref->prev;
    if (ref->prev) ref->prev->next = c; else p->first_child = c;
    ref->prev = c;
}

/* ------------------------------------------------------------------ */
/* node construction                                                   */
/* ------------------------------------------------------------------ */
/* Lowercase into the arena. Element and attribute names are ASCII-lowercased
 * exactly as the old scanner did (HTML namespace); the copy is transient, the
 * interned string is what survives. */
static int attr_set(struct dom_doc *d, struct node *n, const char *lname, size_t nlen,
                    const char *val, size_t vlen);   /* defined with the attributes */

static char *lower_tmp(struct dom_doc *d, const char *s, size_t len, char *stackbuf, size_t stackcap)
{
    char *out = (len < stackcap) ? stackbuf : (char *)arena_alloc(d, len + 1);
    if (!out) return 0;
    for (size_t i = 0; i < len; i++) out[i] = (char)lc((unsigned char)s[i]);
    out[len] = 0;
    return out;
}

static struct node *elem_new(struct dom_doc *d, const char *lname, size_t len)
{
    struct node *n = node_alloc(d);
    if (!n) return 0;
    n->type = N_ELEM;
    n->ns = NS_HTML;
    n->name = doc_intern(d, lname, len);
    if (!n->name) { node_recycle(d, n); return 0; }
    n->tag = lwc_string_data(n->name);          /* NUL-terminated by libwapcaplet */
    n->tag_id = tag_id_of(n->name);
    return n;
}

struct node *dom_create_element(struct dom_doc *d, const char *name, int len)
{
    if (!d || !name) return 0;
    size_t l = (len < 0) ? zlen(name) : (size_t)len;
    if (!l) return 0;
    char sb[64];
    char *low = lower_tmp(d, name, l, sb, sizeof sb);
    if (!low) return 0;
    return elem_new(d, low, l);
}

/* The tree builder's counterpart: the name is taken verbatim and the namespace
 * is explicit. Everything else (interning, tag_id, the arena) is shared with
 * dom_create_element, so a foreign element is not a second kind of node. */
struct node *dom_create_element_ns(struct dom_doc *d, const char *name, int len, int ns)
{
    if (!d || !name) return 0;
    size_t l = (len < 0) ? zlen(name) : (size_t)len;
    if (!l) return 0;
    struct node *n = elem_new(d, name, l);
    if (n) n->ns = (uint8_t)ns;
    return n;
}

/* Shallow element copy into an EXPLICIT document. Interned names are process
 * global (libwapcaplet), so copying across documents is still just a doc_hold
 * on the same lwc_string -- only the per-document reference ledger differs.
 * Values are re-copied into `d`'s arena by attr_set. */
static struct node *clone_elem_into(struct dom_doc *d, const struct node *src)
{
    struct node *n = node_alloc(d);
    if (!n) return 0;
    n->type = N_ELEM;
    n->ns = src->ns;
    n->name = doc_hold(d, src->name);            /* already interned by src */
    n->tag = src->name ? lwc_string_data(src->name) : "";
    n->tag_id = src->tag_id;
    n->htag = src->htag;
    /* Attributes are copied through attr_set so the clone's id/class indexes
     * are built the same way the original's were -- a clone that skipped that
     * would be invisible to getElementById and to every class selector. */
    for (int i = 0; i < src->nattr; i++) {
        lwc_string *an = src->attrs[i].name;
        attr_set(d, n, lwc_string_data(an), lwc_string_length(an),
                 src->attrs[i].value, src->attrs[i].vlen);
    }
    return n;
}

struct node *dom_clone_element(const struct node *src)
{
    if (!src || src->type != N_ELEM || !src->doc) return 0;
    return clone_elem_into(src->doc, src);
}

/* One node, no children, into `d`. NULL for a type a fragment can never carry
 * (a doctype) -- dom_import_node then skips that node's whole subtree. */
static struct node *import_one(struct dom_doc *d, const struct node *s)
{
    switch (s->type) {
    case N_ELEM: {
        struct node *n = clone_elem_into(d, s);
        if (n && s->raw && s->rawlen > 0) {
            n->raw = arena_dup(d, s->raw, (size_t)s->rawlen);
            n->rawlen = n->raw ? s->rawlen : 0;
        }
        return n;
    }
    case N_TEXT:    return dom_create_text(d, s->text ? s->text : "", s->textlen);
    case N_COMMENT: return dom_create_comment(d, s->text ? s->text : "", s->textlen);
    default:        return 0;
    }
}

struct node *dom_import_node(struct dom_doc *d, const struct node *src)
{
    if (!d || !src) return 0;
    struct node *root = import_one(d, src);
    if (!root) return 0;

    /* Iterative, like every other walk here: an innerHTML= string can nest as
     * deep as the fragment parser allows (DOM_MAX_TREE_DEPTH). The invariant is
     * `dst == the copy of s->parent`, which each of the three moves preserves:
     * descending sets dst to the copy of s, a sibling step leaves s->parent (so
     * dst) alone, and popping walks both up one level in lockstep. */
    const struct node *s = src->first_child;
    struct node *dst = root;
    while (s) {
        struct node *c = import_one(d, s);
        if (c) dom_append_child(dst, c);
        if (c && s->first_child) { s = s->first_child; dst = c; continue; }
        /* No copy (or nothing below): skip this subtree and take the next
         * sibling, climbing out of finished levels first. */
        while (s != src && !s->next) { s = s->parent; dst = dst->parent; }
        if (s == src) break;
        s = s->next;
    }
    return root;
}

void dom_set_raw(struct node *n, const char *src, int len)
{
    if (!n || !n->doc || !src || len < 0) return;
    n->raw = arena_dup(n->doc, src, (size_t)len);
    n->rawlen = n->raw ? len : 0;
}

struct node *dom_create_text(struct dom_doc *d, const char *text, int len)
{
    if (!d) return 0;
    size_t l = (len < 0) ? zlen(text ? text : "") : (size_t)len;
    struct node *n = node_alloc(d);
    if (!n) return 0;
    n->type = N_TEXT;
    n->tag = "#text";
    n->text = arena_dup(d, text ? text : "", l);
    if (!n->text) { node_recycle(d, n); return 0; }
    n->textlen = (int)l;
    n->textcap = (int)l + 1;
    return n;
}

int dom_text_append(struct node *n, const char *s, int len)
{
    if (!n || !n->doc || !s || len <= 0) return 0;
    if (n->type != N_TEXT && n->type != N_COMMENT) return 0;
    struct dom_doc *d = n->doc;
    size_t need = (size_t)n->textlen + (size_t)len + 1;
    if ((size_t)n->textcap < need) {
        /* Geometric growth in the arena. The old block is abandoned rather than
         * freed -- that is the arena's whole bargain, and a text run doubles at
         * most log2(len) times, so the waste is bounded by the run itself. */
        size_t cap = n->textcap > 0 ? (size_t)n->textcap : 32;
        while (cap < need) cap *= 2;
        char *nb = arena_alloc(d, cap);
        if (!nb) return 0;
        if (n->textlen) memcpy(nb, n->text, (unsigned long)n->textlen);
        n->text = nb;
        n->textcap = (int)cap;
    }
    memcpy(n->text + n->textlen, s, (unsigned long)len);
    n->textlen += len;
    n->text[n->textlen] = 0;
    return 1;
}

struct node *dom_create_comment(struct dom_doc *d, const char *data, int len)
{
    if (!d) return 0;
    size_t l = (len < 0) ? zlen(data ? data : "") : (size_t)len;
    struct node *n = node_alloc(d);
    if (!n) return 0;
    n->type = N_COMMENT;
    n->tag = "#comment";
    /* Comment data reuses text/textlen: same "arena copy, NUL-terminated"
     * contract, so anything that already knows how to read a text payload
     * (serialisers, the JS bindings) needs no second code path. */
    n->text = arena_dup(d, data ? data : "", l);
    if (!n->text) { node_recycle(d, n); return 0; }
    n->textlen = (int)l;
    n->textcap = (int)l + 1;
    return n;
}

struct node *dom_create_doctype(struct dom_doc *d, const char *name,
                                const char *pubid, const char *sysid)
{
    if (!d) return 0;
    struct node *n = node_alloc(d);
    if (!n) return 0;
    n->type = N_DOCTYPE;
    n->tag = "#doctype";
    if (name && *name) {
        char sb[64];
        size_t l = zlen(name);
        char *low = lower_tmp(d, name, l, sb, sizeof sb);
        if (low) n->name = doc_intern(d, low, l);
    }
    if (pubid) n->pubid = arena_dup(d, pubid, zlen(pubid));
    if (sysid) n->sysid = arena_dup(d, sysid, zlen(sysid));
    return n;
}

const char *dom_doctype_name(const struct node *n)
{
    if (!n || n->type != N_DOCTYPE || !n->name) return "";
    return lwc_string_data(n->name);
}

/* ------------------------------------------------------------------ */
/* attributes                                                          */
/* ------------------------------------------------------------------ */
static int classes_push(struct dom_doc *d, struct node *n, lwc_string *s)
{
    if (n->nclass >= n->clscap) {
        int ncap = n->clscap ? n->clscap * 2 : 4;
        lwc_string **na = arena_alloc(d, (size_t)ncap * sizeof *na);
        if (!na) return 0;
        if (n->nclass) memcpy(na, n->classes, (unsigned long)n->nclass * sizeof *na);
        n->classes = na; n->clscap = ncap;   /* the old array stays in the arena */
    }
    n->classes[n->nclass++] = s;
    return 1;
}

static void set_classes(struct dom_doc *d, struct node *n, const char *v, size_t vlen)
{
    n->nclass = 0; n->classes = 0; n->clscap = 0;
    size_t i = 0;
    while (i < vlen) {
        while (i < vlen && sp((unsigned char)v[i])) i++;
        size_t s = i;
        while (i < vlen && !sp((unsigned char)v[i])) i++;
        if (i > s) {
            lwc_string *t = doc_intern(d, v + s, i - s);
            if (t && !classes_push(d, n, t)) break;
        }
    }
}

static void set_id(struct dom_doc *d, struct node *n, const char *v, size_t vlen)
{
    id_unindex(d, n);
    n->id = vlen ? doc_intern(d, v, vlen) : 0;
    id_index(d, n);
}

/* Append (or overwrite) one attribute. `lname` must already be lowercase. */
static int attr_set(struct dom_doc *d, struct node *n,
                    const char *lname, size_t nlen,
                    const char *val, size_t vlen)
{
    if (n->type != N_ELEM || !nlen) return 0;
    lwc_string *an = attr_atom(lname, nlen);
    if (an) doc_hold(d, an); else an = doc_intern(d, lname, nlen);
    if (!an) return 0;

    const char *v = vlen ? arena_dup(d, val, vlen) : "";
    if (!v) return 0;

    int slot = -1;
    for (int i = 0; i < n->nattr; i++) if (n->attrs[i].name == an) { slot = i; break; }
    if (slot < 0) {
        if (n->nattr >= n->attrcap) {
            int ncap = n->attrcap ? n->attrcap * 2 : 4;
            struct dom_attr *na = arena_alloc(d, (size_t)ncap * sizeof *na);
            if (!na) return 0;
            if (n->nattr) memcpy(na, n->attrs, (unsigned long)n->nattr * sizeof *na);
            n->attrs = na; n->attrcap = ncap;   /* old array abandoned in the arena */
        }
        slot = n->nattr++;
    }
    n->attrs[slot].name = an;
    n->attrs[slot].value = v;
    n->attrs[slot].vlen = (uint32_t)vlen;

    if (an == dom_atoms.a_id)    set_id(d, n, v, vlen);
    else if (an == dom_atoms.a_class) set_classes(d, n, v, vlen);
    return 1;
}

int dom_set_attr(struct node *n, const char *name, const char *val)
{
    if (!n || !name || n->type != N_ELEM) return 0;
    struct dom_doc *d = n->doc;
    size_t nl = zlen(name);
    if (!nl) return 0;
    char sb[64];
    char *low = lower_tmp(d, name, nl, sb, sizeof sb);
    if (!low) return 0;
    return attr_set(d, n, low, nl, val ? val : "", val ? zlen(val) : 0);
}

int dom_set_attr_raw(struct node *n, const char *name, int nlen,
                     const char *val, int vlen)
{
    if (!n || !name || n->type != N_ELEM || nlen <= 0) return 0;
    return attr_set(n->doc, n, name, (size_t)nlen,
                    val ? val : "", (vlen > 0) ? (size_t)vlen : 0);
}

const char *dom_attr_lw(const struct node *n, lwc_string *name)
{
    if (!n || n->type != N_ELEM || !name) return 0;
    for (int i = 0; i < n->nattr; i++)
        if (n->attrs[i].name == name) return n->attrs[i].value;
    return 0;
}

int dom_has_attr_lw(const struct node *n, lwc_string *name)
{
    return dom_attr_lw(n, name) != 0;
}

const char *dom_attr_name_at(const struct node *n, int i)
{
    if (!n || i < 0 || i >= n->nattr) return 0;
    return lwc_string_data(n->attrs[i].name);
}

const char *dom_attr_value_at(const struct node *n, int i)
{
    if (!n || i < 0 || i >= n->nattr) return 0;
    return n->attrs[i].value;
}

const char *dom_attr(const struct node *n, const char *name)
{
    if (!n || n->type != N_ELEM || !n->nattr || !name) return 0;
    dom_atoms_init();
    size_t len = zlen(name);
    if (!len) return 0;

    char sb[64];
    if (len < sizeof sb) {
        for (size_t i = 0; i < len; i++) sb[i] = (char)lc((unsigned char)name[i]);
        sb[len] = 0;
        lwc_string *at = attr_atom(sb, len);
        if (at) return dom_attr_lw(n, at);      /* the hot path: pointer compares */
    }
    /* Cold path: an attribute name nobody asks for often. Compare bytes rather
     * than interning a transient string into the process-global table. */
    for (int i = 0; i < n->nattr; i++) {
        lwc_string *an = n->attrs[i].name;
        if (lwc_string_length(an) != len) continue;
        const char *ad = lwc_string_data(an);
        size_t k = 0;
        while (k < len && ad[k] == (char)lc((unsigned char)name[k])) k++;
        if (k == len) return n->attrs[i].value;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* id lookup + JS wrapper slots                                        */
/* ------------------------------------------------------------------ */
struct node *dom_get_element_by_id(struct dom_doc *d, const char *id)
{
    if (!d || !d->idb || !id || !*id) return 0;
    /* getElementById is case-SENSITIVE per the DOM spec, so this is an exact
     * interned-pointer match; a value nothing in the document uses will not be
     * in the global table at all and the lookup exits immediately. */
    lwc_string *k = 0;
    if (lwc_intern_string(id, zlen(id), &k) != lwc_error_ok || !k) return 0;
    struct node *hit = 0;
    for (struct node *e = d->idb[ptr_hash(k) & (d->idcap - 1)]; e; e = e->id_next) {
        if (e->id != k) continue;
        /* Only nodes connected to the document are visible, matching the tree
         * walk this replaced (a script may hold a detached element with an id). */
        struct node *p = e;
        while (p->parent) p = p->parent;
        if (p == d->root) { hit = e; break; }
    }
    lwc_string_unref(k);
    return hit;
}

void dom_set_wrapper(struct node *n, void *jsobj)
{
    if (!n || !n->doc) return;
    n->jsw = jsobj;
    if (jsobj && !(n->flags & NF_WRAPLISTED)) {
        n->flags |= NF_WRAPLISTED;
        n->wrap_next = n->doc->wrapped;
        n->doc->wrapped = n;
    }
}

void dom_clear_wrappers(struct dom_doc *d)
{
    if (!d) return;
    /* NF_WRAPLISTED is deliberately never cleared, so a slot appears on this
     * chain at most once for the document's lifetime even across recycling --
     * which is what keeps wrap_next a valid link after node_recycle memsets
     * everything else. */
    for (struct node *n = d->wrapped; n; n = n->wrap_next) n->jsw = 0;
}

/* ------------------------------------------------------------------ */
/* parsing                                                             */
/* ------------------------------------------------------------------ */
/* The tolerant single-pass scanner that used to live here is gone: html_parse()
 * (html_tree.c, driven by html_tokenizer.c) is the spec algorithm and replaces
 * it wholesale. dom_parse() stays as the one-line entry point every caller
 * already uses, so the switch is invisible above dom.h. */
struct node *dom_parse(const char *html, int len)
{
    struct dom_doc *d = 0;
    struct node *root = html_parse(&d, html, len > 0 ? len : 0);
    if (root) return root;
    /* html_parse only fails if the document itself could not be allocated;
     * hand back an empty document rather than NULL so callers that walk
     * root->first_child unconditionally keep working. */
    d = dom_doc_new();
    return d ? dom_doc_root(d) : 0;
}
