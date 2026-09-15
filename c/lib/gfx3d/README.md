# OpenLogit software 3D

This directory contains the user-space software extension. The kernel keeps the
2D/animation core in `../gfx`; it does not compile the shader compiler or VM.

| Directory | Responsibility |
| --- | --- |
| `include/` | Public SDK headers; installed by basename |
| `render/` | Triangle pipeline, depth targets, buffers, immutable pipeline resources |
| `scene/` | Camera and matrix transforms, scene hierarchy, skinning |
| `material/` | Programmable screen-space materials composited through OpenLogit |
| `shader/ir/` | Legacy OLS-IR text assembly and native instruction validation |
| `shader/runtime/` | Bounded instruction execution and texture sampling |
| `shader/lsl/` | Typed LSL source compiler and stage linkage |

The LSL frontend is split by responsibility:

- `lexer.c`: bounded source scanning, tokenization, source-line diagnostics.
- `types.c`: type names and scalar/vector arithmetic compatibility.
- `parser.c`, `expressions.c`: declarations/statements and expression grammar.
- `symbols.c`: name resolution, assignments and persistent local values.
- `emit.c`: native instructions and temporary register ownership.
- `operators.c`, `constructors.c`, `builtins.c`: typed expression lowering.
- `compiler.c`, `link.c`: public compilation/reflection and typed stage linkage.

Emission helpers borrow values. Binary expression and call entry points consume
their argument temporaries. Reading a local copies its persistent register;
assigning a local transfers ownership of the expression result. Matrix and
sampler values carry binding slots separately from register indices.

Do not reintroduce root-level C wrappers that include child C files. Build lists
discover translation units recursively. The archive is recreated when its object
list changes so removed flat objects cannot silently remain in `libopenlogit.a`.

Run `make BUILD=/absolute/private/build test-openlogit-lsl test-openlogit-3d
test-openlogit-material`. These host checks verify compilation and numeric output;
they do not establish guest performance, displayed frames, or complete GLSL support.
