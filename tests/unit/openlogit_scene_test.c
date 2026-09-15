#include "../../examples/openlogit/studio/shaders.h"
#include "ols_internal.h"
#include "openlogit_lsl.h"
#include "openlogit_scene.h"
#include "openlogit_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned failures, checks;
static void check(int ok, const char *name)
{
    checks++;
    if (!ok)
        failures++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}
static int near(float a, float b)
{
    return fabsf(a - b) < .0001f;
}

static void meshes(void)
{
    for (unsigned primitive = OL3D_SPHERE; primitive <= OL3D_PLANE; primitive++) {
        struct ol3d_mesh mesh = {0};
        struct ol3d_mesh_desc desc = {primitive, 12, 8, 2, .5f};
        check(ol3d_mesh_create(&desc, &mesh) == OL_OK, "parametric mesh creates");
        if (!mesh.vertices)
            continue;
        int valid = 1;
        for (size_t i = 0; i < mesh.index_count; i += 3) {
            const float *a = mesh.vertices[mesh.indices[i]].attribute[0];
            const float *b = mesh.vertices[mesh.indices[i + 1]].attribute[0];
            const float *c = mesh.vertices[mesh.indices[i + 2]].attribute[0];
            const float *n = mesh.vertices[mesh.indices[i]].attribute[2];
            float u[3], v[3];
            for (int k = 0; k < 3; k++) {
                u[k] = b[k] - a[k];
                v[k] = c[k] - a[k];
            }
            float dot = (u[1] * v[2] - u[2] * v[1]) * n[0] + (u[2] * v[0] - u[0] * v[2]) * n[1] +
                        (u[0] * v[1] - u[1] * v[0]) * n[2];
            if (!(dot > 1e-7f))
                valid = 0;
        }
        check(valid, "triangles have nonzero area and outward winding");
        check(mesh.vertex_count == 117 &&
                  mesh.index_count == (primitive == OL3D_SPHERE ? 504 : 576),
              "mesh counts exclude degenerate sphere poles");
        ol3d_mesh_destroy(&mesh);
    }
}

static void camera_and_shadow(void)
{
    struct ol3d_orbit camera = {.distance = 5, .fovy = 1, .aspect = 1, .near_z = .1f, .far_z = 20};
    struct ol3d_ray ray;
    float eye[3], vp[16];
    check(ol3d_orbit_matrices(&camera, eye, vp) == OL_OK && near(eye[2], 5),
          "orbit eye is analytic five units away");
    check(ol3d_orbit_ray(&camera, 0, 0, &ray) == OL_OK && near(ray.direction[2], -1),
          "center ray points at orbit target");
    struct ol3d_vertex vertices[3] = {0};
    float positions[3][4] = {{-1, -1, 0, 1}, {1, -1, 0, 1}, {0, 1, 0, 1}};
    for (int i = 0; i < 3; i++)
        memcpy(vertices[i].attribute[0], positions[i], sizeof positions[i]);
    uint32_t indices[] = {0, 1, 2};
    struct ol3d_mesh mesh = {vertices, indices, 3, 3};
    float model[16];
    ol_mat4_identity(model);
    check(near(ol3d_mesh_pick(&mesh, model, &ray, NULL), 5),
          "triangle picking returns independent ray distance");
    model[12] = 4;
    check(ol3d_mesh_pick(&mesh, model, &ray, NULL) < 0, "translated mesh misses center ray");
    float shadow[16];
    check(ol3d_planar_shadow(shadow, (float[]){0, 1, 0, 0}, (float[]){2, 4, 0, 1}) == OL_OK,
          "point-light shadow matrix creates");
    float point[4] = {1, 2, 3, 1}, out[4] = {0};
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++)
            out[row] += shadow[col * 4 + row] * point[col];
    check(near(out[1] / out[3], 0) && near(out[0] / out[3], 0) && near(out[2] / out[3], 6),
          "shadow lands on independent ray-plane intersection");
}

static void skin(void)
{
    struct ol3d_vertex source[2] = {0}, out[2];
    struct ol3d_skin_weights weights[2] = {{{0, 1}, {1, 1}}, {{0, 1}, {1, 1}}};
    float bones[2][16];
    ol_mat4_identity(bones[0]);
    ol_mat4_identity(bones[1]);
    bones[1][12] = 4;
    for (int i = 0; i < 2; i++) {
        source[i].attribute[0][0] = 1;
        source[i].attribute[0][3] = 1;
        source[i].attribute[2][0] = source[i].attribute[2][1] = 1;
    }
    check(ol3d_skin_mesh(out, source, weights, 2, bones, 2) == OL_OK &&
              near(out[0].attribute[0][0], 3),
          "two-bone skin applies normalized weights");
    bones[0][0] = bones[1][0] = 2;
    check(ol3d_skin_mesh(out, source, weights, 2, bones, 2) == OL_OK &&
              near(out[0].attribute[2][0], 1 / sqrtf(5)) &&
              near(out[0].attribute[2][1], 2 / sqrtf(5)),
          "skin normals use inverse transpose under nonuniform scale");
    struct ol3d_vertex saved[2];
    memcpy(saved, out, sizeof out);
    weights[1].weights[0] = -1;
    check(ol3d_skin_mesh(out, source, weights, 2, bones, 2) != OL_OK &&
              !memcmp(out, saved, sizeof out),
          "late invalid weight preserves entire output");
    weights[1].weights[0] = 1;
    check(ol3d_skin_mesh(source, source, weights, 2, bones, 2) == OL_OK &&
              near(source[0].attribute[0][0], 4),
          "skin supports exact in-place transformation");
    struct ol3d_node a = {.parent = -1, .rotation = {0, 0, 0, 1}, .scale = {1, 1, 1}};
    struct ol3d_node b = a, mixed;
    b.position[0] = 4;
    b.rotation[2] = 1;
    b.rotation[3] = 0;
    check(ol3d_pose_mix(&mixed, &a, &b, 1, .5f) == OL_OK && near(mixed.position[0], 2) &&
              near(mixed.rotation[2], sqrtf(.5f)) && near(mixed.rotation[3], sqrtf(.5f)),
          "pose blend interpolates translation and quaternion independently");
}

static void ui(void)
{
    struct ol_ui ui;
    struct ol_ui_feedback feedback;
    ol_ui_init(&ui, 0);
    ol_ui_define(&ui, 0, (struct gfx_rect){10, 10, 100, 20});
    ol_ui_begin(&ui, 0, 0);
    ol_ui_enable(&ui, 0, 1);
    check(!ol_ui_needs_frame(&ui), "unchanged enabled state does not wake UI");
    ol_ui_feedback(&ui, 0, 1, &feedback);
    ol_ui_begin(&ui, 100000000, 0);
    ol_ui_feedback(&ui, 0, 1, &feedback);
    check(feedback.value > 0 && feedback.value < 1 && ui.active,
          "UI spring has real intermediate value");
    ol_ui_begin(&ui, 1000000000, 0);
    ol_ui_feedback(&ui, 0, 1, &feedback);
    check(near(feedback.value, 1) && !ol_ui_needs_frame(&ui), "completed UI spring stops wakeups");
    ol_ui_event(&ui, &(struct ol_ui_event){OL_UI_DOWN, 60, 15});
    float value = 0;
    check(ol_ui_slider_value(&ui, 0, &value) && near(value, .5f),
          "captured slider maps pointer to value");
    ol_ui_event(&ui, &(struct ol_ui_event){OL_UI_UP, 200, 15});
    check(!ol_ui_take_activation(&ui, 0), "release outside does not activate button");
    ol_ui_focus(&ui, 0);
    ol_ui_event(&ui, &(struct ol_ui_event){OL_UI_ACTIVATE, 0, 0});
    check(ol_ui_take_activation(&ui, 0) && !ol_ui_take_activation(&ui, 0),
          "keyboard activation is consumed exactly once");
    ol_ui_begin(&ui, 1100000000, 1);
    ol_ui_feedback(&ui, 0, 0, &feedback);
    check(feedback.value == 0 && !ui.active,
          "reduced motion applies endpoint without animation wakeup");
}

static void shaders(void)
{
    struct ol_lsl_shader *vertex = NULL, *fragment = NULL;
    struct ol_shader_error error = {0};
    int status = ol_lsl_compile(studio_vertex, strlen(studio_vertex), &vertex, &error);
    if (status)
        fprintf(stderr, "vertex: %s\n", error.message);
    check(status == OL_OK, "Scene Studio vertex compiles within IR budget");
    status = ol_lsl_compile(studio_fragment, strlen(studio_fragment), &fragment, &error);
    if (status)
        fprintf(stderr, "fragment: %s\n", error.message);
    check(status == OL_OK, "Scene Studio fragment compiles");
    struct ol3d_pipeline_object *pipeline = NULL;
    struct ol_lsl_raster_state raster = {.depth_test = 1, .depth_write = 1};
    check(ol_lsl_pipeline_create(vertex, fragment, &raster, &pipeline, &error) == OL_OK,
          "scene typed shader stages link");
    float uniforms[17][4] = {0};
    uniforms[15][3] = 1;
    uniforms[16][0] = 1;
    unsigned char texel[4] = {128, 64, 32, 255};
    struct ol3d_texture texture = {texel, 1, 1, 4, 4, 0, 0};
    struct ol3d_bindings bindings = {uniforms, 17, &texture, 1};
    float inputs[3][4] = {{.5f, 1, 1, 1}, {0}, {.25f, .25f, .25f, .25f}};
    struct ols_output output = {0};
    status = fragment ? ols_run(ol_lsl_ir(fragment), inputs, 3, &bindings, &output) : OL_ARGUMENT;
    check(status == OL_OK && near(output.color[0], .5f * 128 / 255 * .75f + .25f) &&
              near(output.color[1], 64 / 255.f * .75f),
          "scene fragment executes texture modulation and fog");
    ol3d_pipeline_destroy(pipeline);
    ol_lsl_destroy(vertex);
    ol_lsl_destroy(fragment);
}

int main(void)
{
    meshes();
    camera_and_shadow();
    skin();
    ui();
    shaders();
    printf("scene/ui: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
