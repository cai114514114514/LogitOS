#ifndef ISLAND_SHADERS_H
#define ISLAND_SHADERS_H
/* Editable native OLS-IR programs. Flat lighting runs per vertex: normals
 * are constant on each face, so doing the same dot product per pixel wastes
 * interpreter work without adding detail. Normal matrix is inverse-transpose. */
static const char island_vertex_shader[]=
"ols 1 vertex\ninput r0 0\nmat4 r0 r0 0\nposition r0\n"
"input r2 2\nmat4 r2 r2 7\ndot3 r3 r2 r2\nrsqrt r3 r3\nmul r2 r2 r3\n"
"uniform r3 4\ndot3 r2 r2 r3\nconst r4 .35 .35 .35 .35\nmax r2 r2 r4\n"
"uniform r1 6\nmul r1 r1 r2\nconst r4 1 1 1 0\nmul r1 r1 r4\n"
"const r4 0 0 0 1\nadd r1 r1 r4\nvarying 0 r1\ninput r3 3\nvarying 1 r3\n";
static const char island_pixel_shader[]=
"ols 1 pixel\ninput r0 0\ninput r2 1\ntex2d r2 r2 0\nmul r0 r0 r2\n"
"uniform r5 5\nadd r0 r0 r5\ncolor r0\n";
#endif
