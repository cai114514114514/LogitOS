#ifndef LOGIT_ICONS_H
#define LOGIT_ICONS_H
#include <stdint.h>

/* Procedural monochrome vector icons (M26), drawn via the vg rasterizer. */
enum {
    ICON_FOLDER, ICON_DOC, ICON_TERMINAL, ICON_GRID, ICON_GLOBE,
    ICON_CODE, ICON_CHART, ICON_CLOCK, ICON_IMAGE,
    ICON_PHOTOS, ICON_SLIDERS,
    /* set A: core UI symbols (stroked). Ring-3 twins are GICON_* in c/apps/logit.h. */
    ICON_CHEVRON_L, ICON_CHEVRON_R, ICON_CHEVRON_U, ICON_CHEVRON_D,
    ICON_CLOSE, ICON_PLUS, ICON_MINUS, ICON_CHECK,
    ICON_SEARCH, ICON_SIDEBAR, ICON_ELLIPSIS, ICON_GEAR,
    /* set B: toolbar/sidebar/menu symbols (stroke and fill, 0..100 box). Twins
     * GICON_SHARE..GICON_BELL in c/apps/logit.h hold the same values. */
    ICON_SHARE, ICON_PLAY, ICON_PAUSE, ICON_TRASH,
    ICON_ARROW_L, ICON_ARROW_R, ICON_ARROW_U, ICON_ARROW_D,
    ICON_INFO, ICON_WARNING, ICON_DOWNLOAD, ICON_REFRESH,
    ICON_STAR, ICON_LOCK, ICON_HOME, ICON_LINK, ICON_EYE, ICON_BELL,
    ICON_COUNT
};

/* Draw icon `id` at (x,y), `px` pixels square, in `color`, into the current fb
 * target. Rasterized once per (id,px) and cached. */
void icon_draw(int id, int x, int y, int px, uint32_t color);

/* As icon_draw with a stroke weight 1..3 (thin/regular/bold; clamped). Fill-only
 * icons ignore it. Cached per (id, device px, weight) in a 64-entry LRU. */
void icon_draw_w(int id, int x, int y, int px, uint32_t color, int weight);
/* Cache counters (for tests): lookups that hit / missed, live entries (<= 64). */
void icon_cache_stats(unsigned *hits, unsigned *misses, unsigned *entries);

/* Map an app (.aex file name) to an icon id, or -1 if none. */
int  icon_for_app(const char *name, const char *ext);
/* Full-colour disk artwork; 1 drawn, 0 absent (caller keeps its fallback). */
int app_icon_draw(const char *name, int x, int y, int px);

#endif /* LOGIT_ICONS_H */
