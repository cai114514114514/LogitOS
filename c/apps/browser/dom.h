#ifndef LOGIT_DOM_H
#define LOGIT_DOM_H

#include <stdint.h>

enum { N_ELEM, N_TEXT };

struct attr { char name[32]; char val[256]; };

struct node {
    int type;                       /* N_ELEM / N_TEXT */
    char tag[16];                   /* lowercased tag name (N_ELEM) */
    struct attr *attrs; int nattr;
    char *text; int textlen;        /* N_TEXT (NUL-terminated) */
    struct node *parent, *first_child, *last_child, *next;
    void *style;                    /* struct cstyle* (filled by css) */
    char *raw; int rawlen;          /* <svg>: verbatim source span (start '<' to
                                     * matching "</svg>"), kmalloc'd copy; the DOM
                                     * attrs lowercase viewBox and truncate long
                                     * path data, so layout decodes from this. */
};

/* Parse an HTML document into a DOM tree; returns the root element (a synthetic
 * "#document" element whose children are the top-level nodes), or NULL. */
struct node *dom_parse(const char *html, int len);

/* Attribute value by (case-insensitive) name, or NULL. */
const char *dom_attr(const struct node *n, const char *name);

void dom_free(struct node *root);

#endif /* LOGIT_DOM_H */
