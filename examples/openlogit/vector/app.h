#ifndef VECTOR_STUDIO_H
#define VECTOR_STUDIO_H
#include "openlogit_scene2d.h"
#include "openlogit_ui.h"
#include "openlogit_draw.h"
#include "openlogit_window.h"

enum control {
    PAGE_VECTOR,
    PAGE_SPRITES,
    PAGE_LAYERS,
    PLAY,
    CLIP,
    DASH,
    VARIANT,
    RESET,
    AMOUNT,
    CONTROL_COUNT
};
struct bitmap {
    struct ol_surface *surface;
    void *storage;
    unsigned char *front, *work;
    unsigned w, h;
};
struct vector_app {
    struct ol_device *device;
    struct ol_list *list;
    void *device_storage, *list_storage, *transfer;
    struct bitmap frame, background, toolbar, inspector, stage, card;
    struct ol_image *atlas, *mask;
    void *atlas_storage, *mask_storage;
    unsigned char *atlas_pixels, *mask_pixels;
    struct ol_scene2d scene;
    struct ol_ui ui;
    struct ol_ui_feedback feedback[CONTROL_COUNT];
    struct ol_timeline timeline;
    float phase, amount;
    int page, playing, clipped, dashed, variant, reduced, dragging, drag_x, drag_y;
    int card_x, card_y, toolbar_dirty, inspector_dirty, art_dirty;
    unsigned frames;
};
int bitmap_create(struct vector_app *, struct bitmap *, unsigned, unsigned);
void bitmap_destroy(struct bitmap *);
int artwork_init(struct vector_app *);
int artwork_stage(struct vector_app *);
void artwork_destroy(struct vector_app *);
void controls_init(struct vector_app *, uint64_t);
void controls_action(struct vector_app *, unsigned, uint64_t);
void controls_event(struct vector_app *, const struct logit_event *, uint64_t);
int controls_update(struct vector_app *, uint64_t);
int controls_draw(struct vector_app *);
void controls_labels(struct vector_app *, struct gfx_rect);
#endif
