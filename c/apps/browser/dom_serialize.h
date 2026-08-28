/* dom_serialize.h -- turn a DOM subtree back into text.
 *
 * Two formats, because they answer two different questions:
 *
 *   dom_serialize_test()  the html5lib tree-construction dump format ("| <p>",
 *                         one node per line, attributes sorted).  It is a
 *                         DEBUG format, not markup: it shows the tree, so it
 *                         is what the conformance runner compares and what to
 *                         print when a page renders wrong.
 *
 *   dom_serialize_html()  real HTML, i.e. what innerHTML/outerHTML must
 *                         return.  (js_dom.c still aliases innerHTML to
 *                         textContent; this is the function that fixes it.)
 *
 * Both return a malloc'd NUL-terminated string the caller frees, or NULL on
 * allocation failure.  Neither ever returns a partially built string.
 */
#ifndef DOM_SERIALIZE_H
#define DOM_SERIALIZE_H

#include "dom.h"

/* Serialise the CHILDREN of `root` in the html5lib dump format. Pass the
 * N_DOCUMENT node for a whole document, or the fragment root for a fragment. */
char *dom_serialize_test(const struct node *root);

/* Serialise as HTML markup. include_self == 0 gives innerHTML (children only),
 * 1 gives outerHTML (the node itself included). */
char *dom_serialize_html(const struct node *n, int include_self);

/* getHTML()'s three-way behaviour ("13.2.6.1 ... element serializing steps"
 * with the shadow-serialising steps folded in). The default (no options) and
 * the all-false form MUST equal dom_serialize_html(n,include_self) BYTE FOR
 * BYTE -- shadow-dom/declarative/gethtml.html's own control asserts exactly
 * that identity for every non-shadow element, so the shadow branch below must
 * add NOTHING when both of the following are empty/false, and it does not:
 * dom_serialize_html is defined as this function called with (0,0,0).
 *
 * `roots`/`nroots`: an explicit set of ShadowRoot nodes to serialize
 * regardless of their SHADOW_SERIALIZABLE flag or mode -- Element.getHTML's
 * `shadowRoots` dictionary member. May be NULL/0.
 *
 * `all_serializable`: additionally serialize every descendant shadow root
 * whose OWN SHADOW_SERIALIZABLE flag is set -- the `serializableShadowRoots`
 * option. The explicit list and this flag are independent: a root in `roots[]`
 * is serialized even when this is false and even when the root is CLOSED --
 * the explicit list overrides both, per spec. */
char *dom_serialize_html_opt(const struct node *n, int include_self,
                             const struct node *const *roots, int nroots,
                             int all_serializable);

#endif /* DOM_SERIALIZE_H */
