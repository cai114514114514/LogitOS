#ifndef OPENLOGIT_UI_SHADERS_H
#define OPENLOGIT_UI_SHADERS_H
/* Editable native OLS-IR, shared with 3D. There are no callbacks or widget
 * opcodes: app-owned state becomes ordinary uniforms and textures. */
static const char *ui_progress_shader =
    "ols 1 pixel\ninput r0 0\nswizzle r1 r0 0\nuniform r2 0\n"
    "sub r3 r2 r1\nuniform r4 1\nuniform r5 2\nselect r6 r3 r4 r5\ncolor r6\n";
static const char *ui_shimmer_shader =
    "ols 1 pixel\ninput r0 0\nswizzle r1 r0 0\nuniform r2 0\nsub r3 r1 r2\n"
    "const r4 6.283185 6.283185 6.283185 6.283185\nmul r5 r3 r4\ncos r6 r5\n"
    "const r7 0 0 0 0\nmax r8 r6 r7\nconst r9 .12 .19 .25 0\n"
    "const r10 .13 .20 .30 1\nmad r11 r8 r9 r10\ncolor r11\n";
static const char *ui_ripple_shader =
    "ols 1 pixel\ninput r0 0\nconst r1 .5 .5 0 1\nsub r2 r0 r1\n"
    "const r3 4.375 1 0 0\nmul r4 r2 r3\ndot3 r5 r4 r4\nuniform r6 0\n"
    "sub r7 r6 r5\nconst r8 30 30 30 30\nmul r9 r7 r8\n"
    "const r10 0 0 0 0\nconst r11 1 1 1 1\nmax r12 r9 r10\nmin r13 r12 r11\n"
    "uniform r14 1\nconst r15 0 0 0 1\nmul r16 r13 r15\n"
    "mul r17 r16 r14\nconst r18 .42 .92 .83 0\nadd r19 r17 r18\ncolor r19\n";
static const char *ui_texture_shader =
    "ols 1 pixel\ninput r0 0\ntex2d r1 r0 0\nuniform r2 0\nmul r3 r1 r2\ncolor r3\n";
#endif
