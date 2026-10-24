#ifndef LOGIT_CSS_SIZING_H
#define LOGIT_CSS_SIZING_H
const char *css_sizing_property(int index);

struct cstyle;

/* Unlike LibCSS's old initial encodings, Grid needs normal distinct from
 * explicit stretch/start. These patches use the existing extension cascade;
 * 255 is private until its final parent-first inheritance resolution. */
enum { CSS_SIZING_PARENT = 255 };
struct css_sizing_patch {
    unsigned char ratio_set, justify_set, items_set, self_set;
    double width, height;
    unsigned char automatic;
    unsigned char has_ratio;
    int justify, items, self;
};

int css_sizing_declaration(const char *property, int plen,
                           const char *value, int vlen,
                           struct css_sizing_patch *out);
int css_sizing_supports(const char *property, int plen,
                       const char *value, int vlen);
void css_sizing_merge(struct css_sizing_patch *to,
                      const struct css_sizing_patch *from);
void css_sizing_apply(struct cstyle *style, const struct css_sizing_patch *patch);
void css_sizing_resolve(struct cstyle *style, const struct cstyle *parent);

#endif
