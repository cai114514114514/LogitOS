#ifndef LOGIT_ICONS_H
#define LOGIT_ICONS_H
#include <stdint.h>

/* Procedural monochrome vector icons (M26), drawn via the vg rasterizer. */
enum {
    ICON_FOLDER, ICON_DOC, ICON_TERMINAL, ICON_GRID, ICON_GLOBE,
    ICON_CODE, ICON_CHART, ICON_CLOCK, ICON_IMAGE, ICON_COUNT
};

/* Draw icon `id` at (x,y), `px` pixels square, in `color`, into the current fb
 * target. Rasterized once per (id,px) and cached. */
void icon_draw(int id, int x, int y, int px, uint32_t color);

/* Map an app (.aex file name) to an icon id, or -1 if none. */
int  icon_for_app(const char *name, const char *ext);

#endif /* LOGIT_ICONS_H */
