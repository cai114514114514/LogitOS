#ifndef BROWSER_CSS_BACKGROUND_H
#define BROWSER_CSS_BACKGROUND_H

/* Values stay separate from image bytes. Percent positions use the remaining
 * space (positioning area minus image size), not the area's full width. */
struct bg_length { double px, percent; int automatic; };
enum { BG_EXPLICIT, BG_COVER, BG_CONTAIN };
enum { BG_NO_REPEAT, BG_REPEAT, BG_SPACE, BG_ROUND };
enum { BG_BORDER_BOX, BG_PADDING_BOX, BG_CONTENT_BOX };
enum { BG_NONE, BG_URL, BG_GRADIENT };
enum { BG_IMAGE, BG_SIZE, BG_POSITION, BG_REPEAT_STYLE, BG_ORIGIN, BG_CLIP,
       BG_ATTACHMENT, BG_COLOR, BG_SLOTS };
/* A resource ceiling refuses the entire declaration, never its tail. This
 * count is shared by parsing, resource discovery and bounded paint storage. */
#define BG_MAX_LAYERS 16
struct bg_geometry {
    struct bg_length size[2], position[2];
    int mode, repeat[2];
};
struct cstyle;
struct bg_span { const char *value; int len; };
struct bg_layer {
    struct bg_span image;
    struct bg_geometry geometry;
    int kind, origin, clip, attachment;
};
struct bg_layers {
    int count;
    struct bg_layer layer[BG_MAX_LAYERS];
    struct bg_span color;
};
/* A shorthand publishes all slots atomically. Each span is the ORIGINAL
 * shorthand, with a provenance bit telling selection to extract that slot.
 * Concatenating its non-contiguous parts into temporary strings would leave
 * the extension cascade holding stack pointers after this call returns. */
struct bg_declaration {
    const char *value[BG_SLOTS]; int len[BG_SLOTS];
    unsigned shorthand_bits;
};
int css_background_declaration(const char *property,int plen,const char *value,int len,
                               struct bg_declaration *);
int css_background_url(const char *value, int len, char *url, int capacity);
int css_background_url_token(const char *value,int len,int *consumed,char *url,int capacity);
int css_background_geometry(const struct cstyle *, struct bg_geometry *);
int css_background_layers(const struct cstyle *, struct bg_layers *);
int css_background_color(const struct cstyle *, unsigned *rgb, int *alpha);
int css_background_slot(int xr);
int css_background_xr(int slot);
int css_background_supports(const char *property, int plen, const char *value, int len);

#endif
