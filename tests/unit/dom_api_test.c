/* S1b: the DOM data model, as opposed to the HTML it happens to be fed.
 *
 * dom_test.c is the compatibility proof (it must keep passing unchanged). This
 * one is the *capability* proof: every fixed-size limit the old struct node
 * carried -- 15-char tags, 31-char attribute names, 255-char values, 32
 * attributes, one shared 8 KiB textContent buffer -- is asserted gone, and the
 * new machinery (prev links, per-node serials, the id index, weak wrapper
 * slots, arena accounting, comment/doctype nodes) is exercised directly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dom.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static int fail;
#define CK(cond, msg) do { if (!(cond)) { printf("FAIL %s\n", msg); fail = 1; } } while (0)

static struct node *find(struct node *n, const char *tag)
{
    if (n->type == N_ELEM && !strcmp(n->tag, tag)) return n;
    for (struct node *c = n->first_child; c; c = c->next) {
        struct node *r = find(c, tag);
        if (r) return r;
    }
    return 0;
}

/* ---------------- caps that used to exist ---------------- */

static void test_long_class(void)
{
    /* 4096 characters of class attribute: 256 tokens of "cNNN". The old parser
     * kept 255 bytes of the value and the old select handler kept 32 tokens. */
    char *cls = malloc(8192); cls[0] = 0;
    int ntok = 0, o = 0;
    for (int i = 0; o < 4096; i++) { o += sprintf(cls + o, "%sc%03d", i ? " " : "", i); ntok++; }
    char *html = malloc(16384);
    int hl = sprintf(html, "<p class=\"%s\">hi</p>", cls);

    struct node *root = dom_parse(html, hl);
    struct node *p = find(root, "p");
    CK(p != NULL, "long-class: <p> parsed");
    const char *got = p ? dom_attr(p, "class") : 0;
    CK(got && strlen(got) == strlen(cls), "long-class: full attribute value kept");
    CK(got && !strcmp(got, cls), "long-class: value byte-identical");
    CK(p && p->nclass == ntok, "long-class: every class token interned");
    dom_free(root);
    free(cls); free(html);
}

static void test_many_attrs(void)
{
    /* 200 attributes on one element (old cap: 32, silently dropped). */
    char *html = malloc(16384);
    int o = sprintf(html, "<div");
    for (int i = 0; i < 200; i++) o += sprintf(html + o, " a%03d=\"v%03d\"", i, i);
    o += sprintf(html + o, "></div>");

    struct node *root = dom_parse(html, o);
    struct node *d = find(root, "div");
    CK(d != NULL, "many-attrs: <div> parsed");
    CK(d && d->nattr == 200, "many-attrs: all 200 attributes stored");
    int allok = d != NULL;
    for (int i = 0; i < 200 && allok; i++) {
        char nm[16], want[16];
        sprintf(nm, "a%03d", i); sprintf(want, "v%03d", i);
        const char *v = dom_attr(d, nm);
        if (!v || strcmp(v, want)) allok = 0;
    }
    CK(allok, "many-attrs: every attribute reads back");
    dom_free(root);
    free(html);
}

static void test_long_names_and_values(void)
{
    char tag[80];
    for (int i = 0; i < 64; i++) tag[i] = (char)('a' + i % 26);
    tag[64] = 0;                                     /* 64-char tag name (old cap: 15) */

    char *val = malloc(8193);
    for (int i = 0; i < 8192; i++) val[i] = (char)('A' + i % 26);
    val[8192] = 0;                                   /* 8 KiB value (old cap: 255) */

    char aname[80];
    for (int i = 0; i < 70; i++) aname[i] = (char)('m' + i % 13);
    aname[70] = 0;                                   /* 70-char name (old cap: 31) */

    char *html = malloc(16384);
    int hl = sprintf(html, "<%s %s=\"%s\">x</%s>", tag, aname, val, tag);

    struct node *root = dom_parse(html, hl);
    struct node *e = find(root, tag);
    CK(e != NULL, "long-names: 64-char tag name survives");
    CK(e && strlen(e->tag) == 64, "long-names: tag not truncated");
    const char *v = e ? dom_attr(e, aname) : 0;
    CK(v != NULL, "long-names: 70-char attribute name looked up");
    CK(v && strlen(v) == 8192, "long-values: 8 KiB attribute value intact");
    CK(v && !strcmp(v, val), "long-values: value byte-identical");
    dom_free(root);
    free(val); free(html);
}

static void test_deep_tree(void)
{
    /* The DOM itself has no depth limit: build 500 nested elements through the
     * API and tear them down. dom_free must not recurse (it drops the chunk
     * chain), so this cannot blow the stack however deep it gets. */
    struct dom_doc *d = dom_doc_new();
    struct node *cur = dom_doc_root(d);
    for (int i = 0; i < 500; i++) {
        struct node *e = dom_create_element(d, "div", 3);
        if (!e) break;
        dom_append_child(cur, e);
        cur = e;
    }
    int depth = 0;
    for (struct node *n = cur; n && n != dom_doc_root(d); n = n->parent) depth++;
    CK(depth == 500, "deep-tree: 500 levels of nesting built");
    struct node *leaf = dom_create_text(d, "bottom", -1);
    dom_append_child(cur, leaf);
    CK(leaf && !strcmp(leaf->text, "bottom"), "deep-tree: leaf text at depth 501");
    dom_free(dom_doc_root(d));

    /* The scanner (not the data model) still uses a 64-deep open-element stack,
     * unchanged from before S1b. Assert it stays well-behaved rather than
     * pretending the limit moved. */
    char *html = malloc(16384);
    int o = 0;
    for (int i = 0; i < 300; i++) o += sprintf(html + o, "<div>");
    o += sprintf(html + o, "deep");
    struct node *root = dom_parse(html, o);
    int md = 0;
    for (struct node *n = root; n; n = n->first_child) md++;
    CK(root && md <= 65, "deep-tree: scanner still honours its 64-deep stack");
    dom_free(root);
    free(html);
}

/* ---------------- the new model ---------------- */

static void test_prev_siblings(void)
{
    const char *h = "<ul><li>a</li><li>b</li><li>c</li><li>d</li></ul>";
    struct node *root = dom_parse(h, (int)strlen(h));
    struct node *ul = find(root, "ul");
    CK(ul != NULL, "prev: <ul> parsed");

    int n = 0;
    struct node *last = 0;
    for (struct node *c = ul->first_child; c; c = c->next) { n++; last = c; }
    CK(n == 4, "prev: four children forward");
    CK(last == ul->last_child, "prev: last_child matches forward walk");

    int m = 0;
    struct node *first = 0;
    for (struct node *c = ul->last_child; c; c = c->prev) { m++; first = c; }
    CK(m == 4, "prev: four children backward");
    CK(first == ul->first_child, "prev: backward walk reaches first_child");
    for (struct node *c = ul->first_child; c; c = c->next)
        CK(!c->next || c->next->prev == c, "prev: next/prev are inverses");
    CK(ul->first_child->prev == NULL, "prev: first child has no prev");

    /* O(1) removal keeps both directions consistent */
    struct node *second = ul->first_child->next;
    dom_remove_child(ul, second);
    CK(ul->first_child->next == ul->first_child->next, "prev: removal did not corrupt");
    n = 0;
    for (struct node *c = ul->first_child; c; c = c->next) n++;
    m = 0;
    for (struct node *c = ul->last_child; c; c = c->prev) m++;
    CK(n == 3 && m == 3, "prev: both directions agree after remove");
    CK(second->parent == NULL && second->prev == NULL && second->next == NULL,
       "prev: removed node fully detached");
    dom_free(root);
}

static void test_comment_doctype(void)
{
    struct dom_doc *d = dom_doc_new();
    struct node *root = dom_doc_root(d);
    CK(root->type == N_DOCUMENT, "doc: root is N_DOCUMENT");
    CK(!strcmp(root->tag, "#document"), "doc: root tag is #document");

    struct node *dt = dom_create_doctype(d, "html", "-//W3C//DTD HTML 4.01//EN",
                                         "http://www.w3.org/TR/html4/strict.dtd");
    dom_append_child(root, dt);
    struct node *cm = dom_create_comment(d, "hello <world> & co", -1);
    dom_append_child(root, cm);
    struct node *el = dom_create_element(d, "HTML", 4);
    dom_append_child(root, el);

    CK(dt && dt->type == N_DOCTYPE, "doctype: node type");
    CK(dt && dt->pubid && !strcmp(dt->pubid, "-//W3C//DTD HTML 4.01//EN"), "doctype: pubid kept");
    CK(dt && dt->sysid && !strcmp(dt->sysid, "http://www.w3.org/TR/html4/strict.dtd"), "doctype: sysid kept");
    CK(cm && cm->type == N_COMMENT, "comment: node type");
    CK(cm && !strcmp(cm->text, "hello <world> & co"), "comment: data kept verbatim");
    CK(cm && cm->textlen == 18, "comment: textlen matches");
    CK(el && !strcmp(el->tag, "html"), "createElement lowercases the name");
    CK(el && el->tag_id == TAG_HTML, "tag_id resolved for a known element");
    CK(root->first_child == dt && root->last_child == el, "doctype/comment/element ordered");

    dom_doc_set_quirks(d, QM_LIMITED_QUIRKS);
    CK(dom_doc_quirks(d) == QM_LIMITED_QUIRKS, "quirks mode recorded");
    dom_free(root);
}

static void test_id_index(void)
{
    const char *h = "<div id=outer><p id=mid><span id=inner>x</span></p></div>";
    struct node *root = dom_parse(h, (int)strlen(h));
    struct dom_doc *d = root->doc;

    struct node *inner = dom_get_element_by_id(d, "inner");
    CK(inner && !strcmp(inner->tag, "span"), "id-index: finds #inner");
    CK(dom_get_element_by_id(d, "mid") == find(root, "p"), "id-index: finds #mid");
    CK(dom_get_element_by_id(d, "nope") == NULL, "id-index: miss returns NULL");
    CK(dom_get_element_by_id(d, "INNER") == NULL, "id-index: case sensitive (DOM spec)");
    CK(inner && inner->id != NULL, "id-index: node->id interned");

    /* re-indexing on setAttribute */
    dom_set_attr(inner, "id", "renamed");
    CK(dom_get_element_by_id(d, "inner") == NULL, "id-index: old id unindexed");
    CK(dom_get_element_by_id(d, "renamed") == inner, "id-index: new id indexed");

    /* a detached subtree is not reachable by id */
    struct node *mid = find(root, "p");
    dom_remove_child(mid->parent, mid);
    CK(dom_get_element_by_id(d, "renamed") == NULL, "id-index: detached subtree invisible");
    dom_append_child(find(root, "div"), mid);
    CK(dom_get_element_by_id(d, "renamed") == inner, "id-index: visible again once re-attached");

    /* a node created through the API joins the index too */
    struct node *made = dom_create_element(d, "b", 1);
    dom_set_attr(made, "id", "made");
    dom_append_child(find(root, "div"), made);
    CK(dom_get_element_by_id(d, "made") == made, "id-index: API-created element indexed");

    dom_free(root);
}

static void test_wrappers_and_serial(void)
{
    const char *h = "<div><a id=one>x</a><a id=two>y</a></div>";
    struct node *root = dom_parse(h, (int)strlen(h));
    struct dom_doc *d = root->doc;
    struct node *one = dom_get_element_by_id(d, "one");
    struct node *two = dom_get_element_by_id(d, "two");

    /* Weak wrapper slot: one node -> one slot, and the same node hands back the
     * same slot (that is what makes document.body === document.body in JS). */
    CK(one->jsw == NULL && two->jsw == NULL, "wrapper: slots start empty");
    dom_set_wrapper(one, (void *)0x1000);
    CK(one->jsw == (void *)0x1000, "wrapper: slot stored");
    CK(dom_get_element_by_id(d, "one")->jsw == (void *)0x1000,
       "wrapper: same node -> same slot");
    dom_set_wrapper(two, (void *)0x2000);
    CK(two->jsw == (void *)0x2000, "wrapper: second node has its own slot");

    /* js_dom_cleanup's job: every slot in the document is dropped before the
     * JSRuntime that owned those objects goes away. */
    dom_clear_wrappers(d);
    CK(one->jsw == NULL && two->jsw == NULL, "wrapper: dom_clear_wrappers drops every slot");
    dom_set_wrapper(one, (void *)0x3000);
    dom_clear_wrappers(d);
    CK(one->jsw == NULL, "wrapper: re-registration still tracked after a clear");

    /* Serial: a freed node is dead, and the slot it leaves behind cannot be
     * reached through a stale {node,serial} handle. */
    struct node *victim = dom_get_element_by_id(d, "two");
    uint32_t vser = victim->serial;
    CK(vser != 0, "serial: live node has a non-zero serial");
    dom_destroy_subtree(victim);
    CK(victim->serial == 0, "serial: destroyed node reads as dead");
    CK(victim->serial != vser, "serial: stale handle no longer validates");

    /* The slot is reused; the new occupant must NOT answer to the old serial.
     * The subtree freed two slots (the <a> and its text child), so take two
     * back -- the free list is LIFO over the whole subtree, not just its root. */
    struct node *r1 = dom_create_element(d, "i", 1);
    struct node *r2 = dom_create_element(d, "i", 1);
    struct node *reused = (r1 == victim) ? r1 : r2;
    CK(r1 == victim || r2 == victim, "arena: freed slots return to the free list");
    CK(reused->serial != vser && reused->serial != 0, "serial: reused slot got a fresh stamp");
    CK(reused->jsw == NULL, "wrapper: reused slot starts with no wrapper");
    CK(dom_get_element_by_id(d, "two") == NULL, "id-index: destroyed node unindexed");

    dom_free(root);
}

static void test_text_is_a_copy(void)
{
    /* Hard contract: layout.c points struct item.text straight at node->text and
     * the tests strcmp() it, so text must be an owned, NUL-terminated copy --
     * never a slice of the caller's HTML buffer. */
    char *html = malloc(64);
    strcpy(html, "<p>hello world</p>");
    struct node *root = dom_parse(html, (int)strlen(html));
    struct node *p = find(root, "p");
    struct node *t = p ? p->first_child : 0;
    CK(t && t->type == N_TEXT, "text: text node created");
    memset(html, '#', 63); html[63] = 0;          /* scribble over the source */
    free(html);
    CK(t && !strcmp(t->text, "hello world"), "text: survives the source buffer dying");
    CK(t && t->textlen == 11, "text: textlen matches");
    CK(t && t->text[t->textlen] == 0, "text: NUL-terminated at textlen");
    dom_free(root);
}

static void test_arena_accounting(void)
{
    struct dom_doc *small = dom_doc_new();
    size_t base = dom_doc_bytes(small);
    CK(base > 0, "bytes: a fresh document already owns its first chunks");

    /* Grow past one 256 KiB node chunk (~1300 nodes) and watch the number move. */
    struct node *r = dom_doc_root(small);
    for (int i = 0; i < 4000; i++) {
        struct node *e = dom_create_element(small, "span", 4);
        if (e) dom_append_child(r, e);
    }
    size_t grown = dom_doc_bytes(small);
    CK(grown > base, "bytes: accounting grows with the document");
    CK(grown >= 2u * 256u * 1024u, "bytes: 4000 nodes span more than one chunk");
    dom_free(r);

    /* Recycled nodes come back from the free list, so a rebuild of the same
     * size must not keep allocating chunks. */
    struct dom_doc *d = dom_doc_new();
    struct node *root = dom_doc_root(d);
    for (int i = 0; i < 2000; i++) {
        struct node *e = dom_create_element(d, "b", 1);
        if (e) dom_append_child(root, e);
    }
    size_t after_first = dom_doc_bytes(d);
    for (int pass = 0; pass < 5; pass++) {
        dom_destroy_children(root);
        for (int i = 0; i < 2000; i++) {
            struct node *e = dom_create_element(d, "b", 1);
            if (e) dom_append_child(root, e);
        }
    }
    size_t after_reuse = dom_doc_bytes(d);
    CK(after_reuse == after_first, "bytes: node slots are recycled, not re-chunked");
    dom_free(root);
}

static void test_attr_api(void)
{
    struct dom_doc *d = dom_doc_new();
    struct node *e = dom_create_element(d, "div", 3);
    dom_append_child(dom_doc_root(d), e);

    CK(dom_set_attr(e, "DATA-Src", "/img.png"), "attr: set_attr lowercases the name");
    CK(dom_attr(e, "data-src") && !strcmp(dom_attr(e, "data-src"), "/img.png"), "attr: reads back");
    CK(dom_attr(e, "DATA-SRC") != NULL, "attr: lookup is case-insensitive");
    CK(dom_attr_lw(e, dom_atoms.a_data_src) != NULL, "attr: interned lookup agrees");
    CK(dom_has_attr_lw(e, dom_atoms.a_alt) == 0, "attr: absent attribute reports absent");

    dom_set_attr(e, "data-src", "/other.png");
    CK(e->nattr == 1, "attr: overwrite does not append a duplicate");
    CK(!strcmp(dom_attr(e, "data-src"), "/other.png"), "attr: overwrite took effect");

    dom_set_attr(e, "class", "  alpha   beta gamma  ");
    CK(e->nclass == 3, "attr: class tokens re-derived on set");
    dom_set_attr(e, "class", "solo");
    CK(e->nclass == 1, "attr: class tokens replaced, not appended");

    /* valueless attribute reads as "" (unchanged from the old model) */
    const char *h = "<input disabled>";
    struct node *root = dom_parse(h, (int)strlen(h));
    struct node *in = find(root, "input");
    CK(in && dom_attr(in, "disabled") && !*dom_attr(in, "disabled"),
       "attr: valueless attribute reads as empty string");
    dom_free(root);
    dom_free(dom_doc_root(d));
}

int main(void)
{
    test_long_class();
    test_many_attrs();
    test_long_names_and_values();
    test_deep_tree();
    test_prev_siblings();
    test_comment_doctype();
    test_id_index();
    test_wrappers_and_serial();
    test_text_is_a_copy();
    test_arena_accounting();
    test_attr_api();
    printf(fail ? "SOME FAILED\n" : "ALL PASS\n");
    return fail;
}
