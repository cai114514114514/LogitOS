/* refhost -- the host-side stand-in for everything under the browser that is
 * normally the kernel: a framebuffer surface to paint into, a font loader that
 * reads host files, and the image I/O the diff artefacts need.
 *
 * See tests/unit/refhost/logit.h for the shared/not-shared boundary. In one
 * line: the drawing and the text are the real kernel code, linked; this file
 * is the bench they are bolted to. */
#ifndef REFHOST_H
#define REFHOST_H

#include <stdint.h>

/* The surface the painter draws into. Mirrors c/kernel/gui/fb.h's struct
 * surface field-for-field -- refhost.c casts to it. */
extern int refhost_surf_w, refhost_surf_h;

/* Point the font loader at host files before refhost_fonts(). `ui` and `mono`
 * are host paths; either may be NULL to leave that slot unloaded. */
void refhost_font_map(const char *ui, const char *mono);

/* Load the fonts through the REAL c/kernel/gui/text.c text_init(). Returns 0 if
 * at least the UI font parsed. Idempotent: safe to call once per process. */
int  refhost_fonts(void);

/* Create (or resize) the paint surface and make it the fb target. The surface
 * is filled with `bg` first -- a reftest's canvas is white unless the page
 * paints over it, and "unpainted" must not read as black. */
void refhost_begin(int w, int h, uint32_t bg);

/* Stop targeting the surface and hand back the pixels (0x00RRGGBB, row-major,
 * refhost_surf_w * refhost_surf_h). Valid until the next refhost_begin. */
uint32_t *refhost_end(void);

/* --- comparison ------------------------------------------------------------
 * cmp() fills *maxdiff with the largest per-channel absolute difference over
 * the whole image and returns the number of pixels that differ AT ALL. Those
 * are exactly the two numbers a WPT `fuzzy` annotation is written in, so the
 * comparator answers the question the corpus asks rather than a proxy for it. */
long refhost_cmp(const uint32_t *a, const uint32_t *b, int w, int h, int *maxdiff);

/* Write a PNG (truecolour, 8-bit, stored-deflate). Returns 0 on success. */
int  refhost_png(const char *path, const uint32_t *px, int w, int h);

/* An amplified difference image: black where equal, and a hot colour whose
 * intensity grows with the per-channel delta where not. */
void refhost_diffimg(const uint32_t *a, const uint32_t *b, uint32_t *out, int n);

#endif /* REFHOST_H */
