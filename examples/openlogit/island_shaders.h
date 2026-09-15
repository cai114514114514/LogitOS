#ifndef ISLAND_SHADERS_H
#define ISLAND_SHADERS_H
/* Editable LSL source programs. Flat lighting runs per vertex: normals
 * are constant on each face, so doing the same dot product per pixel wastes
 * interpreter work without adding detail. Normal matrix is inverse-transpose. */
static const char island_vertex_shader[] =
    "lsl 1 vertex\n"
    "layout(location=0) in vec4 position;\n"
    "layout(location=2) in vec3 normal;\n"
    "layout(location=3) in vec2 uv;\n"
    "layout(location=0) uniform mat4 transform;\n"
    "layout(location=4) uniform vec3 light;\n"
    "layout(location=6) uniform vec4 material;\n"
    "layout(location=7) uniform mat4 normalMatrix;\n"
    "layout(location=0) out vec4 lit;\n"
    "layout(location=1) out vec2 texcoord;\n"
    "void main() {\n"
    "    gl_Position = transform * position;\n"
    "    vec3 worldNormal = normalize((normalMatrix * vec4(normal, 0.0)).xyz);\n"
    "    float diffuse = max(dot(worldNormal, light), 0.35);\n"
    "    lit = vec4(material.rgb * diffuse, 1.0);\n"
    "    texcoord = uv;\n"
    "}\n";

static const char island_fragment_shader[] =
    "lsl 1 fragment\n"
    "layout(location=0) in vec4 lit;\n"
    "layout(location=1) in vec2 texcoord;\n"
    "layout(location=5) uniform vec4 glow;\n"
    "layout(binding=0) uniform sampler2D image;\n"
    "layout(location=0) out vec4 color;\n"
    "void main() {\n"
    "    color = lit * texture(image, texcoord) + glow;\n"
    "}\n";
#endif
