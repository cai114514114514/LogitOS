#include "../example_log.h"
#include "shaders.h"
#include "studio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const struct gfx_rect studio_viewport = {208, 108, 520, 364};

int studio_image_create(struct studio *studio, struct studio_image *image, unsigned w, unsigned h)
{
    *image = (struct studio_image){.width = w, .height = h};
    image->storage = malloc(ol_surface_size());
    image->front = calloc((size_t)w * h, 4);
    image->work = malloc((size_t)w * h * 4);
    if (!image->storage || !image->front || !image->work) {
        studio_image_destroy(image);
        return 0;
    }
    struct ol_surface_desc desc = {sizeof desc,
                                   OL_FORMAT_RGBA8_STRAIGHT,
                                   w,
                                   h,
                                   w * 4,
                                   image->front,
                                   image->work,
                                   (unsigned long)w * h * 4,
                                   (unsigned long)w * h * 4};
    if (ol_surface_create(studio->device, image->storage, ol_surface_size(), &desc,
                          &image->surface) != OL_OK) {
        studio_image_destroy(image);
        return 0;
    }
    return 1;
}

void studio_image_destroy(struct studio_image *image)
{
    if (image->surface)
        ol_surface_destroy(image->surface);
    free(image->storage);
    free(image->front);
    free(image->work);
    *image = (struct studio_image){0};
}

static int pipeline(const char *vertex_source, const char *fragment_source,
                    struct ol3d_pipeline_object **out, int cull)
{
    struct ol_lsl_shader *vertex = NULL, *fragment = NULL;
    struct ol_shader_error error = {0};
    struct ol_lsl_raster_state state = {.cull_back = cull, .depth_test = 1, .depth_write = 1};
    int status = ol_lsl_compile(vertex_source, strlen(vertex_source), &vertex, &error);
    if (status == OL_OK)
        status = ol_lsl_compile(fragment_source, strlen(fragment_source), &fragment, &error);
    if (status == OL_OK)
        status = ol_lsl_pipeline_create(vertex, fragment, &state, out, &error);
    if (status != OL_OK)
        example_log("STUDIO ERROR shader %u: %s\n", error.line, error.message);
    ol_lsl_destroy(vertex);
    ol_lsl_destroy(fragment);
    return status == OL_OK;
}

int studio_scene_resize(struct studio *studio, int low)
{
    unsigned width = low ? 160 : 320;
    unsigned height = low ? 112 : 224;
    struct ol3d_context *renderer = NULL;
    struct studio_image image = {0};
    if (ol3d_create(width, height, &renderer) != OL_OK)
        return 0;
    if (!studio_image_create(studio, &image, width, height)) {
        ol3d_destroy(renderer);
        return 0;
    }
    /* Release recorded layer references before replacing their backing pixels. */
    ol_list_reset(studio->list);
    ol3d_destroy(studio->renderer);
    studio_image_destroy(&studio->scene);
    studio->renderer = renderer;
    studio->scene = image;
    studio->low = low;
    studio->scene_dirty = studio->dirty = 1;
    return 1;
}

int studio_scene_init(struct studio *studio)
{
    static const struct ol3d_mesh_desc descriptions[4] = {{OL3D_SPHERE, 16, 10, .78f, 1},
                                                          {OL3D_TORUS, 16, 10, .55f, .22f},
                                                          {OL3D_TUBE, 12, 10, .24f, 1.7f},
                                                          {OL3D_PLANE, 3, 2, 4.5f, 4.5f}};
    if (!pipeline(studio_vertex, studio_fragment, &studio->lit_pipeline, 1) ||
        !pipeline(studio_flat_vertex, studio_flat_fragment, &studio->flat_pipeline, 0) ||
        !studio_scene_resize(studio, studio->low))
        return 0;
    for (unsigned index = 0; index < 4; index++) {
        struct studio_object *object = &studio->objects[index];
        if (ol3d_mesh_create(descriptions + index, &object->mesh) != OL_OK ||
            ol3d_buffer_create(OL3D_VERTEX_BUFFER, object->mesh.vertices, object->mesh.vertex_count,
                               &object->vertices) != OL_OK ||
            ol3d_buffer_create(OL3D_INDEX_BUFFER, object->mesh.indices, object->mesh.index_count,
                               &object->indices) != OL_OK)
            return 0;
        ol_mat4_identity(object->model);
    }
    size_t count = studio->objects[2].mesh.vertex_count;
    studio->deformed = malloc(count * sizeof *studio->deformed);
    studio->weights = calloc(count, sizeof *studio->weights);
    if (!studio->deformed || !studio->weights)
        return 0;
    for (size_t index = 0; index < count; index++) {
        float y = studio->objects[2].mesh.vertices[index].attribute[0][1];
        float weight = (y + .85f) / 1.7f;
        studio->weights[index] = (struct ol3d_skin_weights){.joints = {0, 1, 0, 0},
                                                            .weights = {1 - weight, weight, 0, 0}};
    }
    return 1;
}

static int deform_rig(struct studio *studio)
{
    float angle = .95f * (float)sin(studio->phase * 6.2831853f);
    struct ol3d_node rest[2] = {{.parent = -1, .rotation = {0, 0, 0, 1}, .scale = {1, 1, 1}},
                                {.parent = 0, .rotation = {0, 0, 0, 1}, .scale = {1, 1, 1}}};
    struct ol3d_node bent[2], mixed[2];
    memcpy(bent, rest, sizeof rest);
    bent[1].rotation[2] = (float)sin(angle * .5f);
    bent[1].rotation[3] = (float)cos(angle * .5f);
    float bones[2][16];
    if (ol3d_pose_mix(mixed, rest, bent, 2, studio->blend) != OL_OK ||
        ol3d_scene_matrices(mixed, 2, bones) != OL_OK)
        return 0;
    struct studio_object *rig = &studio->objects[2];
    if (ol3d_skin_mesh(studio->deformed, rig->mesh.vertices, studio->weights,
                       rig->mesh.vertex_count, bones, 2) != OL_OK)
        return 0;
    return ol3d_buffer_update(rig->vertices, 0, rig->mesh.vertex_count, studio->deformed) == OL_OK;
}

static int draw_object(struct studio *studio, unsigned index, const float model[16],
                       const float color[4], int flat)
{
    struct studio_object *object = &studio->objects[index];
    float mvp[16], normal[16];
    ol_mat4_mul(mvp, studio->view_projection, model);
    memcpy(studio->uniforms, mvp, sizeof mvp);
    if (!flat) {
        if (ol_mat4_normal(normal, model) != OL_OK)
            return 0;
        memcpy(studio->uniforms + 4, model, 16 * sizeof(float));
        memcpy(studio->uniforms + 8, normal, sizeof normal);
    }
    memcpy(studio->uniforms[14], color, 4 * sizeof(float));
    struct ol3d_texture texture = {studio->texture.front, 64, 64, 256, 64 * 64 * 4, 1, 1};
    struct ol3d_bindings bindings = {studio->uniforms, 17, &texture, 1};
    return ol3d_draw_buffers(studio->renderer, flat ? studio->flat_pipeline : studio->lit_pipeline,
                             &bindings, ol3d_buffer_view(object->vertices),
                             ol3d_buffer_view(object->indices), 0,
                             object->mesh.index_count) == OL_OK;
}

int studio_scene_render(struct studio *studio)
{
    uint64_t start = monotonic_ns();
    if (ol3d_orbit_matrices(&studio->camera, studio->eye, studio->view_projection) != OL_OK ||
        !deform_rig(studio) || ol3d_begin(studio->renderer, 0x14263a) != OL_OK)
        return 0;
    float turn = studio->phase * 6.2831853f;
    float rotation[4] = {0, (float)sin(turn * .5f), 0, (float)cos(turn * .5f)};
    const float positions[4][3] = {
        {-1.05f, 0, .25f}, {1.05f, 0, .25f}, {0, -.1f, -1.15f}, {0, -1.15f, 0}};
    for (unsigned index = 0; index < 4; index++)
        ol_mat4_trs(studio->objects[index].model, positions[index],
                    index == 1 ? rotation : (float[]){0, 0, 0, 1}, (float[]){1, 1, 1});
    studio->uniforms[12][0] = (studio->light - .5f) * 7;
    studio->uniforms[12][1] = 4;
    studio->uniforms[12][2] = 2;
    studio->uniforms[12][3] = 1;
    memcpy(studio->uniforms[13], studio->eye, sizeof studio->eye);
    studio->uniforms[15][0] = studio->gloss;
    studio->uniforms[15][1] = studio->toon;
    studio->uniforms[15][2] = .003f;
    studio->uniforms[15][3] = studio->textured;
    memcpy(studio->uniforms[16], (float[]){.078f, .149f, .227f, 1}, 4 * sizeof(float));
    if (!draw_object(studio, 3, studio->objects[3].model, (float[]){.15f, .25f, .29f, 1}, 1))
        return 0;
    if (studio->shadows) {
        float shadow[16];
        if (ol3d_planar_shadow(shadow, (float[]){0, 1, 0, 1.14f}, studio->uniforms[12]) != OL_OK)
            return 0;
        for (unsigned index = 0; index < 3; index++) {
            float projected[16];
            ol_mat4_mul(projected, shadow, studio->objects[index].model);
            if (!draw_object(studio, index, projected, (float[]){.065f, .105f, .13f, 1}, 1))
                return 0;
        }
    }
    const float colors[3][4] = {{.35f, .8f, .67f, 1}, {.9f, .53f, .26f, 1}, {.49f, .58f, .95f, 1}};
    for (unsigned index = 0; index < 3; index++) {
        float color[4];
        memcpy(color, colors[index], sizeof color);
        if (studio->selected == (int)index) {
            for (unsigned axis = 0; axis < 3; axis++)
                color[axis] *= 1.15f;
        }
        if (!draw_object(studio, index, studio->objects[index].model, color, 0))
            return 0;
    }
    if (ol3d_end(studio->renderer, studio->scene.surface) != OL_OK)
        return 0;
    studio->render_ns = monotonic_ns() - start;
    studio->scene_frames++;
    example_log("STUDIO RENDER ns=%lu width=%u height=%u\n", (unsigned long)studio->render_ns,
                studio->scene.width, studio->scene.height);
    return 1;
}

int studio_pick(struct studio *studio, int x, int y)
{
    struct ol3d_ray ray;
    float normalized_x = 2.f * (x - studio_viewport.x) / studio_viewport.w - 1;
    float normalized_y = 1 - 2.f * (y - studio_viewport.y) / studio_viewport.h;
    if (ol3d_orbit_ray(&studio->camera, normalized_x, normalized_y, &ray) != OL_OK)
        return -1;
    float nearest = -1;
    int selected = -1;
    for (unsigned index = 0; index < 3; index++) {
        struct ol3d_mesh mesh = studio->objects[index].mesh;
        if (index == 2)
            mesh.vertices = studio->deformed;
        float hit = ol3d_mesh_pick(&mesh, studio->objects[index].model, &ray, NULL);
        if (hit >= 0 && (nearest < 0 || hit < nearest)) {
            nearest = hit;
            selected = (int)index;
        }
    }
    return selected;
}

void studio_scene_destroy(struct studio *studio)
{
    ol3d_pipeline_destroy(studio->lit_pipeline);
    ol3d_pipeline_destroy(studio->flat_pipeline);
    ol3d_destroy(studio->renderer);
    for (unsigned index = 0; index < 4; index++) {
        ol3d_buffer_destroy(studio->objects[index].vertices);
        ol3d_buffer_destroy(studio->objects[index].indices);
        ol3d_mesh_destroy(&studio->objects[index].mesh);
    }
    free(studio->deformed);
    free(studio->weights);
}
