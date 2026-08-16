#ifndef LOGIT_DOM_H
#define LOGIT_DOM_H

#include <stdint.h>
#include <stddef.h>

/* S1b: the DOM core.
 *
 * `struct node` is a *field-compatible superset* of the old one: every name
 * layout.c / css_extra.c / browser.c / the host tests already touch keeps its
 * meaning, so those files compile unchanged. The single representation change
 * is `char tag[16]` -> `const char *tag` (still NUL-terminated), which keeps
 * strcmp(n->tag,..), n->tag[k], strlen(n->tag) and JS_NewString(ctx,n->tag)
 * working while removing the 15-character tag-name cap.
 *
 * Everything a document owns -- nodes, attribute/class arrays, text, comment
 * data, verbatim <svg> spans -- lives in a `struct dom_doc` arena of 256 KiB
 * chunks. That is a feasibility prerequisite, not an optimisation: mini-libc's
 * malloc (c/apps/libc/src/malloc.c) is first-fit and walks the header chain
 * from the arena start on every call, so with LibCSS's hundreds of thousands
 * of live blocks a per-node malloc turns parsing into a quadratic crawl.
 * Bump-allocating from chunks also makes dom_free() non-recursive: it drops
 * the chunk chain instead of walking the tree.
 *
 * Names (tag names, attribute names, the id value, class tokens) are interned
 * through libwapcaplet -- LibCSS's own interning library, already linked --
 * so selector matching is a pointer compare. VALUES are arena copies, never
 * interned: attribute values (Tailwind class lists, data-*, srcset), text and
 * comment data are effectively unique per occurrence and interning them would
 * grow the process-global lwc table without bound.
 */

/* libwapcaplet's lwc_string, forward-declared so dom.h itself costs no include
 * path (only dom.c and css_engine.c need -I third_party/css/libwapcaplet). C11
 * permits the identical typedef to appear again when libwapcaplet.h is pulled
 * in afterwards. */
#ifndef libwapcaplet_h_
typedef struct lwc_string_s lwc_string;
#endif

struct dom_doc;

/* Node types. N_ELEM/N_TEXT keep their values (0/1) -- they are compared, not
 * serialised, but there is no reason to renumber them. */
enum { N_ELEM = 0, N_TEXT = 1, N_COMMENT = 2, N_DOCTYPE = 3, N_DOCUMENT = 4 };

/* Element namespace. html_tree.c sets NS_SVG/NS_MATHML inside foreign content;
 * everything built through the DOM API is NS_HTML unless it says otherwise. */
enum { NS_HTML = 0, NS_SVG = 1, NS_MATHML = 2 };

/* Document quirks mode, set from the doctype by html_tree.c. css_engine reads
 * it through dom_doc_quirks() to pick the quirks UA sheet and to let LibCSS
 * parse quirky lengths/colours. */
enum { QM_NO_QUIRKS = 0, QM_QUIRKS = 1, QM_LIMITED_QUIRKS = 2 };

/* node->flags */
enum {
    NF_ID_INDEXED = 1u << 0,   /* node is linked into doc->idbuckets via id_next */
    NF_WRAPLISTED = 1u << 1,   /* node is on doc's wrap_next chain (never cleared:
                                * see dom_clear_wrappers) */
    NF_SELF_CLOSED = 1u << 2,  /* start tag ended in "/>" (informational) */
    NF_SCRIPT_DONE = 1u << 3   /* a <script> already prepared+run: parser-run
                                * scripts are stamped so DOM insertion of the
                                * SAME node (or re-insertion) never re-runs it.
                                * See dom_script_mark_done / dom_script_is_done
                                * and 2026-08-16-inserted-script-execution. */
};

/* Well-known tag ids. 0 = unknown/other; the value is stable for switch().
 * Element creation fills node->tag_id from the interned name, so layout and
 * CSS can branch on an integer instead of strcmp. */
enum {
    TAG_UNKNOWN = 0,
    TAG_HTML, TAG_HEAD, TAG_BODY, TAG_TITLE, TAG_META, TAG_LINK, TAG_BASE,
    TAG_STYLE, TAG_SCRIPT, TAG_NOSCRIPT, TAG_TEMPLATE,
    TAG_DIV, TAG_SPAN, TAG_P, TAG_A, TAG_IMG, TAG_BR, TAG_HR, TAG_WBR,
    TAG_H1, TAG_H2, TAG_H3, TAG_H4, TAG_H5, TAG_H6,
    TAG_UL, TAG_OL, TAG_LI, TAG_DL, TAG_DT, TAG_DD,
    TAG_TABLE, TAG_THEAD, TAG_TBODY, TAG_TFOOT, TAG_TR, TAG_TD, TAG_TH,
    TAG_CAPTION, TAG_COL, TAG_COLGROUP,
    TAG_FORM, TAG_INPUT, TAG_BUTTON, TAG_SELECT, TAG_OPTION, TAG_TEXTAREA,
    TAG_LABEL, TAG_FIELDSET, TAG_LEGEND,
    TAG_B, TAG_I, TAG_EM, TAG_STRONG, TAG_CODE, TAG_PRE, TAG_SMALL, TAG_SUB,
    TAG_SUP, TAG_U, TAG_S, TAG_Q, TAG_BLOCKQUOTE,
    TAG_HEADER, TAG_FOOTER, TAG_SECTION, TAG_ARTICLE, TAG_NAV, TAG_MAIN,
    TAG_ASIDE, TAG_FIGURE, TAG_FIGCAPTION,
    TAG_SVG, TAG_MATH, TAG_CANVAS, TAG_VIDEO, TAG_AUDIO, TAG_SOURCE,
    TAG_IFRAME, TAG_EMBED, TAG_OBJECT, TAG_PARAM, TAG_TRACK, TAG_AREA,
    TAG_PICTURE, TAG_FRAME, TAG_FRAMESET, TAG_PLAINTEXT, TAG_XMP, TAG_IMAGE,
    TAG__COUNT
};

/* One attribute. The name is interned (pointer-comparable, always lowercase for
 * HTML); the value is an arena copy that is NUL-terminated at value[vlen]. */
struct dom_attr {
    lwc_string *name;
    const char *value;
    uint32_t    vlen;
};

struct node {
    /* ---------------- the compatibility core ---------------- */
    int type;                       /* N_ELEM / N_TEXT / N_COMMENT / ... */
    const char *tag;                /* NUL-terminated: the lowercased element
                                     * name (== lwc_string_data(name)), or one of
                                     * "#text"/"#comment"/"#document"/"#doctype".
                                     * Never NULL, so tag[0] is always safe. */
    struct dom_attr *attrs; int nattr;
    char *text; int textlen;        /* N_TEXT/N_COMMENT payload: an arena copy,
                                     * NUL-terminated at text[textlen]. Never a
                                     * slice of the source buffer -- layout.c
                                     * points struct item.text straight at it. */
    struct node *parent, *first_child, *last_child, *next;
    void *style;                    /* struct cstyle*, kmalloc'd by css_engine */
    char *raw; int rawlen;          /* <svg>: verbatim source span (start '<' to
                                     * the matching "</svg>"), an arena copy; the
                                     * DOM attrs lowercase viewBox, so layout
                                     * decodes SVG from this instead. */

    /* ---------------- the new model ---------------- */
    struct node *prev;              /* previous sibling -> O(1) unlink, and the
                                     * previous-element-sibling walk CSS needs */
    struct dom_doc *doc;            /* owning document (never NULL) */
    uint32_t serial;                /* generation stamp: bumped every time this
                                     * slot is handed out. A JS wrapper stores
                                     * {node,serial} and compares, so a slot
                                     * recycled from the free list can never be
                                     * addressed through a stale handle. */
    uint16_t tag_id;                /* TAG_* (0 = unknown) */
    uint16_t flags;                 /* NF_* */
    uint8_t  ns;                    /* NS_* */
    int attrcap;                    /* capacity of attrs[] (arena, doubling) */

    uint16_t htag;                  /* enum html_tag (html_tags.inc), the id the
                                     * HTML5 tree builder branches on. It is a
                                     * DIFFERENT enumeration from tag_id above:
                                     * tag_id names the ~90 tags layout/CSS care
                                     * about, htag names the 147 the spec's
                                     * special/formatting/scope sets are written
                                     * over. Only html_tree.c fills it in -- an
                                     * element built through the DOM API leaves
                                     * it 0, which reads as "unknown tag" and is
                                     * the right answer for a node the tree
                                     * builder never saw. */
    int textcap;                    /* N_TEXT/N_COMMENT: bytes reserved for
                                     * ->text, so dom_text_append can grow a run
                                     * in place instead of re-copying per token */

    lwc_string *name;               /* interned element name (N_ELEM), else NULL.
                                     * For N_DOCTYPE this is the doctype name. */
    lwc_string *id;                 /* interned value of the id attribute, or NULL */
    lwc_string **classes; int nclass, clscap;   /* interned class tokens */

    struct node *id_next;           /* doc id-index bucket chain */
    struct node *cls_next;          /* reserved for the class index. Not wired:
                                     * one link per node cannot express
                                     * membership of several class buckets, so
                                     * the selector index will need its own
                                     * (class,node) pair list. Kept so the
                                     * struct does not churn again. */

    void *computed;                 /* css_computed_style* (LibCSS interns and
                                     * refcounts these, so holding one is ~8
                                     * bytes/node, not a whole style block) */
    void *jsw;                      /* WEAK JS wrapper slot: the JSObject* of the
                                     * Element wrapper, holding NO reference.
                                     * The finalizer clears it. */
    struct node *wrap_next;         /* doc's chain of nodes that ever had a
                                     * wrapper (see dom_clear_wrappers) */

    const char *pubid, *sysid;      /* N_DOCTYPE: PUBLIC/SYSTEM identifiers */
};

/* A <script> node's run-once flag (NF_SCRIPT_DONE). dom.c owns the field;
 * these two are the only writers/readers, so browser.c (parser-run scripts)
 * and js_dom.c (inserted scripts) agree without either reaching into
 * node->flags directly. Declared AFTER struct node so the parameter type is
 * the file-scope struct, not a prototype-scoped one. */
void dom_script_mark_done(struct node *n);
int  dom_script_is_done(const struct node *n);

/* The tree builder's open-element stack cap. The DOM data model itself has no
 * depth limit, but the recursive consumers (css_engine's style_node,
 * layout.c's layout_block, css_extra's walk) run on the browser's 8 MiB ring-3
 * stack. Measured worst frame is ~1 KiB (style_node holds a css_select_results
 * plus locals), so ~512 is the depth at which those walks must be converted to
 * explicit stacks rather than the tree builder being capped. */
#define DOM_MAX_TREE_DEPTH 512

/* ---------------- parsing / lifetime ---------------- */

/* Parse an HTML document; returns the document node (type N_DOCUMENT, tag
 * "#document") whose children are the doctype, any top-level comments, and the
 * <html> element. NULL only if the document could not be allocated at all.
 *
 * This is html_parse() (html_tree.h): the WHATWG tree construction algorithm.
 * So the tree it returns is a SPEC tree, not the tag soup the old scanner
 * produced -- there is always an <html> with a <head> and a <body>, misnested
 * formatting elements are reconstructed, and table content is fostered. */
struct node *dom_parse(const char *html, int len);

/* Free a whole document (pass the N_DOCUMENT root) or detach+recycle a subtree
 * (pass any other node). Never recursive. */
void dom_free(struct node *n);

/* An empty document, for building a tree through the API. */
struct dom_doc *dom_doc_new(void);
struct node    *dom_doc_root(struct dom_doc *d);
/* document.documentElement: the root <html> element, or NULL. */
struct node    *dom_doc_element(struct dom_doc *d);
/* document.body: the first <body>/<frameset> child of the document element, or
 * NULL. The tree builder always makes one for a parsed document, so a caller
 * that used to hand-roll a two-level scan for <body> can just ask. */
struct node    *dom_doc_body(struct dom_doc *d);
/* Total bytes this document holds from the allocator (arena chunks + its two
 * index tables). */
size_t dom_doc_bytes(const struct dom_doc *d);
int    dom_doc_quirks(const struct dom_doc *d);
void   dom_doc_set_quirks(struct dom_doc *d, int mode);

/* ---------------- attributes ---------------- */

/* Attribute value by (case-insensitive) name, or NULL. Valueless attributes
 * read as "". */
const char *dom_attr(const struct node *n, const char *name);
/* Same, but the caller already holds the interned (lowercase) name: a pointer
 * compare per attribute. */
const char *dom_attr_lw(const struct node *n, lwc_string *name);
int         dom_has_attr_lw(const struct node *n, lwc_string *name);
/* Set (or add) an attribute; the name is lowercased, the value copied into the
 * arena. Keeps node->id and node->classes in sync. 1 on success. */
int         dom_set_attr(struct node *n, const char *name, const char *val);
/* Same, but the name is used VERBATIM (already lowercase, or deliberately not:
 * "viewBox" on an SVG element) and both strings carry explicit lengths so an
 * attribute value may contain NUL. The namespaced foreign attributes are stored
 * under their serialised form -- "xlink href", "xml lang", "xmlns xlink" -- a
 * space being the one character an HTML attribute name can never contain, so
 * one interned name still identifies one attribute. */
int         dom_set_attr_raw(struct node *n, const char *name, int nlen,
                             const char *val, int vlen);
/* Positional access for code that enumerates attributes (serialisers, dumps)
 * and would otherwise need libwapcaplet on its include path just to turn an
 * interned name back into bytes. Both are NUL-terminated; NULL if out of range. */
const char *dom_attr_name_at(const struct node *n, int i);
const char *dom_attr_value_at(const struct node *n, int i);

/* ---------------- construction / tree mutation ---------------- */

struct node *dom_create_element(struct dom_doc *d, const char *name, int len);
struct node *dom_create_text(struct dom_doc *d, const char *text, int len);
struct node *dom_create_comment(struct dom_doc *d, const char *data, int len);
struct node *dom_create_doctype(struct dom_doc *d, const char *name,
                                const char *pubid, const char *sysid);

/* Create an element in an explicit namespace with the name taken VERBATIM --
 * no ASCII-lowercasing. Foreign content needs both halves of that: SVG's
 * localName is case-sensitive ("foreignObject", "clipPath", "feGaussianBlur"),
 * and the caller (html_tree.c) has already lowercased HTML names via the
 * tokenizer, so lowercasing again here would only be able to do damage. */
struct node *dom_create_element_ns(struct dom_doc *d, const char *name, int len, int ns);

/* A shallow copy: same document, namespace, name, htag and attributes, no
 * children and no parent. This is what "reconstruct the active formatting
 * elements" and the adoption agency algorithm mean by "create an element for
 * the token for which the element was created". */
struct node *dom_clone_element(const struct node *n);

/* DOM importNode(deep): a deep copy of `src`'s whole subtree into document `d`,
 * returned detached. `src` may belong to a DIFFERENT document -- that is the
 * point. innerHTML= parses into a throwaway document (html_parse_fragment
 * makes its own), and the result cannot simply be spliced in: every string in
 * it lives in the throwaway document's arena and dies with it. Doctypes are
 * skipped along with their subtree; a fragment never contains one. */
struct node *dom_import_node(struct dom_doc *d, const struct node *src);

/* The doctype's name as bytes ("" if the doctype had none), so a serialiser
 * does not need libwapcaplet on its include path to print it. */
const char *dom_doctype_name(const struct node *n);

/* N_ELEM: record the verbatim source span this element was parsed from
 * (node->raw / ->rawlen). Only <svg> uses it -- see the field comment. */
void dom_set_raw(struct node *n, const char *src, int len);

void dom_append_child(struct node *parent, struct node *child);
/* Insert `child` immediately before `ref` (a child of `parent`); ref == NULL
 * appends. Foster parenting is the reason this exists: stray content inside a
 * <table> is inserted BEFORE the table, not at the end of some parent. */
void dom_insert_before(struct node *parent, struct node *child, struct node *ref);
/* Append bytes to an N_TEXT/N_COMMENT payload, growing ->text geometrically.
 * The tree builder inserts characters token by token but the spec's tree has
 * one text node per run, so without this every entity in a paragraph would
 * either split the node or re-copy the whole run. */
int  dom_text_append(struct node *n, const char *s, int len);
/* O(1): the node knows its prev. Only unlinks; the child stays allocated. */
void dom_remove_child(struct node *parent, struct node *child);
/* Unlink `n` from its parent and return it and its whole subtree to the free
 * list (iterative). Every wrapper into the subtree goes stale via `serial`. */
void dom_destroy_subtree(struct node *n);
/* Recycle every child of `n`, keeping `n` itself. */
void dom_destroy_children(struct node *n);

/* ---------------- indexes / wrappers ---------------- */

/* Exact-match (DOM-spec, case-sensitive) id lookup over the document's id
 * index; only nodes connected to the document root are returned. */
struct node *dom_get_element_by_id(struct dom_doc *d, const char *id);

/* Record (or clear) the WEAK JS wrapper for a node: `jsobj` is the wrapper's
 * JSObject*, stored without taking a reference. The first non-NULL store links
 * the node into the document's wrapper chain. */
void dom_set_wrapper(struct node *n, void *jsobj);

/* Drop every JS wrapper slot in the document. MUST be called before the
 * JSRuntime the wrappers came from is freed: browser.c's run_js() builds a
 * fresh JSRuntime per page, so a slot left over from page 1 would hand page 2
 * a dangling JSObject* the moment it touched document.body. */
void dom_clear_wrappers(struct dom_doc *d);

/* css_engine registers how to release node->computed, so dom.c stays free of
 * any LibCSS dependency beyond libwapcaplet. */
void dom_set_computed_free(void (*fn)(void *));

/* ---------------- interned atoms ---------------- */

/* Process-global atoms for the names hot paths ask for by literal. Interned
 * once (never released: they live as long as the process, like any atom
 * table). dom_atoms_init() is idempotent and is called lazily by dom.c. */
struct dom_atoms {
    lwc_string *a_class, *a_id, *a_style, *a_href, *a_src, *a_rel;
    lwc_string *a_width, *a_height, *a_type, *a_name, *a_data_src;
    lwc_string *a_alt, *a_title;
};
extern struct dom_atoms dom_atoms;
void dom_atoms_init(void);

#endif /* LOGIT_DOM_H */
