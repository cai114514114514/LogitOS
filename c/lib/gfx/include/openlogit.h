#ifndef OPENLOGIT_H
#define OPENLOGIT_H
/* LogitOS graphics runtime, API 1.0. gfx.h supplies geometry builders and
 * paint values; devices, surface lifetimes, command ownership, submission
 * and version/capability discovery belong here. All storage is caller-owned
 * and 8-byte aligned. Query sizes instead of depending on private layouts.
 *
 * This is a native API, not a DirectX/OpenGL compatibility implementation.
 * API version, implementation revision and rendering capabilities are separate.
 * Unsupported requirements fail at device creation, never silently degrade.
 *
 * A device/surface/list has one owner while it is being changed. Independent
 * devices may render concurrently. Submit on a busy device returns OL_BUSY.
 * Pixel/storage buffers must remain alive until their object is destroyed;
 * inspect pixels only while idle and upload changes through the API. Object
 * storage and pixel allocations must not alias other live objects/buffers;
 * do not recreate an object in occupied storage. These are in-process C
 * objects, not validated handles for a kernel/GPU command transport. */
#include "gfx.h"
#include <stdint.h>

#define OL_VERSION(major, minor) (((uint32_t)(major) << 16) | (uint32_t)(minor))
#define OL_API_VERSION OL_VERSION(1, 1)
enum ol_status {
    OL_OK = 0,
    OL_ARGUMENT = -1,
    OL_VERSION_UNSUPPORTED = -2,
    OL_UNSUPPORTED = -3,
    OL_LIMIT = -4,
    OL_STATE = -5,
    OL_BUSY = -6,
    OL_STALE_RESOURCE = -7,
    OL_RENDER_FAILED = -8,
    OL_BACKEND_FAILED = -9
};
enum { OL_BACKEND_SOFTWARE = 1, OL_FORMAT_RGBA8_STRAIGHT = 1,
       OL_FORMAT_A8 = 2, OL_FORMAT_BGRX8 = 3 };
enum {
    OL_CAP_PATH_FILL = 1u << 0,
    OL_CAP_RECT_CLIP = 1u << 1,
    OL_CAP_GRADIENT = 1u << 2,
    OL_CAP_IMAGE = 1u << 3,
    OL_CAP_ATOMIC_FRAME = 1u << 4,
    OL_CAP_PATH_CLIP = 1u << 5,
    OL_CAP_STROKE = 1u << 6,
    OL_CAP_TRANSFORM = 1u << 7,
    OL_CAP_GLYPH_MASK = 1u << 8,
    OL_CAP_DAMAGE = 1u << 9,
    OL_CAP_LAYER = 1u << 10,
    OL_CAP_BLUR = 1u << 11,
    OL_CAP_BACKDROP = 1u << 12,
    OL_CAP_GPU = 1u << 16,
    OL_CAP_3D = 1u << 17,
    OL_CAP_PIXEL_MATERIAL = 1u << 18 /* user-space extension query only */
};

struct ol_caps {
    uint32_t size, api_version, implementation_revision, backend, features;
    uint32_t max_width, max_height, max_path_points, max_command_bytes;
    uint32_t max_active_edges, max_samples;
};
struct ol_device;
struct ol_surface;
struct ol_list;

unsigned long ol_device_size(void);
int ol_device_create(void *storage, unsigned long bytes, uint32_t api_version,
                     uint32_t required_features, struct ol_device **out);
int ol_device_caps(const struct ol_device *, struct ol_caps *out);
uint64_t ol_device_completed(const struct ol_device *);
int ol_device_destroy(struct ol_device *);
const char *ol_status_string(int status);

/* Front is the last successfully submitted frame. Work is a disjoint scratch
 * image: failed commands never publish a partly drawn frame to front.
 * Stride is bytes per row, format is explicit; padding bytes are preserved. */
struct ol_surface_desc {
    uint32_t size, format, width, height, stride;
    unsigned char *front, *work;
    unsigned long front_bytes, work_bytes;
};
struct ol_surface_view {
    const unsigned char *pixels;
    uint32_t width, height, stride, format;
    uint64_t generation;
};
unsigned long ol_surface_size(void);
int ol_surface_create(struct ol_device *, void *storage, unsigned long bytes,
                      const struct ol_surface_desc *, struct ol_surface **out);
int ol_surface_view(const struct ol_surface *, struct ol_surface_view *out);
int ol_surface_upload(struct ol_surface *, const unsigned char *pixels, unsigned long bytes,
                      unsigned stride);
int ol_surface_destroy(struct ol_surface *);

/* Each list owns copies of path coordinates and paint values. Resetting the
 * builder after recording cannot alter queued geometry. Image commands retain
 * a surface reference plus its generation until list reset/destroy; mutation
 * after recording is reported as OL_STALE_RESOURCE on submit.
 * Recording errors latch: close/submit fail until reset, including exhaustion
 * of the caller's command arena. No successful-looking truncated command list. */
unsigned long ol_list_min_size(void);
int ol_list_create(struct ol_device *, void *storage, unsigned long bytes, struct ol_list **out);
int ol_list_reset(struct ol_list *);
int ol_list_destroy(struct ol_list *);
int ol_cmd_clear(struct ol_list *, unsigned rgb, int alpha);
int ol_cmd_fill(struct ol_list *, const struct gfx_path *, int fill_rule, const struct gfx_paint *,
                const struct gfx_rect *clip, int samples);
int ol_cmd_image(struct ol_list *, struct ol_surface *source, struct gfx_rect destination,
                 int alpha, int bilinear);
int ol_list_close(struct ol_list *);
/* Synchronous software execution. The completion serial advances only after
 * front is committed; it is not a GPU fence or a display/vblank timestamp. */
int ol_submit(struct ol_device *, const struct ol_list *, struct ol_surface *target,
              uint64_t *completion_serial);

/* 1.1 additions. Every 1.0 descriptor above keeps its original layout.
 * Image resources have one storage buffer and cannot be draw targets. The
 * application keeps storage alive and updates it through ol_image_update;
 * recorded references retain the resource and reject a changed generation.
 * A8 is linear coverage, tightly packed; RGBA permits padded rows. */
struct ol_image;
struct ol_image_desc {
    uint32_t size, format, width, height, stride;
    unsigned char *pixels;
    unsigned long bytes;
};
unsigned long ol_image_size(void);
int ol_image_create(struct ol_device *, void *storage, unsigned long bytes,
                     const struct ol_image_desc *, struct ol_image **out);
int ol_image_view(const struct ol_image *, struct ol_surface_view *out);
int ol_image_update(struct ol_image *, struct gfx_rect region, const unsigned char *,
                     unsigned long bytes, unsigned stride);
int ol_image_destroy(struct ol_image *);
struct ol_fill_options {
    uint32_t size;
    const struct gfx_matrix *transform; /* flattened geometry, then transformed */
    const struct gfx_rect *clip;
    struct ol_image *coverage;           /* optional A8 path clip */
    int mask_x, mask_y, opacity, samples;
};
int ol_cmd_fill_ex(struct ol_list *, const struct gfx_path *, int rule,
                   const struct gfx_paint *, const struct ol_fill_options *);
/* Scratch outline is caller storage, reused immediately after recording. */
int ol_cmd_stroke(struct ol_list *, const struct gfx_path *, const struct gfx_stroke *,
                  const struct gfx_paint *, const struct ol_fill_options *, struct gfx_path *scratch);
int ol_cmd_image_resource(struct ol_list *, struct ol_image *, struct gfx_rect dst,
                          int opacity, int bilinear);
int ol_cmd_glyph_mask(struct ol_list *, struct ol_image *, int x, int y, unsigned rgb, int opacity);
struct ol_submit_info {
    uint32_t size;
    struct gfx_rect damage;
    /* Front/work initialization plus publication bytes; excludes temporary
     * effect snapshots and rasterizer workspace, and is not display traffic. */
    uint64_t copied_bytes, commands, completion_serial;
};
int ol_submit_damage(struct ol_device *, const struct ol_list *, struct ol_surface *,
                      struct ol_submit_info *);
/* An offscreen surface is a group: overlapping children are composed there
 * before applying this opacity once. Recording retains its published version.
 * Box blur radius is in destination pixels, bounded by OL_MAX_BLUR_RADIUS.
 * Filtering uses premultiplied color and transparent samples outside the layer;
 * the returned/displayed pixels still use straight RGBA. Shadow derives from
 * source coverage, is drawn behind the group, and shares the group opacity.
 * Clip applies after effects (including their expanded bounds). */
#define OL_MAX_BLUR_RADIUS 64
struct ol_layer_options {
    uint32_t size;
    const struct gfx_rect *clip;
    int opacity, bilinear, blur_radius;
    int shadow_radius, shadow_dx, shadow_dy, shadow_opacity;
    unsigned shadow_rgb;
};
int ol_cmd_layer(struct ol_list *, struct ol_surface *source, struct gfx_rect dst,
                  const struct ol_layer_options *);
/* Blur the accumulated frame behind region, then source-over a uniform tint.
 * The backdrop is sampled at execution time, including earlier list commands;
 * it is snapshotted before filtering, so neighboring outputs cannot feed back.
 * Transparent sampling beyond target edges matches offscreen layer filtering. */
int ol_cmd_backdrop(struct ol_list *, struct gfx_rect region, int radius,
                     unsigned tint_rgb, int tint_opacity);
/* Effects allocate nothing internally. Query scratch for this list/target and
 * supply disjoint, 8-byte aligned caller storage. Plain submit returns OL_LIMIT
 * when scratch is required; it never silently omits an effect. The largest
 * command reuses the workspace, rather than allocating once per layer.
 * Too-small/aliased scratch, stale sources or a later failure publish nothing. */
int ol_submit_workspace_size(const struct ol_list *, const struct ol_surface *, unsigned long *);
int ol_submit_workspace(struct ol_device *, const struct ol_list *, struct ol_surface *,
                         void *scratch, unsigned long bytes, struct ol_submit_info *);
/* Include old and new bounds plus blur/shadow extent when moving a layer.
 * Result is clamped to the target. Empty damage never means full damage. */
struct gfx_rect ol_damage_move(struct gfx_rect old_bounds, struct gfx_rect new_bounds,
                               int effect_extent, int width, int height);
#endif
