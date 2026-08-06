#ifndef LOGIT_IMG_H
#define LOGIT_IMG_H

#include <stdint.h>

/* Decoded image: straight 8-bit RGBA, w*h*4 bytes (kmalloc'd; free with img_free). */
struct image { int w, h; uint8_t *rgba; };

typedef int (*img_detect_fn)(const uint8_t *p, int n);            /* 1 if mine */
typedef int (*img_decode_fn)(const uint8_t *p, int n, struct image *out); /* 0 ok */

void img_register(img_detect_fn detect, img_decode_fn decode);
void img_init(void);                                  /* registers PNG + GIF + JPEG + SVG */
int  img_decode(const uint8_t *p, int n, struct image *out);  /* 0 ok, -1 unsupported/error */
void img_free(struct image *im);

void png_register(void);
void gif_register(void);
void jpeg_register(void);
void svg_register(void);

#endif /* LOGIT_IMG_H */
