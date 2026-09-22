/* Inline SVG paint properties absent from LibCSS's property table. Keep the
 * capture names, inheritance and renderer defaults together: a new spelling
 * must not silently be accepted by the cascade but lost at rasterisation. */
#ifndef LOGIT_SVG_STYLE_PROPS_H
#define LOGIT_SVG_STYLE_PROPS_H
#define CSS_SVG_PAINT_PROPERTIES(X) \
 X(FILL, "fill", "black", 1) \
 X(STROKE, "stroke", "none", 1) \
 X(FILL_RULE, "fill-rule", "nonzero", 1) \
 X(CLIP_RULE, "clip-rule", "nonzero", 1) \
 X(STROKE_WIDTH, "stroke-width", "1", 1) \
 X(FILL_OPACITY, "fill-opacity", "1", 1) \
 X(STROKE_OPACITY, "stroke-opacity", "1", 1) \
 X(CLIP_PATH, "clip-path", "none", 0)
#endif
