#ifndef CSS_TRANSFORM_CONTEXT_H
#define CSS_TRANSFORM_CONTEXT_H
#include "dom.h"
#include "css.h"
#include "css_interp.h"
#include "../../../include/weaksym.h"

int ci_transform_parse_context(const char *,int,const struct ci_length_context *,
                               struct ci_xform *) LOGIT_WEAK;
LOGIT_WEAK_STUB(ci_transform_parse_context);

/* The root belongs to this document, including passive child documents and
 * detached elements. The currently active page's global font metrics are not
 * a valid rlh basis for another owner. No borrowed style pointer is retained. */
static inline struct ci_length_context css_transform_lengths(const struct node *n)
{
    const struct cstyle *s=n?n->style:0;
    const struct node *root=n&&n->doc?dom_doc_element(n->doc):0;
    const struct cstyle *rs=root?root->style:0;
    struct ci_length_context c={s&&s->font_px>0?s->font_px:16,
        rs&&rs->font_px>0?rs->font_px:16,css_used_line_px(s),css_used_line_px(rs)};
    return c;
}

static inline int css_node_transform_parse(const struct node *n,
        const char *value,int len,struct ci_xform *out)
{
    if(!LOGIT_HAVE(ci_transform_parse_context))return -1;
    struct ci_length_context c=css_transform_lengths(n);
    return ci_transform_parse_context(value,len,&c,out);
}
#endif
