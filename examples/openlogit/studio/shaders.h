#ifndef STUDIO_SHADERS_H
#define STUDIO_SHADERS_H

/* Gouraud lighting keeps the software fragment workload bounded. Smooth normal
 * interpolation is not advertised as per-pixel lighting: diffuse/specular terms
 * are computed at mesh vertices, then perspective-interpolated by the SDK. */
static const char studio_vertex[] =
    "lsl 1 vertex\n"
    "layout(location=0) in vec4 position;\n"
    "layout(location=2) in vec3 normal;\n"
    "layout(location=3) in vec2 uv;\n"
    "layout(location=0) uniform mat4 mvp;\n"
    "layout(location=4) uniform mat4 model;\n"
    "layout(location=8) uniform mat4 normalMatrix;\n"
    "layout(location=12) uniform vec4 light;\n"
    "layout(location=13) uniform vec4 eye;\n"
    "layout(location=14) uniform vec4 material;\n"
    "layout(location=15) uniform vec4 options;\n"
    "layout(location=0) out vec4 lit;\n"
    "layout(location=1) out vec2 texcoord;\n"
    "layout(location=2) out float fog;\n"
    "void main() {\n"
    "  gl_Position = mvp * position;\n"
    "  vec3 world = (model * position).xyz;\n"
    "  vec3 n = normalize((normalMatrix * vec4(normal, 0.0)).xyz);\n"
    "  vec3 direction = normalize(light.xyz - world);\n"
    "  float diffuse = max(dot(n, direction), 0.0);\n"
    "  float bands = 0.2 + step(0.3, diffuse) * 0.35 + step(0.7, diffuse) * 0.45;\n"
    "  diffuse = mix(diffuse, bands, options.y);\n"
    "  vec3 viewDirection = normalize(eye.xyz - world);\n"
    "  vec3 halfDirection = normalize(direction + viewDirection);\n"
    "  float specular = max(dot(n, halfDirection), 0.0);\n"
    "  specular = specular * specular;\n"
    "  specular = specular * specular;\n"
    "  specular = specular * specular;\n"
    "  lit = vec4(material.rgb * (0.22 + diffuse * 0.78) + specular * options.x, 1.0);\n"
    "  texcoord = uv * 3.0;\n"
    "  vec3 distance = eye.xyz - world;\n"
    "  fog = clamp(dot(distance, distance) * options.z, 0.0, 0.7);\n"
    "}\n";

static const char studio_fragment[] =
    "lsl 1 fragment\n"
    "layout(location=0) in vec4 lit;\n"
    "layout(location=1) in vec2 uv;\n"
    "layout(location=2) in float fog;\n"
    "layout(location=15) uniform vec4 options;\n"
    "layout(location=16) uniform vec4 fogColor;\n"
    "layout(binding=0) uniform sampler2D image;\n"
    "layout(location=0) out vec4 color;\n"
    "void main() {\n"
    "  vec4 pattern = mix(vec4(1.0), texture(image, uv), options.w);\n"
    "  color = vec4(mix((lit * pattern).rgb, fogColor.rgb, fog), 1.0);\n"
    "}\n";

static const char studio_flat_vertex[] =
    "lsl 1 vertex\n"
    "layout(location=0) in vec4 position;\n"
    "layout(location=0) uniform mat4 mvp;\n"
    "void main() { gl_Position = mvp * position; }\n";
static const char studio_flat_fragment[] =
    "lsl 1 fragment\n"
    "layout(location=14) uniform vec4 material;\n"
    "layout(location=0) out vec4 color;\n"
    "void main() { color = material; }\n";

#endif
