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

/* Same two entry points, with Declarative Shadow DOM's opt-in made explicit
 * instead of implied. html_parse()/html_parse_fragment() are
 * html_parse_ex(...,0)/html_parse_fragment_ex(...,0): a <template
 * shadowrootmode> anywhere in the source parses as an ORDINARY, inert
 * <template> -- which is the spec-correct default for innerHTML= and
 * DOMParser.parseFromString (they must NOT attach a declarative shadow root),
 * and is also, today, the ONLY behaviour this tree has, because nothing yet
 * calls the ",1" form.
 *
 * Per spec the opt-in should default ON for a whole document parse (the
 * browser's real page load) and for setHTMLUnsafe()/parseHTMLUnsafe(). Wiring
 * that call is deliberately NOT done here: it is browser.c's / js_dom.c's
 * call to make (which document-parse call sites opt in), and js_dom.c is
 * owned by another agent editing it concurrently -- see
 * CLAUDE.md's shadow-dom triage, cluster C3, for the exact call this needs. */
struct node *html_parse_ex(struct dom_doc **out_doc, const char *src, int len,
                           int allow_declarative_shadow);
struct node *html_parse_fragment_ex(struct dom_doc **out_doc, const char *src, int len,
                                    const char *context, int ctxlen, int ctx_ns,
                                    int allow_declarative_shadow);

#endif /* HTML_TREE_H */
