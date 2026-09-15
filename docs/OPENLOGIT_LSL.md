# LSL 1 source language

LSL is OpenLogit's typed shader source language. Programs use `vertex` and
`fragment` stage names and compile into the existing OLS-IR v1 execution format.
Legacy `ols 1 vertex` / `ols 1 pixel` programs and binary layouts remain valid.
This is a bounded native language, not a claim of GLSL conformance.

```c
lsl 1 vertex
layout(location=0) in vec4 position;
layout(location=0) uniform mat4 transform;
layout(location=0) out vec2 uv;

void main() {
    gl_Position = transform * position;
    uv = position.xy * 0.5 + vec2(0.5);
}
```

```c
lsl 1 fragment
layout(location=0) in vec2 uv;
layout(binding=0) uniform sampler2D image;
layout(location=0) out vec4 color;

void main() {
    vec4 sampled = texture(image, uv);
    color = sampled * vec4(0.8, 0.9, 1.0, 1.0);
}
```

The first frontend supports initialized locals, assignment, parentheses, unary
signs, `+ - * /`, scalar float comparisons, numeric constructors and read swizzles
(`xyzw` or `rgba`, without mixing alphabets). Types are `float`, `vec2`, `vec3`,
`vec4`, `bool`, and uniform-only `mat4` / `sampler2D`. A matrix occupies four
consecutive column-major uniform slots; matrix multiplication currently accepts
`mat4 * vec4`. A scalar expands across vector arithmetic. Assignments require
matching types; conversions are explicit constructors.

Built-ins are `texture`, `dot`, `normalize`, `sin`, `cos`, `abs`, `min`, `max`,
`step`, `clamp`, `mix`, and `select(bool, when_true, when_false)`. Scalar arguments
can expand to the vector width of numeric arguments. `select` evaluates both
values; it is not a short-circuit branch. Invalid arithmetic such as division by
zero or normalizing a zero vector fails execution through the existing VM.

Only `void main()` is supported. Loops, user functions, recursion, arrays,
swizzle assignments, `if`, and ternary expressions are not implemented and are
rejected. The limits are 128 KiB of source, 64 live registers, 256 native
instructions, 128 symbols, 64 reflected bindings and 64 expression nesting levels.

Include `openlogit_lsl.h` and call `ol_lsl_compile`, then either use `ol_lsl_ir`
with the existing material API or link vertex and fragment with
`ol_lsl_pipeline_create`. Compile and pipeline creation return `OL_OK` on success.
Output pointers are null on failure. Shader objects own their code and reflection;
created pipelines copy code, so source shader objects can then be destroyed.
Reflection lookups return 1 for a match and 0 for an absent/invalid entry.

The SDK installs `/bin/lslcc input.lsl output.olsb` alongside the legacy `/bin/olscc`.
`examples/openlogit/passthrough.lsl` is a minimal fragment program. Sky Islands
compiles the readable LSL vertex/fragment sources in `island_shaders.h` and creates
its mesh pipeline through typed linkage; no private shader executor is added.

Compilation checks declaration ranges, overlapping slots, symbol lifetimes and
expression types. Linkage checks varying locations/types and uniform slot
compatibility between stages. Source diagnostics use one-based lines; native
validation and linkage diagnostics use line 0. The existing renderer validates
actual uniform and texture resources when drawing. GPU support remains unavailable.

`test-openlogit-lsl` compiles source and executes the resulting programs against
independent numeric expectations, including padding lanes, scalar conversion,
matrix transforms, texture reads, type conflicts and malformed source. Its VM
disabled negative control must visibly fail the numeric oracle. ASan/UBSan runs
the same checks. Guest SDK compilation and performance require separate evidence.
