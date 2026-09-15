#include "../example_log.h"
#include "studio.h"
#include <stdio.h>
#include <string.h>

static const struct gfx_rect bounds[CONTROL_COUNT] = {
    [TAB_SCENE] = {208, 66, 164, 30},
    [TAB_MOTION] = {380, 66, 164, 30},
    [TAB_MATERIAL] = {552, 66, 176, 30},
    [OBJECT_SPHERE] = {28, 162, 156, 38},
    [OBJECT_TORUS] = {28, 208, 156, 38},
    [OBJECT_RIG] = {28, 254, 156, 38},
    [PLAY] = {220, 502, 88, 30},
    [RESET] = {760, 502, 80, 30},
    [HELP] = {848, 502, 80, 30},
    [SHADOW] = {878, 185, 48, 24},
    [TEXTURE] = {878, 231, 48, 24},
    [TOON] = {878, 277, 48, 24},
    [LIGHT] = {764, 348, 160, 22},
    [GLOSS] = {764, 400, 160, 22},
    [BLEND] = {764, 452, 160, 22},
    [TIMELINE] = {328, 506, 380, 22},
    [ZOOM_IN] = {28, 340, 70, 30},
    [ZOOM_OUT] = {110, 340, 70, 30},
    [CAMERA_LEFT] = {28, 380, 70, 30},
    [CAMERA_RIGHT] = {110, 380, 70, 30},
    [QUALITY] = {28, 424, 152, 30},
    [CLOSE_DIALOG] = {418, 367, 124, 34}
};

static void reset_camera(struct studio *s)
{
    s->camera = (struct ol3d_orbit){.target = {0, -.1f, 0},
                                    .yaw = .45f,
                                    .pitch = .35f,
                                    .distance = 5.8f,
                                    .fovy = .8f,
                                    .aspect = 520.f / 364.f,
                                    .near_z = .1f,
                                    .far_z = 50};
}

static int control_visible(const struct studio *s, unsigned id)
{
    if (id == SHADOW || id == LIGHT)
        return s->tab == TAB_SCENE;
    if (id == TEXTURE || id == TOON || id == GLOSS)
        return s->tab == TAB_MATERIAL;
    if (id == BLEND)
        return s->tab == TAB_MOTION;
    return id != CLOSE_DIALOG;
}

static void sync_controls(struct studio *s)
{
    for (unsigned id = 0; id < CONTROL_COUNT; id++)
        ol_ui_enable(&s->ui, id, s->modal ? id == CLOSE_DIALOG : control_visible(s, id));
}

void studio_ui_init(struct studio *s, uint64_t now)
{
    static const struct ol_keyframe loop[] = {{.offset = 0, .value = {0}},
                                              {.offset = 1, .value = {1}}};
    static const struct ol_keyframe toast[] = {
        {.offset = 0, .value = {0}, .easing = {.kind = OL_EASE_OUT}},
        {.offset = .15, .value = {1}},
        {.offset = .75, .value = {1}},
        {.offset = 1, .value = {0}}};
    struct ol_animation_desc desc = {sizeof desc, OL_SCALAR,     2, OL_FORWARD,
                                     0,           6000000000ULL, 0, loop};
    ol_ui_init(&s->ui, now);
    for (unsigned id = 0; id < CONTROL_COUNT; id++)
        ol_ui_define(&s->ui, id, bounds[id]);
    ol_animation_init(&s->motion, &desc, now);
    ol_animation_pause(&s->motion, now);
    ol_animation_seek(&s->motion, now, 900000000);
    desc.count = 4;
    desc.iterations = 1;
    desc.duration_ns = 1800000000;
    desc.keys = toast;
    ol_animation_init(&s->notice, &desc, now);
    ol_animation_pause(&s->notice, now);
    ol_animation_seek(&s->notice, now, desc.duration_ns);
    ol_spring_init(&s->dialog_motion, 0, 0, 0, 5, 1, now, 350000000);
    ol_spring_init(&s->tab_motion, 0, 0, 0, 5, 1, now, 320000000);
    s->shadows = s->textured = 1;
    s->light = .65f;
    s->gloss = .45f;
    s->blend = 1;
    s->phase = s->timeline = .15f;
    reset_camera(s);
    sync_controls(s);
    s->dirty = s->scene_dirty = 1;
}

static void pause_motion(struct studio *s, uint64_t now)
{
    s->playing = 0;
    ol_animation_pause(&s->motion, now);
}

void studio_action(struct studio *s, unsigned action, uint64_t now)
{
    if (s->modal && action != CLOSE_DIALOG)
        return;
    if (action <= TAB_MATERIAL) {
        s->tab = (int)action;
        ol_spring_retarget(&s->tab_motion, now, (float)s->tab);
    } else if (action >= OBJECT_SPHERE && action <= OBJECT_RIG) {
        s->selected = action - OBJECT_SPHERE;
        s->scene_dirty = 1;
    } else
        switch (action) {
        case PLAY:
            if (s->playing)
                pause_motion(s, now);
            else if (!s->reduced) {
                s->playing = 1;
                ol_animation_play(&s->motion, now);
            }
            break;
        case RESET:
            pause_motion(s, now);
            ol_animation_seek(&s->motion, now, 900000000);
            reset_camera(s);
            s->light = .65f;
            s->gloss = .45f;
            s->blend = 1;
            s->shadows = s->textured = 1;
            s->toon = 0;
            s->scene_dirty = 1;
            ol_animation_seek(&s->notice, now, 0);
            ol_animation_play(&s->notice, now);
            break;
        case HELP:
            s->modal = 1;
            break;
        case CLOSE_DIALOG:
            s->modal = 0;
            break;
        case SHADOW:
            s->shadows = !s->shadows;
            s->scene_dirty = 1;
            break;
        case TEXTURE:
            s->textured = !s->textured;
            s->scene_dirty = 1;
            break;
        case TOON:
            s->toon = !s->toon;
            s->scene_dirty = 1;
            break;
        case ZOOM_IN:
            s->camera.distance -= .5f;
            s->scene_dirty = 1;
            break;
        case ZOOM_OUT:
            s->camera.distance += .5f;
            s->scene_dirty = 1;
            break;
        case CAMERA_LEFT:
            s->camera.yaw -= .25f;
            s->scene_dirty = 1;
            break;
        case CAMERA_RIGHT:
            s->camera.yaw += .25f;
            s->scene_dirty = 1;
            break;
        case QUALITY:
            if (!studio_scene_resize(s, !s->low))
                example_log("STUDIO ERROR resize\n");
            break;
        default:
            break;
        }
    if (s->camera.distance < 3)
        s->camera.distance = 3;
    if (s->camera.distance > 12)
        s->camera.distance = 12;
    ol_spring_retarget(&s->dialog_motion, now, s->modal ? 1 : 0);
    s->dirty = 1;
    sync_controls(s);
    example_log("STUDIO ACTION id=%u playing=%d shadow=%d texture=%d toon=%d selected=%d\n", action,
                s->playing, s->shadows, s->textured, s->toon, s->selected);
}

void studio_adjust(struct studio *s, int direction, uint64_t now)
{
    if (s->modal)
        return;
    int id = s->ui.focused;
    float *value = id == LIGHT      ? &s->light
                   : id == GLOSS    ? &s->gloss
                   : id == BLEND    ? &s->blend
                   : id == TIMELINE ? &s->timeline
                                    : NULL;
    if (!value) {
        studio_action(s, direction < 0 ? CAMERA_LEFT : CAMERA_RIGHT, now);
        return;
    }
    *value += direction * .05f;
    if (*value < 0)
        *value = 0;
    if (*value > 1)
        *value = 1;
    if (id == TIMELINE) {
        pause_motion(s, now);
        ol_animation_seek(&s->motion, now, *value * 5999999000.0);
    }
    s->dirty = s->scene_dirty = 1;
}

static int in_view(int x, int y)
{
    return x >= 208 && x < 728 && y >= 108 && y < 472;
}

void studio_pointer(struct studio *s, const struct logit_event *event, uint64_t now)
{
    int x = event->a, y = event->b;
    if (s->modal)
        s->orbiting = 0;
    if (!s->modal &&
        (event->type == EV_MOUSE_R || (event->type == EV_MOUSE && event->button == EV_BTN_RIGHT))) {
        s->orbiting = in_view(x, y);
        s->pointer_x = x;
        s->pointer_y = y;
        return;
    }
    if (event->type == EV_MOUSE_UP && event->button == EV_BTN_RIGHT) {
        s->orbiting = 0;
        return;
    }
    if (event->type == EV_MOUSE_MOVE && s->orbiting) {
        s->camera.yaw += (x - s->pointer_x) * .008f;
        s->camera.pitch += (y - s->pointer_y) * .006f;
        if (s->camera.pitch < .05f)
            s->camera.pitch = .05f;
        if (s->camera.pitch > 1.3f)
            s->camera.pitch = 1.3f;
        s->pointer_x = x;
        s->pointer_y = y;
        s->scene_dirty = s->dirty = 1;
        return;
    }
    if (event->type == EV_WHEEL && !s->modal) {
        studio_action(s, event->wheel > 0 ? ZOOM_OUT : ZOOM_IN, now);
        return;
    }
    int type = event->type == EV_MOUSE      ? OL_UI_DOWN
               : event->type == EV_MOUSE_UP ? OL_UI_UP
                                            : OL_UI_MOVE;
    struct ol_ui_event input = {type, x, y};
    ol_ui_event(&s->ui, &input);
    if (type == OL_UI_DOWN && !s->modal && in_view(x, y)) {
        int hit = studio_pick(s, x, y);
        if (hit >= 0)
            studio_action(s, OBJECT_SPHERE + hit, now);
    }
    for (unsigned id = 0; id < CONTROL_COUNT; id++)
        if (ol_ui_take_activation(&s->ui, id))
            studio_action(s, id, now);
    if (ol_ui_slider_value(&s->ui, LIGHT, &s->light) ||
        ol_ui_slider_value(&s->ui, GLOSS, &s->gloss) ||
        ol_ui_slider_value(&s->ui, BLEND, &s->blend))
        s->scene_dirty = s->dirty = 1;
    if (ol_ui_slider_value(&s->ui, TIMELINE, &s->timeline)) {
        pause_motion(s, now);
        /* An infinite timeline wraps at exactly its duration. Keep the right
         * endpoint in this iteration so scrubbing does not jump to zero. */
        ol_animation_seek(&s->motion, now, s->timeline * 5999999000.0);
        s->scene_dirty = s->dirty = 1;
    }
}

void studio_ui_sample(struct studio *s, uint64_t now)
{
    struct ol_anim_sample motion, notice;
    if (s->reduced && s->playing)
        pause_motion(s, now);
    ol_animation_sample(&s->motion, now, 0, &motion);
    if (s->phase != motion.value[0])
        s->scene_dirty = 1;
    s->phase = s->timeline = motion.value[0];
    ol_animation_sample(&s->notice, now, s->reduced, &notice);
    s->notice_amount = notice.value[0];
    int dialog_active = 0, tab_active = 0;
    s->dialog_amount = s->reduced ? (float)s->modal
                                  : ol_spring_sample(&s->dialog_motion, now, NULL, &dialog_active);
    s->tab_amount =
        s->reduced ? (float)s->tab : ol_spring_sample(&s->tab_motion, now, NULL, &tab_active);
    for (unsigned id = 0; id < CONTROL_COUNT; id++) {
        int enabled = s->modal ? (id == CLOSE_DIALOG) : control_visible(s, id);
        ol_ui_enable(&s->ui, id, enabled);
        if (!enabled)
            continue;
        float target = 0;
        if (id == SHADOW)
            target = s->shadows;
        if (id == TEXTURE)
            target = s->textured;
        if (id == TOON)
            target = s->toon;
        if (id == LIGHT)
            target = s->light;
        if (id == GLOSS)
            target = s->gloss;
        if (id == BLEND)
            target = s->blend;
        if (id == TIMELINE)
            target = s->timeline;
        ol_ui_feedback(&s->ui, id, target, &s->feedback[id]);
    }
    s->ui_active = s->ui.active || dialog_active || tab_active || notice.active;
    s->dirty |= s->ui.dirty;
}

static void label(int x, int y, int size, unsigned rgb, const char *text)
{
    ol_window_text_run(x, y, size, 0, rgb, text, (int)strlen(text));
}

static int layer(struct studio *s, struct studio_image *image, struct gfx_rect rect)
{
    struct ol_layer_options options = {.size = sizeof options, .opacity = 255, .bilinear = 1};
    return ol_cmd_layer(s->list, image->surface, rect, &options);
}

int studio_ui_draw(struct studio *s)
{
    if (ol_cmd_clear(s->list, 0x101a29, 255) != OL_OK)
        return 0;
    ol_ui_panel(s->list, (struct gfx_rect){16, 108, 180, 364}, 0x1a2b3c, 255);
    ol_ui_panel(s->list, (struct gfx_rect){744, 108, 200, 380}, 0x1a2b3c, 255);
    ol_ui_panel(s->list, (struct gfx_rect){208, 490, 520, 90}, 0x1a2b3c, 255);
    if (layer(s, &s->scene, studio_viewport) != OL_OK)
        return 0;
    for (unsigned id = 0; id < CONTROL_COUNT - 1; id++) {
        if (!control_visible(s, id))
            continue;
        int selected = id <= TAB_MATERIAL ? (id == (unsigned)s->tab)
                       : id >= OBJECT_SPHERE && id <= OBJECT_RIG
                           ? (id == OBJECT_SPHERE + (unsigned)s->selected)
                           : 0;
        if (id >= SHADOW && id <= TOON)
            ol_ui_toggle(s->list, bounds[id], &s->feedback[id]);
        else if (id >= LIGHT && id <= TIMELINE)
            ol_ui_slider(s->list, bounds[id], &s->feedback[id]);
        else
            ol_ui_button(s->list, bounds[id], &s->feedback[id], selected);
    }
    ol_ui_panel(s->list, (struct gfx_rect){208 + (int)(s->tab_amount * 172), 98, 164, 3}, 0x6adbc2,
                255);
    int toast_y = 548 + (int)((1 - s->notice_amount) * 20);
    if (s->notice_amount > .01f)
        ol_ui_panel(s->list, (struct gfx_rect){754, toast_y, 180, 28}, 0x285748,
                    (unsigned)(s->notice_amount * 255));
    int dialog_y = 200 + (int)((1 - s->dialog_amount) * 32);
    if (s->dialog_amount > .01f) {
        ol_ui_panel(s->list, (struct gfx_rect){0, 0, 960, 600}, 0x08121e,
                    (unsigned)(s->dialog_amount * 195));
        ol_ui_panel(s->list, (struct gfx_rect){270, dialog_y, 420, 220}, 0x29445a, 255);
        ol_ui_button(s->list, bounds[CLOSE_DIALOG], &s->feedback[CLOSE_DIALOG], 1);
    }
    if (ol_list_close(s->list) != OL_OK ||
        ol_submit(s->device, s->list, s->frame.surface, NULL) != OL_OK ||
        ol_window_composite(s->frame.surface, 0, 0, 960, 600) != OL_OK)
        return 0;
    label(24, 20, 26, 0xf0f5fa, "OpenLogit / Scene Studio");
    label(26, 60, 12, 0x93aabd, "LSL vertex + fragment");
    const char *tabs[] = {"Scene", "Motion", "Material"};
    for (unsigned i = 0; i < 3; i++)
        label(bounds[i].x + 18, 74, 14, 0xe1edf5, tabs[i]);
    label(28, 125, 15, 0xe1edf5, "Scene objects");
    const char *objects[] = {"01  Sphere", "02  Torus", "03  Skinned tube"};
    for (unsigned i = 0; i < 3; i++)
        label(38, 174 + (int)i * 46, 13, 0xe1edf5, objects[i]);
    label(28, 316, 12, 0x93aabd, "Orbit camera");
    label(43, 348, 12, 0xe1edf5, "Zoom +");
    label(122, 348, 12, 0xe1edf5, "Zoom -");
    label(44, 388, 12, 0xe1edf5, "Left");
    label(127, 388, 12, 0xe1edf5, "Right");
    label(40, 432, 12, 0xe1edf5, s->low ? "Quality: 160 x 112" : "Quality: 320 x 224");
    label(760, 126, 16, 0xe1edf5, tabs[s->tab]);
    const char *details[] = {"Pick / orbit / inspect", "Two-bone pose blend",
                             "Vertex lighting + fog"};
    label(760, 153, 11, 0x93aabd, details[s->tab]);
    char text[128];
    if (s->tab == TAB_SCENE) {
        label(760, 190, 13, 0xe1edf5, "Shadow");
        snprintf(text, sizeof text, "%u vertices / %u triangles",
                 (unsigned)s->objects[s->selected].mesh.vertex_count,
                 (unsigned)s->objects[s->selected].mesh.index_count / 3);
        label(760, 242, 11, 0xb7cddd, text);
        label(760, 272, 12, 0xb7cddd, "Triangle ray picking");
        snprintf(text, sizeof text, "Light position  %d%%", (int)(s->light * 100));
        label(764, 326, 12, 0xb7cddd, text);
        label(760, 402, 12, 0xb7cddd, "Planar projected shadow");
        label(760, 432, 12, 0xb7cddd, "Depth-tested geometry");
    } else if (s->tab == TAB_MOTION) {
        label(760, 196, 13, 0xe1edf5, "Root > bend joint");
        label(760, 236, 12, 0xb7cddd, "Rest pose + animated pose");
        label(760, 270, 12, 0xb7cddd, "Quaternion interpolation");
        label(760, 310, 12, 0xb7cddd, "Drag the timeline to seek");
        label(760, 346, 12, 0xb7cddd, "Space to pause / resume");
        snprintf(text, sizeof text, "Pose blend  %d%%", (int)(s->blend * 100));
        label(764, 430, 12, 0xb7cddd, text);
    } else {
        label(760, 190, 12, 0xb7cddd, "Bilinear repeated sampler");
        label(760, 236, 13, 0xe1edf5, "Texture");
        label(760, 282, 13, 0xe1edf5, "Toon bands");
        label(760, 326, 12, 0xb7cddd, "Distance fog");
        snprintf(text, sizeof text, "Specular gain  %d%%", (int)(s->gloss * 100));
        label(764, 378, 12, 0xb7cddd, text);
        label(760, 452, 12, 0xb7cddd, "LSL vertex / fragment");
    }
    label(240, 510, 13, 0xe1edf5, s->playing ? "Pause" : "Play");
    label(780, 510, 13, 0xe1edf5, "Reset");
    label(870, 510, 13, 0xe1edf5, "Help");
    snprintf(text, sizeof text, "%s  /  %.2f s  /  render %u ms  /  %u scene frames",
             s->reduced   ? "Reduced motion"
             : s->playing ? "Playing"
                          : "Paused",
             (double)s->phase * 6, (unsigned)(s->render_ns / 1000000), s->scene_frames);
    label(224, 546, 12, 0xa8c5d1, text);
    label(24, 584, 11, 0x93aabd,
          "Space play   S shadow   T texture   C toon   1/2/3 select   H help   Tab / Enter   "
          "Right-drag orbit");
    if (s->notice_amount > .4f)
        label(778, toast_y + 7, 12, 0xe0fff0, "Scene reset");
    if (s->dialog_amount > .01f) {
        label(294, dialog_y + 22, 22, 0xffffff, "Explore the scene");
        label(294, dialog_y + 65, 13, 0xd5e5ef, "Click a mesh to select. Right-drag to orbit.");
        label(294, dialog_y + 93, 13, 0xd5e5ef, "Scrub the timeline to inspect a bone pose.");
        label(294, dialog_y + 121, 13, 0xd5e5ef, "Light and blend sliders update the real scene.");
        label(444, 378, 13, 0xffffff, "Got it");
    }
    if (ol_window_present() != OL_OK)
        return 0;
    example_log("STUDIO FRAME n=%u scenes=%u playing=%d active=%d phase=%d\n", ++s->frames,
                s->scene_frames, s->playing, s->ui_active, (int)(s->phase * 1000));
    return 1;
}
