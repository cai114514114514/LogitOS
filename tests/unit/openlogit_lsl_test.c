#include "openlogit_lsl.h"
#include "ols_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static unsigned failures;

static void check(int condition, const char *name)
{
    checks++;
    if (!condition)
        failures++;
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
}

static struct ol_lsl_shader *compile(const char *source)
{
    struct ol_lsl_shader *shader = NULL;
    struct ol_shader_error error = {0};
    int status = ol_lsl_compile(source, strlen(source), &shader, &error);
    if (status != OL_OK)
        fprintf(stderr, "compile line %u: %s\n", error.line, error.message);
    return shader;
}

static int close_color(const float actual[4], const float expected[4])
{
    for (unsigned lane = 0; lane < 4; lane++) {
        if (!isfinite(actual[lane]) || fabsf(actual[lane] - expected[lane]) > 0.0001f)
            return 0;
    }
    return 1;
}

static void check_expression(const char *expression, const float expected[4],
                             const char *name)
{
    char source[4096];
    snprintf(source, sizeof source,
             "lsl 1 fragment\nlayout(location=0) out vec4 color;\n"
             "void main() { color = %s; }", expression);
    struct ol_lsl_shader *shader = compile(source);
    struct ols_output output = {0};
    struct ol3d_bindings bindings = {0};
    int status = shader ? ols_run(ol_lsl_ir(shader), NULL, 0, &bindings, &output) : OL_ARGUMENT;
    check(status == OL_OK && close_color(output.color, expected), name);
    ol_lsl_destroy(shader);
}

static void test_numeric_output(void)
{
    check_expression("vec4(0.25, 0.5, 0.75, 1.0)", (float[]){.25f, .5f, .75f, 1},
                     "LSL constructor executes in native VM");
    check_expression("vec4(1.0) * float(vec3(2.0, 7.0, 9.0))", (float[]){2, 2, 2, 2},
                     "scalar conversion broadcasts the first vector component");
    check_expression("vec4(vec2(8.0, 6.0) / vec2(2.0, 3.0), 0.0, 1.0)",
                     (float[]){4, 2, 0, 1}, "vec2 division ignores unused zero lanes");
    check_expression("vec4(vec3(6.0) / vec3(2.0), 1.0)", (float[]){3, 3, 3, 1},
                     "vec3 division ignores unused zero lane");
    check_expression("vec4(vec2(vec4(1.0, 2.0, 3e38, 3e38)) + "
                     "vec2(vec4(3.0, 4.0, 3e38, 3e38)), 0.0, 1.0)",
                     (float[]){4, 6, 0, 1}, "discarded vector components cannot overflow later arithmetic");
    check_expression("vec4(2.0 + 3.0 * 4.0, (2.0 + 3.0) * 4.0, -2.0, +3.0)",
                     (float[]){14, 20, -2, 3}, "operator precedence and unary signs");
    check_expression("vec4(vec3(2.0, 3.0, 4.0).zyx, 1.0)", (float[]){4, 3, 2, 1},
                     "swizzle preserves component order");
    check_expression("vec4(dot(vec2(3.0), vec2(4.0)))", (float[]){24, 24, 24, 24},
                     "dot ignores broadcast vector padding");
    check_expression("vec4(normalize(vec2(3.0, 4.0)), 0.0, 1.0)",
                     (float[]){.6f, .8f, 0, 1}, "normalize matches independent vector result");
    check_expression("mix(0.0, 1.0, vec4(0.0, 0.25, 0.75, 1.0))",
                     (float[]){0, .25f, .75f, 1}, "mix propagates vector result type");
    check_expression("clamp(vec4(-2.0, 0.25, 0.75, 4.0), 0.0, 1.0)",
                     (float[]){0, .25f, .75f, 1}, "clamp applies scalar bounds to vectors");
    check_expression("vec4(sin(0.0), cos(0.0), abs(-3.0), step(1.0, 1.0))",
                     (float[]){0, 1, 3, 1}, "unary built-ins and inclusive step");
    check_expression("vec4(select(2.0 < 2.0, 1.0, 0.0), select(2.0 <= 2.0, 1.0, 0.0),"
                     "select(3.0 > 2.0, 1.0, 0.0), select(1.0 >= 2.0, 1.0, 0.0))",
                     (float[]){0, 1, 1, 0}, "strict and inclusive comparisons select correctly");
    check_expression("vec4(select(2.0 == 2.0, 1.0, 0.0), select(2.0 != 2.0, 1.0, 0.0),"
                     "select(true, 1.0, 0.0), select(false, 1.0, 0.0))",
                     (float[]){1, 0, 1, 0}, "equality and boolean selection preserve false");

    char expression[1024] = "vec4(1.0";
    for (unsigned term = 1; term < 80; term++)
        strcat(expression, "+1.0");
    strcat(expression, ")");
    check_expression(expression, (float[]){80, 80, 80, 80},
                     "temporary registers are reused across long expressions");
}

static void test_bindings_and_link(void)
{
    const char *vertex_source =
        "lsl 1 vertex\n"
        "layout(location=0) in vec4 position;\n"
        "layout(location=0) uniform mat4 transform;\n"
        "layout(location=4) uniform float gain;\n"
        "layout(location=0) out vec2 uv;\n"
        "void main() {\n"
        "  float local = gain; local = local + local;\n"
        "  gl_Position = transform * position;\n"
        "  uv = vec2(local);\n"
        "}";
    const char *fragment_source =
        "lsl 1 fragment\nlayout(location=0) in vec2 uv;\n"
        "layout(binding=0) uniform sampler2D image;\n"
        "layout(location=0) out vec4 color;\n"
        "void main() { color = texture(image, uv); }";
    struct ol_lsl_shader *vertex = compile(vertex_source);
    struct ol_lsl_shader *fragment = compile(fragment_source);
    float uniforms[5][4] = {
        {2, 0, 0, 0}, {0, 3, 0, 0}, {0, 0, 4, 0}, {5, 6, 7, 1},
        {.25f, 20, 30, 40}
    };
    const float inputs[1][4] = {{1, 2, 3, 1}};
    struct ol3d_bindings bindings = {.uniforms = uniforms, .uniform_count = 5};
    struct ols_output output = {0};
    int status = vertex ? ols_run(ol_lsl_ir(vertex), inputs, 1, &bindings, &output) : OL_ARGUMENT;
    check(status == OL_OK && close_color(output.position, (float[]){7, 12, 19, 1}),
          "column-major matrix binding transforms vertex position");
    check(status == OL_OK && output.varying[0][0] == .5f && output.varying[0][1] == .5f,
          "local reassignment preserves scalar uniform broadcast");

    struct ol_lsl_binding reflection = {0};
    check(ol_lsl_binding_count(vertex) == 4 &&
          ol_lsl_find_binding(vertex, "transform", &reflection) &&
          reflection.type == LSL_MAT4 && reflection.location == 0,
          "reflection retains matrix type and binding location");
    check(!ol_lsl_binding(vertex, 99, &reflection) &&
          !ol_lsl_find_binding(vertex, "missing", &reflection),
          "reflection reports absent entries");

    unsigned char pixel[4] = {64, 128, 192, 255};
    struct ol3d_texture texture = {.pixels = pixel, .width = 1, .height = 1, .stride = 4, .bytes = 4};
    bindings = (struct ol3d_bindings){.textures = &texture, .texture_count = 1};
    float varyings[OL3D_MAX_VARYINGS][4];
    memcpy(varyings, output.varying, sizeof varyings);
    status = fragment ? ols_run(ol_lsl_ir(fragment), varyings, 1, &bindings, &output) : OL_ARGUMENT;
    check(status == OL_OK && close_color(output.color, (float[]){64/255.f, 128/255.f, 192/255.f, 1}),
          "fragment sampler binding produces actual texel color");

    struct ol_lsl_raster_state state = {.depth_test = 1, .depth_write = 1};
    struct ol3d_pipeline_object *pipeline = NULL;
    struct ol_shader_error error = {0};
    status = ol_lsl_pipeline_create(vertex, fragment, &state, &pipeline, &error);
    check(status == OL_OK && pipeline != NULL, "typed vertex and fragment pipeline links");
    ol3d_pipeline_destroy(pipeline);
    ol_lsl_destroy(fragment);
    fragment = compile("lsl 1 fragment\nlayout(location=0) in vec3 uv;"
                       "layout(location=0) out vec4 color; void main() { color = vec4(uv, 1.0); }");
    pipeline = NULL;
    status = ol_lsl_pipeline_create(vertex, fragment, &state, &pipeline, &error);
    check(status != OL_OK && !pipeline && strstr(error.message, "types differ"),
          "typed linkage rejects differing varying widths");
    ol_lsl_destroy(fragment);
    fragment = compile("lsl 1 fragment\nlayout(location=1) uniform vec4 conflict;"
                       "layout(location=0) out vec4 color; void main() { color = conflict; }");
    status = ol_lsl_pipeline_create(vertex, fragment, &state, &pipeline, &error);
    check(status != OL_OK && !pipeline && strstr(error.message, "uniform"),
          "typed linkage rejects partial matrix uniform overlap");
    ol_lsl_destroy(fragment);
    ol_lsl_destroy(vertex);
}

static void test_diagnostics(void)
{
    static const struct {
        const char *source;
        const char *diagnostic;
    } cases[] = {
        {"lsl 1 pixel", "vertex or fragment"},
        {"lsl 1 fragment\n/*", "unterminated"},
        {"lsl 1 fragment\nlayout(location=1e+) out vec4 c;", "exponent"},
        {"lsl 1 fragment\nlayout(location=1e99) out vec4 c;", "finite"},
        {"lsl 1 vertex\nlayout(location=254) uniform mat4 m;", "range"},
        {"lsl 1 vertex\nlayout(location=0) uniform mat4 m; layout(location=2) uniform float f;", "overlap"},
        {"lsl 1 fragment\nlayout(location=0) uniform sampler2D s;", "binding"},
        {"lsl 1 vertex\nvoid main() {}", "never assigned"},
        {"lsl 1 fragment\nvoid main() {}", "requires an output"},
        {"lsl 1 vertex\nvoid main() { vec2 a = vec2(1.0); gl_Position = vec4(a.z); }", "swizzle"},
        {"lsl 1 vertex\nvoid main() { gl_Position = vec4(vec2(1.0), 2.0); }", "component count"},
        {"lsl 1 vertex\nvoid main() { gl_Position = vec2(1.0); }", "types"},
        {"lsl 1 vertex\nvoid main() { gl_Position = vec4(1.0) + vec3(1.0); }", "widths"},
        {"lsl 1 vertex\nvoid main() { float x = x; gl_Position = vec4(x); }", "before assignment"},
        {"lsl 1 vertex\nvoid main() { gl_Position = vec4(1.0); while (true) {} }", "unsupported statement"},
        {"lsl 1 vertex\nvoid main() { gl_Position = vec4(1.0,); }", "missing argument"}
    };
    for (unsigned index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        struct ol_lsl_shader *shader = (void *)1;
        struct ol_shader_error error = {0};
        int status = ol_lsl_compile(cases[index].source, strlen(cases[index].source), &shader, &error);
        check(status != OL_OK && shader == NULL && error.line > 0 &&
              strstr(error.message, cases[index].diagnostic), cases[index].diagnostic);
    }
    const char source[] = "lsl 1 vertex\n\0void main() {}";
    struct ol_lsl_shader *shader = NULL;
    struct ol_shader_error error = {0};
    check(ol_lsl_compile(source, sizeof source - 1, &shader, &error) != OL_OK &&
          !shader && error.line == 2, "embedded NUL is rejected at its actual source line");
}

int main(void)
{
    test_numeric_output();
    test_bindings_and_link();
    test_diagnostics();
    printf("LSL: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
