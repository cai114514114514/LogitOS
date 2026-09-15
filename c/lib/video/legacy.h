/* c/lib/video/legacy.h -- shared types for four archival VQ/RLE decoders:
 * Cinepak, Microsoft Video 1, Apple RPZA, QuickTime Animation ("qtrle").
 *
 * ARCHIVAL, NOT WEB: nothing in demux.c's sniff table, media.h's ops table,
 * Preview or js_media*.c points at these four files, and this phase does not
 * add such a wire -- that integration belongs to a workflow that owns those
 * files and is named, with the exact lines it needs, in each legacy_*.c's
 * header comment and in the phase report. These decode a shape of file that
 * stopped being made around 2005 (Video for Windows .avi, classic QuickTime
 * .mov). Nobody serves Cinepak over HTTP in 2026; the point is opening what
 * a machine already has sitting on a disk, not the live web -- breadth here
 * is not a priority claim against the codecs that are.
 *
 * All four are deterministic BLOCK-REPLACEMENT schemes over a persistent
 * frame buffer -- no transform, no entropy coder, no probability model, and
 * (Cinepak's YUV->RGB convert aside) no rounding -- so "matches ffmpeg's
 * decoder byte-for-byte" is exactly the right bar and the one tests/legacy.mk
 * holds every case to.
 *
 * STATEFUL BY CONSTRUCTION: every one of the four is an inter-frame delta
 * codec at heart (a "skip" block means "leave this block exactly as the
 * previous frame left it"), and Cinepak's codebooks persist across frames
 * the same way. So every decode context owns the previously-decoded picture
 * and must be reused call-to-call for one stream; a context's frame buffer
 * is undefined before the first frame that actually paints it (real files
 * always open on a keyframe, but nothing here assumes that of the caller --
 * an inter frame with no prior keyframe decodes into a still-undefined
 * buffer rather than fabricating one, matching every one of these formats'
 * own real-world lack of a defined default).
 *
 * OUTPUT CONVENTION -- chosen so the byte-exact gate is a plain memcmp
 * against `ffmpeg -f rawvideo`, and so no decoder does a color-space
 * conversion beyond the one Cinepak's OWN bitstream specifies:
 *   - gray8          1 byte/pixel Y            Cinepak palette-video mode
 *   - RGB24          3 bytes/pixel R,G,B        Cinepak 24bpp, QTRLE 24bpp
 *   - RGB555 (native native uint16, bit15=0,    MS Video 1 16-bit, RPZA,
 *             endian)  5R/5G/5B, matches         QTRLE 16bpp
 *             AV_PIX_FMT_RGB555
 *   - PAL8           1 byte/pixel index,        MS Video 1 8-bit,
 *                     caller supplies the        QTRLE 1/2/4/8bpp
 *                     256-entry RGB palette
 *   - ARGB32         4 bytes/pixel A,R,G,B       QTRLE 32bpp
 * A decoder never converts between these -- that would be a second place a
 * byte could go quietly wrong that a byte-exact diff against the format's
 * own native pixels would not catch.
 *
 * Every decode call is handed attacker-controlled bytes (an old media file
 * is an untrusted input like any other): every chunk/opcode/length is
 * bounds-checked before use, and a violation returns LEGACY_ERR_CORRUPT
 * rather than reading or writing outside the buffers the caller gave it.
 * tests/legacy.mk's negative controls exist to keep that true.
 */
#ifndef LOGIT_LEGACY_H
#define LOGIT_LEGACY_H

#include <stdint.h>

#define LEGACY_OK               0
#define LEGACY_ERR_CORRUPT     -1   /* bitstream violates the format */
#define LEGACY_ERR_UNSUPPORTED -2   /* valid per spec, a feature not implemented */
#define LEGACY_ERR_OOM         -3

/* ---------------------------------------------------------------- Cinepak */

#define CVID_MAX_STRIPS 32   /* real encoders use <=32; ffmpeg's own cap */

typedef struct {
    /* 12 bytes/entry, same layout cinepak.c builds: byte i*3+c for pixel i
     * (0..3), channel c (0..2, R/G/B or Y replicated x3 in mono mode). */
    uint8_t v[256][12];
} cvid_codebook;

typedef struct {
    int id, x1, y1, x2, y2;
} cvid_strip_geom;

typedef struct legacy_cinepak_ctx {
    int width, height;          /* true (display) size, as given at open */
    int rw, rh;                 /* (width+3)&~3, (height+3)&~3 -- decode grid */
    int gray;                   /* 1 = palette-video (gray8 out), 0 = RGB24 */
    int bpp;                    /* bytes/pixel actually written: 1 or 3 */
    /* rw*bpp stride, rh rows, persistent across frames. rw/rh (not
     * width/height) because a strip's block grid is allowed to overhang the
     * true edge on a non-multiple-of-4 frame (ffmpeg's own comment: "such
     * streams exist") -- a caller wanting the true picture reads `width`
     * columns of each of the first `height` rows and ignores the rest,
     * exactly what tests/legacy.mk's harness does before diffing against
     * ffmpeg's cropped `-f rawvideo` output. */
    uint8_t *frame;
    cvid_codebook v1_codebook[CVID_MAX_STRIPS];
    cvid_codebook v4_codebook[CVID_MAX_STRIPS];
    cvid_strip_geom strips[CVID_MAX_STRIPS];
    int nstrips;                /* strips valid from the last decoded frame */
} legacy_cinepak_ctx;

int legacy_cinepak_open(legacy_cinepak_ctx *c, int width, int height, int gray);
void legacy_cinepak_close(legacy_cinepak_ctx *c);
/* Decode one whole frame chunk (the bytes of one 'movi' '..dc'/'..db' AVI
 * sample). Returns LEGACY_OK or LEGACY_ERR_CORRUPT. On success c->frame
 * holds width columns x height rows, stride rw*bpp, top row first. */
int legacy_cinepak_decode(legacy_cinepak_ctx *c, const uint8_t *data, int size);

/* ------------------------------------------------------------ MS Video 1 */

typedef struct legacy_msvideo1_ctx {
    int width, height;
    int mode_8bit;               /* 0 = 16-bit RGB555, 1 = 8-bit palette index */
    uint8_t *frame;               /* mode_8bit? 1 : 2 bytes/pixel, width stride */
} legacy_msvideo1_ctx;

int legacy_msvideo1_open(legacy_msvideo1_ctx *c, int width, int height, int mode_8bit);
void legacy_msvideo1_close(legacy_msvideo1_ctx *c);
int legacy_msvideo1_decode(legacy_msvideo1_ctx *c, const uint8_t *data, int size);

/* ------------------------------------------------------------------ RPZA */

typedef struct legacy_rpza_ctx {
    int width, height;
    uint16_t *frame;             /* RGB555 words, width stride, persistent */
} legacy_rpza_ctx;

int legacy_rpza_open(legacy_rpza_ctx *c, int width, int height);
void legacy_rpza_close(legacy_rpza_ctx *c);
int legacy_rpza_decode(legacy_rpza_ctx *c, const uint8_t *data, int size);

/* ----------------------------------------------------- QuickTime Animation */

/* bits_per_coded_sample, exactly QuickTime's convention: 1/2/4/8 are
 * palette-indexed (add 32 for the "greyscale" variant, e.g. 40 = 8bpp
 * grayscale -- qtrle_decode_init groups depth and depth+32 into the same
 * decode routine, and so does this decoder: the low 5 bits select the
 * routine for those four, while 16/24/32 route on the raw value (32 & 0x1f
 * is 0, so that mask alone cannot be the whole rule). The caller decides
 * what a palette index from one of the `+32` depths' frames means -- this
 * file only decodes indices. */
typedef struct legacy_qtrle_ctx {
    int width, height;
    int depth;                   /* 1,2,4,8,16,24,32 (33/34/36/40 also legal) */
    int bpp_out;                 /* bytes/pixel actually stored: 1,2,3,4 */
    uint8_t *frame;               /* bpp_out * width stride, persistent */
} legacy_qtrle_ctx;

int legacy_qtrle_open(legacy_qtrle_ctx *c, int width, int height, int depth);
void legacy_qtrle_close(legacy_qtrle_ctx *c);
/* Decode one chunk (the full 'rle ' sample, chunk-size word included, same
 * as what a MOV 'stsz'/'stco' sample table names). */
int legacy_qtrle_decode(legacy_qtrle_ctx *c, const uint8_t *data, int size);

#endif /* LOGIT_LEGACY_H */
