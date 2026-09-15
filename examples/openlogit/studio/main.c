/* Guest build, from /usr/share/openlogit/studio:
 * tcc main.c scene.c ui.c -I/usr/include/openlogit -lopenlogit -o /tmp/scene-studio
 */
#include "../example_log.h"
#include "studio.h"
#include <stdlib.h>

static int initialize(struct studio *s)
{
    s->device_storage = malloc(ol_device_size());
    s->list_storage = malloc(512 * 1024);
    if (!s->device_storage || !s->list_storage ||
        ol_device_create(s->device_storage, ol_device_size(), OL_API_VERSION, OL_CAP_LAYER,
                         &s->device) != OL_OK ||
        ol_list_create(s->device, s->list_storage, 512 * 1024, &s->list) != OL_OK ||
        !studio_image_create(s, &s->frame, 960, 600) ||
        !studio_image_create(s, &s->texture, 64, 64))
        return 0;
    ol_cmd_clear(s->list, 0xe7e8d5, 255);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if ((x + y) % 2)
                ol_ui_panel(s->list, (struct gfx_rect){x * 16, y * 16, 16, 16}, 0x66888d, 255);
        }
    }
    if (ol_list_close(s->list) != OL_OK ||
        ol_submit(s->device, s->list, s->texture.surface, NULL) != OL_OK)
        return 0;
    ol_list_reset(s->list);
    studio_ui_init(s, monotonic_ns());
    return studio_scene_init(s);
}

static void key(struct studio *s, int code, uint64_t now)
{
    unsigned action = CONTROL_COUNT;
    switch (code) {
    case ' ':
        action = PLAY;
        break;
    case 's':
    case 'S':
        action = SHADOW;
        break;
    case 't':
    case 'T':
        action = TEXTURE;
        break;
    case 'c':
    case 'C':
        action = TOON;
        break;
    case 'h':
    case 'H':
        action = HELP;
        break;
    case 'r':
    case 'R':
        action = RESET;
        break;
    case 'q':
    case 'Q':
        action = QUALITY;
        break;
    case '+':
    case '=':
        action = ZOOM_IN;
        break;
    case '-':
        action = ZOOM_OUT;
        break;
    case KEY_LEFT:
        studio_adjust(s, -1, now);
        break;
    case KEY_RIGHT:
        studio_adjust(s, 1, now);
        break;
    case '1':
    case '2':
    case '3':
        action = OBJECT_SPHERE + code - '1';
        break;
    case 27:
        if (s->modal)
            action = CLOSE_DIALOG;
        else if (s->playing)
            action = PLAY;
        break;
    case 9: {
        struct ol_ui_event event = {OL_UI_NEXT, 0, 0};
        ol_ui_event(&s->ui, &event);
        break;
    }
    case 10:
    case 13: {
        struct ol_ui_event event = {OL_UI_ACTIVATE, 0, 0};
        int id = ol_ui_event(&s->ui, &event);
        if (id >= 0 && ol_ui_take_activation(&s->ui, (unsigned)id))
            action = (unsigned)id;
        break;
    }
    default:
        break;
    }
    if (action < CONTROL_COUNT)
        studio_action(s, action, now);
}

int main(void)
{
    struct studio *s = calloc(1, sizeof *s);
    int result = 1;
    if (!s || !initialize(s))
        goto done;
    gui_create("OpenLogit Scene Studio", 960, 600);
    _sys(SYS_GUI_WIN_MIN, (960L << 16) | 600, 0, 0);
    s->reduced = setting_int("ui.reduce_motion", 0) != 0;
    example_log("STUDIO READY api=1.1 lsl=1 skin=1 shadow=1 gpu=0\n");
    int was_active = 0;
    result = 0;
    for (;;) {
        uint64_t now = monotonic_ns();
        ol_ui_begin(&s->ui, now, s->reduced);
        struct logit_event event;
        while (poll_event(&event)) {
            if (event.type == EV_CLOSE)
                goto done;
            if (event.type == EV_KEY)
                key(s, event.a, now);
            if (event.type == EV_THEME || event.type == EV_WINDOW_FOCUS ||
                event.type == EV_RESIZE) {
                s->reduced = setting_int("ui.reduce_motion", 0) != 0;
                s->ui.reduced = s->reduced;
                s->dirty = 1;
                example_log("STUDIO MOTION reduced=%d\n", s->reduced);
            }
            if (event.type == EV_MOUSE || event.type == EV_MOUSE_R || event.type == EV_MOUSE_UP ||
                event.type == EV_MOUSE_MOVE || event.type == EV_WHEEL)
                studio_pointer(s, &event, now);
        }
        if (!ol_window_visible()) {
            wait_idle(0);
            continue;
        }
        studio_ui_sample(s, now);
        int active = s->ui_active || s->playing;
        if (s->dirty || s->scene_dirty || active || was_active) {
            /* A recorded layer retains its source version. Drop last frame's
             * references before publishing a fresh 3D image. */
            ol_list_reset(s->list);
            if (s->scene_dirty && !studio_scene_render(s)) {
                example_log("STUDIO ERROR render\n");
                result = 1;
                goto done;
            }
            if (!studio_ui_draw(s)) {
                example_log("STUDIO ERROR UI\n");
                result = 1;
                goto done;
            }
            s->dirty = s->scene_dirty = 0;
        }
        was_active = active;
        wait_idle(active ? 16 : 0);
    }
done:
    if (s) {
        if (s->list)
            ol_list_reset(s->list);
        studio_scene_destroy(s);
        studio_image_destroy(&s->scene);
        studio_image_destroy(&s->texture);
        studio_image_destroy(&s->frame);
        if (s->list)
            ol_list_destroy(s->list);
        if (s->device)
            ol_device_destroy(s->device);
        free(s->list_storage);
        free(s->device_storage);
        free(s);
    }
    return result;
}
