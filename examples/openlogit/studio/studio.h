#ifndef OPENLOGIT_STUDIO_H
#define OPENLOGIT_STUDIO_H

#include "openlogit_lsl.h"
#include "openlogit_scene.h"
#include "openlogit_ui.h"
#include "openlogit_window.h"

enum studio_control {
    TAB_SCENE,
    TAB_MOTION,
    TAB_MATERIAL,
    OBJECT_SPHERE,
    OBJECT_TORUS,
    OBJECT_RIG,
    PLAY,
    RESET,
    HELP,
    SHADOW,
    TEXTURE,
    TOON,
    LIGHT,
    GLOSS,
    BLEND,
    TIMELINE,
    ZOOM_IN,
    ZOOM_OUT,
    CAMERA_LEFT,
    CAMERA_RIGHT,
    QUALITY,
    CLOSE_DIALOG,
    CONTROL_COUNT
};
struct studio_image {
    struct ol_surface *surface;
    void *storage;
    unsigned char *front, *work;
    unsigned width, height;
};
struct studio_object {
    struct ol3d_mesh mesh;
    struct ol3d_buffer *vertices, *indices;
    float model[16];
};
struct studio {
    struct ol_device *device;
    struct ol_list *list;
    void *device_storage, *list_storage;
    struct studio_image frame, scene, texture;
    struct ol3d_context *renderer;
    struct ol3d_pipeline_object *lit_pipeline, *flat_pipeline;
    struct studio_object objects[4];
    struct ol3d_vertex *deformed;
    struct ol3d_skin_weights *weights;
    struct ol3d_orbit camera;
    struct ol_ui ui;
    struct ol_ui_feedback feedback[CONTROL_COUNT];
    struct ol_timeline motion, notice;
    struct ol_spring dialog_motion, tab_motion;
    float view_projection[16], eye[3], uniforms[17][4];
    float phase, light, gloss, blend, timeline, dialog_amount, tab_amount, notice_amount;
    int selected, tab, playing, shadows, textured, toon, low, modal, reduced;
    int dirty, scene_dirty, ui_active, orbiting, pointer_x, pointer_y;
    unsigned frames, scene_frames;
    uint64_t render_ns;
};

extern const struct gfx_rect studio_viewport;
int studio_image_create(struct studio *studio, struct studio_image *image, unsigned w, unsigned h);
void studio_image_destroy(struct studio_image *image);
int studio_scene_init(struct studio *studio);
int studio_scene_resize(struct studio *studio, int low);
int studio_scene_render(struct studio *studio);
void studio_scene_destroy(struct studio *studio);
int studio_pick(struct studio *studio, int x, int y);
void studio_ui_init(struct studio *studio, uint64_t now);
void studio_ui_sample(struct studio *studio, uint64_t now);
void studio_action(struct studio *studio, unsigned action, uint64_t now);
void studio_adjust(struct studio *studio, int direction, uint64_t now);
void studio_pointer(struct studio *studio, const struct logit_event *event, uint64_t now);
int studio_ui_draw(struct studio *studio);

#endif
