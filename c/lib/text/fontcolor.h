#ifndef LOGIT_FONTCOLOR_H
#define LOGIT_FONTCOLOR_H

#include <stdint.h>
#include "ttf.h"
/* text_raster_extent + text_raster_at, which this header used to declare
 * itself. They moved to glyphras.h with the rest of the rasterizer entry
 * points when c/kernel/gui/raster.c was deleted; the include keeps every
 * existing caller of this header working unchanged. */
#include "glyphras.h"

/* Colour glyphs: COLR/CPAL (layered vector) and the bitmap strikes, CBDT/CBLC
 * and sbix. Emoji are not decoration on a system that renders web pages -- a
 * page whose text is half emoji renders as half tofu without this.
 *
 * THE IMPLEMENTED RENDERING PATH IS COLR/CPAL (version 0).
 *   A COLR glyph is a list of (glyph id, palette index) layers, each layer an
 *   ordinary outline in the same font. That is exactly what the existing
 *   rasterizer already draws, so a colour glyph costs one coverage pass per
 *   layer and a palette lookup -- no new rasterizer, no image decoder, and it
 *   stays sharp at every size. text_raster_extent + text_raster_at (declared at
 *   the bottom of this header) are the two rasterizer entry points that make the
 *   layers land in one shared bitmap box.
 *
 * COLR version 1 (gradients, transforms, compositing modes) is DETECTED and
 * REFUSED, not half-drawn: colr_version() reports 1, and colr_layers() still
 * returns the v0 records the font keeps for compatibility, so a v1 font renders
 * with its v0 fallback layers rather than wrongly.
 *
 * CBDT/CBLC and sbix are parsed to the point of LOCATING the embedded image and
 * its metrics -- they hold PNG (or JPEG/TIFF for sbix), and decoding an image is
 * the image line's job, not this one's. cbdt_lookup/sbix_lookup hand back a
 * pointer into the font blob plus the format tag; nothing is copied.
 *
 * All reads are bounds-checked; no allocation, no globals. */

/* ------------------------------------------------------------ COLR / CPAL -- */

struct colr_layer {
    uint16_t gid;                /* the outline to draw for this layer */
    uint16_t palette_index;      /* index into the CPAL palette; 0xFFFF = use the
                                  * text foreground colour, per the spec */
};

/* COLR table version, or -1 if there is no COLR. 0 = layers only (fully drawn),
 * 1 = the gradient/transform extension (its v0 records are still usable). */
int colr_version(const struct ttf_font *f);

/* Layers of `gid`. Writes up to `cap` into `out` and returns the true layer
 * count, or -1 when the glyph has no COLR record (draw it as a normal glyph).
 * A return of 0 means the record exists but is empty. */
int colr_layers(const struct ttf_font *f, uint16_t gid, struct colr_layer *out, int cap);

/* Palette count and entries-per-palette from CPAL; 0 if there is no CPAL. */
int cpal_palette_count(const struct ttf_font *f);
int cpal_entry_count(const struct ttf_font *f);

/* Colour `index` of palette `pal` as 0xAARRGGBB. Returns 0 (fully transparent)
 * when the palette or index does not exist, which draws nothing -- the safe
 * failure for a malformed font. */
uint32_t cpal_color(const struct ttf_font *f, int pal, int index);

/* --------------------------------------------------------- bitmap strikes -- */

/* graphic formats an embedded bitmap can be in. */
enum { FONTIMG_UNKNOWN = 0, FONTIMG_PNG, FONTIMG_JPEG, FONTIMG_TIFF };

struct font_bitmap {
    const uint8_t *data;         /* into the font blob; NOT owned, NOT decoded */
    uint32_t len;
    int format;                  /* FONTIMG_* */
    int ppem;                    /* the strike's pixels-per-em */
    int width, height;           /* pixels, 0 when the strike does not say */
    int bearing_x, bearing_y;    /* pixels, from the pen origin to the top-left */
    int advance;                 /* pixels, 0 when the strike does not say */
};

/* Best CBDT/CBLC bitmap for `gid` at `want_ppem` (the strike nearest at or above
 * it, else the largest). Returns 0 on success, -1 if absent. */
int cbdt_lookup(const struct ttf_font *f, uint16_t gid, int want_ppem,
                struct font_bitmap *out);

/* Same, for sbix. bearing_y is derived from the strike's origin offsets, so a
 * caller can place both kinds identically. */
int sbix_lookup(const struct ttf_font *f, uint16_t gid, int want_ppem,
                struct font_bitmap *out);

/* Either of the above, whichever the font has. */
int font_bitmap_lookup(const struct ttf_font *f, uint16_t gid, int want_ppem,
                       struct font_bitmap *out);

/* The two rasterizer entry points a COLR composite needs -- text_raster_extent
 * to fix ONE bitmap box, text_raster_at to paint every layer into it -- are
 * declared in glyphras.h, included at the top of this header. */

#endif /* LOGIT_FONTCOLOR_H */
