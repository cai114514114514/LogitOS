/* html_tree.h -- HTML5 tree construction (WHATWG "13.2.6 Tree construction").
 *
 * The stage above html_tokenizer.c: tokens in, a DOM out.  Where the tokenizer
 * is mechanical, this is the layer that encodes twenty years of "what do real
 * browsers do with broken markup", and the spec's answer to almost every
 * question here is an explicit algorithm rather than a principle.  So the file
 * follows the spec's structure literally -- one function per insertion mode,
 * the sets written as tables, the adoption agency algorithm step by numbered
 * step -- because a tree builder that is "morally equivalent" to the spec is a
 * tree builder whose bugs can only be found by rendering somebody's page and
 * squinting at it.
 *
 * The measurement is `make test-html5lib` (html5lib-tests' tree-construction
 * corpus, the suite every browser is scored on).
 *
 * This does NOT replace dom_parse() yet.  Switching the browser over moves
 * every layout test's expected geometry, so it is deliberately a separate
 * change; until then the legacy scanner stays in dom.c and this is reachable
 * through html_parse().
 */
#ifndef HTML_TREE_H
#define HTML_TREE_H

#include "dom.h"

/* Parse a whole document.  Returns the N_DOCUMENT node (whose children are the
 * doctype, comments and the html element) and, through *out_doc, the document
 * that owns every node -- free with dom_free(dom_doc_root(*out_doc)).
 * Returns NULL only if the document could not be allocated at all. */
struct node *html_parse(struct dom_doc **out_doc, const char *src, int len);

/* The HTML fragment parsing algorithm: parse `src` as if it were the contents
 * of `context` (innerHTML=).  `ctx_ns` is NS_HTML / NS_SVG / NS_MATHML.
 *
 * Returns the synthetic <html> root element whose CHILDREN are the fragment --
 * the caller adopts those, not the root.  *out_doc owns everything, as above.
 */
struct node *html_parse_fragment(struct dom_doc **out_doc, const char *src, int len,
                                 const char *context, int ctxlen, int ctx_ns);

#endif /* HTML_TREE_H */
