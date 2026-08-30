#ifndef LOGIT_IMG_H
#define LOGIT_IMG_H

#include <stdint.h>

/* Decoded image: straight 8-bit RGBA, w*h*4 bytes (kmalloc'd; free with img_free). */
struct image { int w, h; uint8_t *rgba; };

/* --- animation -----------------------------------------------------------
 * A decoded animation is a list of ALREADY-COMPOSITED canvases: frame k is the
 * full w*h RGBA the screen should show for `delay_ms`, with the format's
 * disposal and blend rules already applied. That is deliberate. The alternative
 * (hand the caller sub-rectangles plus a disposal enum) pushes the part every
 * naive implementation gets wrong -- restore-to-background vs restore-to-
 * previous, and the fact that "background" means TRANSPARENT for both GIF and
 * APNG rather than the background-colour-index -- out into every renderer.
 * Compositing once, here, is also what makes the frames directly comparable
 * byte-for-byte with a reference decoder's per-frame output.
 *
 * A still image is just an animation with nframes == 1, so a caller that only
 * wants a picture keeps using img_decode. */
struct img_frame { int delay_ms; uint8_t *rgba; };  /* rgba = w*h*4, kmalloc'd */
struct img_anim {
    int w, h;
    int nframes;
    int loops;                    /* 0 = loop forever; n = play n times */
    struct img_frame *frames;     /* kmalloc'd array of nframes */
};

typedef int (*img_detect_fn)(const uint8_t *p, int n);            /* 1 if mine */
typedef int (*img_decode_fn)(const uint8_t *p, int n, struct image *out); /* 0 ok */
typedef int (*img_anim_fn)(const uint8_t *p, int n, struct img_anim *out); /* 0 ok */

void img_register(img_detect_fn detect, img_decode_fn decode);
/* Same, plus the animated path. `anim` may be 0 (format has no animation). */
void img_register_anim(img_detect_fn detect, img_decode_fn decode, img_anim_fn anim);
void img_init(void);                     /* registers PNG/GIF/JPEG/SVG/BMP/ICO/WEBP */
int  img_decode(const uint8_t *p, int n, struct image *out);  /* 0 ok, -1 unsupported/error */
void img_free(struct image *im);

/* Decode every frame. Formats without an animated path fall back to one frame
 * (delay 0, loops 1) built from img_decode, so callers need only one code path.
 * Returns 0 on success; free with img_anim_free. */
int  img_decode_anim(const uint8_t *p, int n, struct img_anim *out);
void img_anim_free(struct img_anim *a);

void png_register(void);
void gif_register(void);
void jpeg_register(void);
void svg_register(void);

/* Parse ONE CSS colour literal -- "#f0f", "rgba(1,2,3,.5)", "red" -- into
 * rgba[4]. 1 if it named a colour, 0 if not (rgba untouched, so the caller
 * keeps its previous value: what both SVG's default and Canvas's "ignore an
 * unparseable fillStyle" require). Implemented in svg.c over the parser that
 * file already had, so an SVG attribute and a canvas fillStyle cannot come to
 * disagree about what "rebeccapurple" is. This is NOT the cascade's parser and
 * must not become it -- LibCSS owns what a stylesheet means. */
int img_css_color(const char *s, int len, unsigned char *rgba);
void bmp_register(void);
void ico_register(void);
void webp_register(void);

/* --- EXIF ---------------------------------------------------------------
 * Orientation is metadata, not pixels: a phone photo is stored in the sensor's
 * frame and carries the rotation that makes it upright. A decoder that ignores
 * it is wrong on essentially every photo taken in portrait.
 * exif_orientation() returns 1..8 (the TIFF/EXIF tag 0x0112 values), or 1 when
 * absent/unparseable -- 1 is "already upright", so the no-metadata path costs
 * nothing. exif_apply() rewrites an image in place to orientation 1. */
int  exif_orientation(const uint8_t *p, int n);      /* whole JPEG/TIFF file */
int  exif_apply(struct image *im, int orientation);  /* 0 ok, -1 alloc failure */

/* Shared by ico.c (icons hold whole PNGs) and the BMP-in-ICO path. */
int  bmp_decode_dib(const uint8_t *p, int n, int is_icon, struct image *out);

/* --- ENCODING -------------------------------------------------------------
 * The direction this tree could not go until 2026-08-29. Everything above
 * READS an image; `rust/src/pngenc.rs` writes one -- straight (non-
 * premultiplied) RGBA8 in, a conforming PNG out (colour type 6, depth 8, no
 * interlace, filter None, a zlib stream of RFC 1951 STORED deflate blocks).
 * Its whole reason for existing is that a canvas readback must report what was
 * actually drawn: `js_canvas.c`'s toDataURL threw for years precisely because
 * a FABRICATED data URL is believed rather than detected, and the way past
 * that was to remove the premise rather than relax the rule.
 *
 * No compression: the output is ~1.001x the raw RGBA. That is a size cost and
 * not a correctness one, and a real deflate slots in behind this same
 * signature later.
 *
 * FREE IT WITH png_encode_free AND NOT WITH free(). The Rust staticlib links
 * into two domains with two different allocators (the kernel heap; mini-libc's
 * arena via browser_rt.c's kmalloc shim), and pairing the allocation with its
 * own free is the only form that is right in both without the caller having to
 * know which one it is in. Returns 0 on a bad size or OOM, and writes the
 * length through out_len (which is zeroed first, so a caller that checks only
 * the length still sees a failure). */
uint8_t *png_encode_rgba(const uint8_t *px, int w, int h, int *out_len);
void     png_encode_free(uint8_t *p);

#endif /* LOGIT_IMG_H */
