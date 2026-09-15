#include "openlogit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "../../c/apps/gui/clock/geometry.h"

static int checks, failures;
#define CHECK(ok, label)                                                                           \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(ok)) {                                                                               \
            printf("FAIL %s\n", label);                                                            \
            failures++;                                                                            \
        } else                                                                                     \
            printf("PASS %s\n", label);                                                            \
    } while (0)
struct fixture {
    void *dm, *sm, *lm;
    struct ol_device *d;
    struct ol_surface *s;
    struct ol_list *l;
    unsigned char front[40 * 32], work[40 * 32];
};
static void setup(struct fixture *f)
{
    memset(f, 0, sizeof *f);
    f->dm = calloc(1, ol_device_size());
    f->sm = calloc(1, ol_surface_size());
    f->lm = calloc(1, 65536);
    if (ol_device_create(f->dm, ol_device_size(), OL_API_VERSION, OL_CAP_ATOMIC_FRAME, &f->d))
        abort();
    struct ol_surface_desc a = {
        sizeof a,      OL_FORMAT_RGBA8_STRAIGHT, 8, 32, 40, f->front, f->work, sizeof f->front,
        sizeof f->work};
    memset(f->front, 0x73, sizeof f->front);
    memset(f->work, 0x51, sizeof f->work);
    if (ol_surface_create(f->d, f->sm, ol_surface_size(), &a, &f->s) ||
        ol_list_create(f->d, f->lm, 65536, &f->l))
        abort();
}
static void cleanup(struct fixture *f)
{
    ol_list_destroy(f->l);
    ol_surface_destroy(f->s);
    ol_device_destroy(f->d);
    free(f->lm);
    free(f->sm);
    free(f->dm);
}
static int rectangle(struct ol_list *l, unsigned color, int x, int y, int w, int h)
{
    int pt[32], sub[4];
    struct gfx_path p;
    struct gfx_paint paint;
    gfx_path_init(&p, pt, 16, sub, 4);
    gfx_path_rect(&p, GFX_PX(x), GFX_PX(y), GFX_PX(w), GFX_PX(h));
    gfx_paint_solid(&paint, color, 255);
    return ol_cmd_fill(l, &p, GFX_NONZERO, &paint, 0, 4);
}
static int pixel(struct fixture *f, int x, int y, unsigned rgb, int alpha)
{
    unsigned char *p = f->front + y * 40 + x * 4;
    return p[0] == GFX_R(rgb) && p[1] == GFX_G(rgb) && p[2] == GFX_B(rgb) && p[3] == alpha;
}
struct worker {
    int index, failed;
};
static void *parallel(void *arg)
{
    struct worker *w = arg;
    struct fixture f;
    setup(&f);
    unsigned rgb = 0x102030u + w->index * 0x131109u;
    for (int i = 0; i < 100; i++) {
        ol_list_reset(f.l);
        ol_cmd_clear(f.l, 0, 255);
        rectangle(f.l, rgb, 1, 2, 5, 10);
        ol_list_close(f.l);
        if (ol_submit(f.d, f.l, f.s, 0) || !pixel(&f, 3, 5, rgb, 255) || !pixel(&f, 0, 0, 0, 255))
            w->failed = 1;
    }
    cleanup(&f);
    return 0;
}
int main(void)
{
    struct fixture f;
    setup(&f);
    struct ol_caps caps = {.size = sizeof caps};
    CHECK(ol_device_caps(f.d, &caps) == OL_OK && caps.api_version == OL_API_VERSION &&
              caps.backend == OL_BACKEND_SOFTWARE && !(caps.features & (OL_CAP_GPU | OL_CAP_3D)) &&
              caps.max_active_edges == GFX_MAX_ACTIVE && caps.max_samples == GFX_MAX_SUBS,
          "truthful API and software capabilities");
    void *scratch = calloc(1, ol_device_size());
    struct ol_device *bad = 0;
    CHECK(ol_device_create(scratch, ol_device_size(), OL_VERSION(2, 0), 0, &bad) ==
                  OL_VERSION_UNSUPPORTED &&
              !bad,
          "unsupported API version refused");
    CHECK(ol_device_create(scratch, ol_device_size(), OL_API_VERSION, OL_CAP_GPU, &bad) ==
                  OL_UNSUPPORTED &&
              !bad,
          "unsupported hardware requirement refused");
    free(scratch);
    CHECK(ol_device_destroy(f.d) == OL_BUSY, "live surfaces and lists retain device");
    CHECK(ol_submit(f.d, f.l, f.s, 0) == OL_STATE, "unclosed command list refused");
    ol_cmd_clear(f.l, 0x204060, 255);
    rectangle(f.l, 0xa04020, 2, 3, 4, 6);
    ol_list_close(f.l);
    uint64_t serial = 0;
    CHECK(ol_submit(f.d, f.l, f.s, &serial) == OL_OK && serial == 1, "ordinary frame commits");
    CHECK(pixel(&f, 3, 4, 0xa04020, 255) && pixel(&f, 0, 0, 0x204060, 255),
          "recorded local geometry survives builder lifetime");
    int padding = 1;
    for (int y = 0; y < 32; y++)
        for (int i = 32; i < 40; i++)
            if (f.front[y * 40 + i] != 0x73)
                padding = 0;
    CHECK(padding, "row padding preserved");
    CHECK(ol_device_completed(f.d) == serial, "completion serial follows committed frame");
    ol_list_reset(f.l);
    int pt[32], sub[4];
    struct gfx_path p;
    struct gfx_paint paint;
    gfx_path_init(&p, pt, 16, sub, 4);
    gfx_path_rect(&p, 0, 0, GFX_PX(8), GFX_PX(32));
    gfx_paint_solid(&paint, 0x00ee77, 255);
    struct gfx_rect clip = {3, 5, 2, 4};
    ol_cmd_clear(f.l, 0x010203, 255);
    CHECK(ol_cmd_fill(f.l, &p, GFX_NONZERO, &paint, &clip, 16) == OL_OK, "record clipped fill");
    gfx_path_reset(&p);
    gfx_path_rect(&p, 0, 0, GFX_PX(1), GFX_PX(1));
    gfx_paint_solid(&paint, 0xff0000, 255);
    ol_list_close(f.l);
    CHECK(ol_submit(f.d, f.l, f.s, 0) == OL_OK && pixel(&f, 3, 5, 0x00ee77, 255) &&
              pixel(&f, 2, 5, 0x010203, 255),
          "clip and copied paint remain independent of builder changes");
    ol_list_reset(f.l);
    ol_cmd_clear(f.l, 0, 255);
    gfx_path_reset(&p);
    gfx_path_rect(&p, 0, 0, GFX_PX(8), GFX_PX(32));
    gfx_paint_linear(&paint, 0, 0, GFX_PX(8), 0);
    gfx_paint_stop(&paint, 0, 0x000000, 255);
    gfx_paint_stop(&paint, 65536, 0xffffff, 255);
    ol_cmd_fill(f.l, &p, GFX_NONZERO, &paint, 0, 4);
    ol_list_close(f.l);
    CHECK(ol_submit(f.d, f.l, f.s, 0) == OL_OK && f.front[0] < f.front[7 * 4] &&
              f.front[0] == f.front[1],
          "gradient is a real recorded paint consumer");
    unsigned char previous[sizeof f.front];
    memcpy(previous, f.front, sizeof previous);
    uint64_t completed = ol_device_completed(f.d);
    ol_list_reset(f.l);
    ol_cmd_clear(f.l, 0xff0000, 255);
    int *dense = calloc(600 * 5 * 2, sizeof(int)), *contours = calloc(600, sizeof(int));
    struct gfx_path many;
    gfx_path_init(&many, dense, 600 * 5, contours, 600);
    for (int i = 0; i < 600; i++)
        gfx_path_rect(&many, GFX_PX(1), 0, GFX_PX(5), GFX_PX(20));
    CHECK(ol_cmd_fill(f.l, &many, GFX_NONZERO, &paint, 0, 4) == OL_OK,
          "large valid geometry records before backend capacity check");
    ol_list_close(f.l);
    CHECK(ol_submit(f.d, f.l, f.s, &serial) == OL_RENDER_FAILED && serial == 0 &&
              ol_device_completed(f.d) == completed && !memcmp(previous, f.front, sizeof previous),
          "failed late draw leaves front and completion unchanged");
    free(dense);
    free(contours);
    void *small = calloc(1, ol_list_min_size());
    struct ol_list *short_list;
    ol_list_create(f.d, small, ol_list_min_size(), &short_list);
    ol_cmd_clear(short_list, 0, 0);
    CHECK(rectangle(short_list, 0xffffff, 0, 0, 2, 2) == OL_LIMIT &&
              ol_list_close(short_list) == OL_LIMIT &&
              ol_submit(f.d, short_list, f.s, 0) == OL_LIMIT,
          "command capacity failure latches until reset");
    CHECK(ol_list_reset(short_list) == OL_OK && ol_cmd_clear(short_list, 0x8899aa, 255) == OL_OK &&
              ol_list_close(short_list) == OL_OK,
          "reset recovers exhausted list");
    ol_list_destroy(short_list);
    free(small);
    unsigned char img[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255},
                  front[16], work[16];
    void *im = calloc(1, ol_surface_size());
    struct ol_surface *image;
    struct ol_surface_desc a = {
        sizeof a, OL_FORMAT_RGBA8_STRAIGHT, 2, 2, 8, front, work, sizeof front, sizeof work};
    CHECK(ol_surface_create(f.d, im, ol_surface_size(), &a, &image) == OL_OK &&
              ol_surface_upload(image, img, sizeof img, 8) == OL_OK,
          "image resource upload");
    ol_list_reset(f.l);
    ol_cmd_clear(f.l, 0, 255);
    CHECK(ol_cmd_image(f.l, image, (struct gfx_rect){2, 3, 4, 4}, 255, 0) == OL_OK,
          "record scaled image resource");
    ol_list_close(f.l);
    CHECK(ol_surface_destroy(image) == OL_BUSY, "recorded image retains resource lifetime");
    CHECK(ol_submit(f.d, f.l, f.s, 0) == OL_OK && pixel(&f, 2, 3, 0xff0000, 255) &&
              pixel(&f, 5, 6, 0xffffff, 255),
          "image commands sample uploaded pixels");
    memcpy(previous, f.front, sizeof previous);
    img[0] = 127;
    ol_surface_upload(image, img, sizeof img, 8);
    CHECK(ol_submit(f.d, f.l, f.s, 0) == OL_STALE_RESOURCE &&
              !memcmp(previous, f.front, sizeof previous),
          "changed image generation refuses stale recorded work");
    ol_list_reset(f.l);
    CHECK(ol_surface_destroy(image) == OL_OK, "reset releases retained images");
    free(im);
    a.front = f.front;
    a.front_bytes = sizeof f.front;
    a.work = f.front;
    a.work_bytes = sizeof f.front;
    void *sm = calloc(1, ol_surface_size());
    CHECK(ol_surface_create(f.d, sm, ol_surface_size(), &a, &image) == OL_ARGUMENT,
          "overlapping transactional buffers refused");
    free(sm);
    pthread_t tids[4];
    struct worker workers[4] = {0};
    for (int i = 0; i < 4; i++) {
        workers[i].index = i;
        pthread_create(&tids[i], 0, parallel, &workers[i]);
    }
    int independent = 1;
    for (int i = 0; i < 4; i++) {
        pthread_join(tids[i], 0);
        if (workers[i].failed)
            independent = 0;
    }
    CHECK(independent, "four independent device workspaces render 400 frames concurrently");
    int cpt[CLOCK_PATH_CAP * 2], csub[16], fits = 1, max_points = 0;
    for (int radius = 23; radius <= 192; radius++) {
        struct gfx_path dial;
        gfx_path_init(&dial, cpt, CLOCK_PATH_CAP, csub, 16);
        clock_face_path(&dial, 192, 192, radius, 1);
        ol_list_reset(f.l);
        gfx_paint_solid(&paint, 0xffffff, 255);
        if (dial.overflow || ol_cmd_fill(f.l, &dial, GFX_EVENODD, &paint, 0, 4) != OL_OK)
            fits = 0;
        if (dial.npt > max_points)
            max_points = dial.npt;
    }
    CHECK(fits, "Clock device geometry fits at every supported face radius");
    printf("Clock double-circle maximum points: %d; capacity: %d\n", max_points, CLOCK_PATH_CAP);
    cleanup(&f);
    printf("Storage bytes: device=%lu surface=%lu list-min=%lu\n",
           ol_device_size(), ol_surface_size(), ol_list_min_size());
    printf("OpenLogit runtime: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
