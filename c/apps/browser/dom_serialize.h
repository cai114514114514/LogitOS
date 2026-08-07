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

#endif /* DOM_SERIALIZE_H */
