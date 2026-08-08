#include "img.h"

void *kmalloc(unsigned long);
void  kfree(void *);
void *memcpy(void *, const void *, unsigned long);
void *memset(void *, int, unsigned long);

/* GIF decoder: the LOGICAL SCREEN, every frame, the delays and the disposal.
 *
 * The old decoder read the first image descriptor and stopped, which on the web
 * means it supported the one use of GIF that has been obsolete since PNG
 * shipped. Animation is what the format is still for.
 *
 * Three things a first-frame decoder never has to get right, and which are
 * therefore where the bugs are:
 *
 *  1. A frame is a SUB-RECTANGLE of the logical screen, not the picture. Most
 *     encoders shrink each frame to the pixels that changed, so frame 2 of a
 *     500x300 animation is often 40x18 at some offset. The output canvas is
 *     always the logical screen; the frame is composited into it.
 *
 *  2. Transparent index means "leave what is underneath", not "write
 *     transparent black". Compositing, not copying.
 *
 *  3. DISPOSAL says what to do with the frame's rectangle *after* it has been
 *     shown, and it is the field everyone gets wrong:
 *       0/1 leave it (the next frame draws on top),
 *       2   "restore to background" -- which in every browser, and in the only
 *           interpretation that makes transparent GIFs work, means clear the
 *           rectangle to TRANSPARENT, not to the background colour index,
 *       3   "restore to previous" -- put back whatever the canvas held BEFORE
 *           this frame was drawn, which means snapshotting it first.
 *     Get 2 wrong and a moving sprite smears; get 3 wrong and it smears only on
 *     the frames that used it. Neither is visible in "it decoded N frames", so
 *     tests/unit/img_anim_test.c asserts the composited canvas per frame.
 *
 * SECURITY: every input byte is untrusted. Sub-block walks are bounded by the
 * buffer, the LZW dictionary is index-checked, frame rectangles are clipped to
 * the canvas, and both the frame count and the total canvas memory are capped
 * so that a 40-byte header cannot ask for gigabytes. */

#define MAXFRAMES 1024
#define MAXANIMBYTES (192u << 20)      /* total RGBA across all frames */

static int gif_detect(const uint8_t *p, int n)
{ return n >= 6 && p[0]=='G'&&p[1]=='I'&&p[2]=='F'&&p[3]=='8'; }

/* LZW decode GIF image data into `idx` (capacity cap). Returns #indices or -1.
 * `data` is the raw stream starting at the LZW-min-code-size byte; sub-blocks
 * are length-prefixed and 0-terminated. `*consumed` gets the number of bytes of
 * `data` the image occupied (through the terminating 0 block), so the caller
 * can continue the block walk. */
static int gif_lzw(const uint8_t *data, int dlen, uint8_t *idx, int cap, int *consumed)
{
    if (dlen < 1) { if (consumed) *consumed = 0; return -1; }
    int mincode = data[0];
    if (mincode < 2 || mincode > 11) { if (consumed) *consumed = 1; return -1; }   /* legal min code size is 2-8; 12+ overflows the 4096-entry dict, >=31 is UB shift */
    int clear = 1 << mincode, eoi = clear + 1;
    int codesize = mincode + 1, next = eoi + 1;
    /* dictionary: prefix[] + suffix[]; entries up to 4096 */
    static int prefix[4096]; static uint8_t suffix[4096]; static uint8_t stack[4096];
    int oi = 0;
    /* bit reader over sub-blocks */
    int pos = 1, blkrem = 0; uint32_t bits = 0; int nbits = 0;
    #define NEEDBYTE() do { \
        if (blkrem == 0) { if (pos >= dlen) { goto done; } blkrem = data[pos++]; if (blkrem == 0) goto done; } \
        if (pos >= dlen) goto done; /* malformed sub-block length must not over-read the stream */ \
        bits |= (uint32_t)data[pos++] << nbits; nbits += 8; blkrem--; } while (0)
    int prev = -1;
    for (;;) {
        while (nbits < codesize) NEEDBYTE();
        int code = bits & ((1 << codesize) - 1);
        bits >>= codesize; nbits -= codesize;
        if (code == clear) { codesize = mincode + 1; next = eoi + 1; prev = -1; continue; }
        if (code == eoi) break;
        int sp = 0, cur = code;
        if (cur == next && prev >= 0) { stack[sp++] = (uint8_t)0; cur = prev; }  /* KwKwK */
        int first = 0;
        if (cur >= next) goto done;                      /* corrupt (incl. first code == next after a clear) */
        while (cur >= clear + 2) { stack[sp++] = suffix[cur]; cur = prefix[cur]; if (sp >= 4096) goto done; }
        first = cur; stack[sp++] = (uint8_t)cur;
        if (code == next && prev >= 0) stack[0] = (uint8_t)first;   /* fix KwKwK last byte */
        while (sp > 0) { if (oi >= cap) goto done; idx[oi++] = stack[--sp]; }
        if (prev >= 0 && next < 4096) { prefix[next] = prev; suffix[next] = (uint8_t)first; next++;
            if (next >= (1 << codesize) && codesize < 12) codesize++; }
        prev = code;
    }
done:
    #undef NEEDBYTE
    /* Skip to the end of the sub-block chain so the caller resumes correctly
     * even when the entropy decode stopped early. */
    if (consumed) {
        int q = pos;
        if (blkrem > 0) q += blkrem;
        while (q < dlen) { int sz = data[q++]; if (!sz) break; q += sz; }
        if (q > dlen) q = dlen;
        *consumed = q;
    }
    return oi;
}

/* --- one pass of the block structure ------------------------------------- */

struct gce { int delay_ms, transparent, disposal; };

/* Composite a decoded frame's indices into `canvas` at (fx, fy). */
static void composite(uint8_t *canvas, int cw, int ch,
                      const uint8_t *idx, int fx, int fy, int fw, int fh,
                      int interlaced, const uint8_t *ctab, int nct, int transparent)
{
    /* Interlaced images store rows in 4 passes (8/8/4/2 offsets); map each
     * stored row to its display row, sequential otherwise. */
    int p1 = (fh + 7) / 8, p2 = (fh + 3) / 8, p3 = (fh + 1) / 4;
    for (int k = 0; k < fw * fh; k++) {
        int ix = idx[k];
        if (ix == transparent) continue;               /* leave the canvas alone */
        int dr = k / fw;
        if (interlaced) {
            if (dr < p1) dr = dr * 8;
            else if (dr < p1 + p2) dr = 4 + (dr - p1) * 8;
            else if (dr < p1 + p2 + p3) dr = 2 + (dr - p1 - p2) * 4;
            else dr = 1 + (dr - p1 - p2 - p3) * 2;
        }
        int cx = fx + (k % fw), cy = fy + dr;
        if (cx < 0 || cy < 0 || cx >= cw || cy >= ch) continue;   /* clip */
        uint8_t *o = canvas + ((long)cy * cw + cx) * 4;
        if (ix < nct) { o[0]=ctab[ix*3]; o[1]=ctab[ix*3+1]; o[2]=ctab[ix*3+2]; }
        else { o[0]=o[1]=o[2]=0; }
        o[3] = 255;
    }
}

static void clear_rect(uint8_t *canvas, int cw, int ch, int fx, int fy, int fw, int fh)
{
    for (int y = fy; y < fy + fh; y++) {
        if (y < 0 || y >= ch) continue;
        for (int x = fx; x < fx + fw; x++) {
            if (x < 0 || x >= cw) continue;
            uint8_t *o = canvas + ((long)y * cw + x) * 4;
            o[0] = o[1] = o[2] = o[3] = 0;
        }
    }
}

/* The whole decode. `maxframes` of 1 stops after the first frame (the still
 * path); anything larger decodes the animation. */
static int gif_run(const uint8_t *p, int n, struct img_anim *out, int maxframes)
{
    if (n < 13) return -1;
    int cw = p[6] | (p[7] << 8), chh = p[8] | (p[9] << 8);
    int packed = p[10];
    int gct = (packed & 0x80) ? (2 << (packed & 7)) : 0;
    int pos = 13;
    const uint8_t *gctab = p + pos; pos += gct * 3;
    if (pos > n) return -1;                               /* global color table must fit the file */
    if (cw <= 0 || chh <= 0 || cw > 8192 || chh > 8192) return -1;

    unsigned long fsz = (unsigned long)cw * chh * 4;
    if (fsz == 0) return -1;
    unsigned frame_cap = maxframes;
    if (fsz && MAXANIMBYTES / fsz < frame_cap) frame_cap = MAXANIMBYTES / fsz;
    if (frame_cap < 1) frame_cap = 1;

    uint8_t *canvas = kmalloc(fsz);
    uint8_t *prev = kmalloc(fsz);
    struct img_frame *frames = kmalloc(sizeof(struct img_frame) * frame_cap);
    if (!canvas || !prev || !frames) { kfree(canvas); kfree(prev); kfree(frames); return -1; }
    memset(canvas, 0, fsz);
    memset(prev, 0, fsz);

    int nframes = 0, loops = 1;
    struct gce g = { 0, -1, 0 };

    while (pos < n) {
        int b = p[pos++];
        if (b == 0x3B) break;                             /* trailer */
        if (b == 0x21) {                                  /* extension */
            if (pos >= n) break;
            int label = p[pos++];
            if (label == 0xF9 && pos + 6 <= n && p[pos] == 4) {
                int gpacked = p[pos+1];
                g.disposal = (gpacked >> 2) & 7;
                g.delay_ms = (p[pos+2] | (p[pos+3] << 8)) * 10;
                g.transparent = (gpacked & 1) ? p[pos+4] : -1;
            } else if (label == 0xFF && pos + 12 <= n && p[pos] == 11 &&
                       p[pos+1]=='N' && p[pos+2]=='E' && p[pos+3]=='T' && p[pos+4]=='S' &&
                       p[pos+5]=='C' && p[pos+6]=='A' && p[pos+7]=='P' && p[pos+8]=='E') {
                /* NETSCAPE2.0 loop count. Layout after the 0xFF label:
                 *   [11]["NETSCAPE2.0"][3][1][lo][hi][0]
                 * so the count sits at +14/+15, past the sub-block LENGTH byte
                 * at +12 -- which is the byte an off-by-one lands on, and it
                 * reads as 3, i.e. "play three times", on every file. */
                int q = pos + 12;                 /* sub-block length byte */
                if (q + 4 <= n && p[q] >= 3 && p[q+1] == 1)
                    loops = p[q+2] | (p[q+3] << 8);
            }
            while (pos < n) { int sz = p[pos++]; if (!sz) break; pos += sz; }   /* skip sub-blocks */
            continue;
        }
        if (b == 0x2C) {                                  /* image descriptor */
            if (pos + 9 > n) break;                       /* need the 9-byte image descriptor */
            int fx = p[pos] | (p[pos+1]<<8), fy = p[pos+2] | (p[pos+3]<<8);
            int fw = p[pos+4] | (p[pos+5]<<8), fh = p[pos+6] | (p[pos+7]<<8);
            int ipacked = p[pos+8];
            int interlaced = ipacked & 0x40;
            pos += 9;
            const uint8_t *ctab = gctab; int nct = gct;
            if (ipacked & 0x80) { nct = 2 << (ipacked & 7); if (pos + nct*3 > n) break; ctab = p + pos; pos += nct*3; }
            if (fw <= 0 || fh <= 0 || fw > 8192 || fh > 8192) break;

            uint8_t *idx = kmalloc((unsigned long)fw * fh);
            if (!idx) break;
            int used = 0;
            int got = gif_lzw(p + pos, n - pos, idx, fw * fh, &used);
            for (int k = got < 0 ? 0 : got; k < fw * fh; k++) idx[k] = 0;   /* pad short data */
            pos += used;

            if ((unsigned)nframes >= frame_cap) { kfree(idx); break; }

            /* "Restore to previous" restores what was there BEFORE this frame,
             * so the snapshot has to be taken now, not after compositing. */
            if (g.disposal == 3) memcpy(prev, canvas, fsz);

            composite(canvas, cw, chh, idx, fx, fy, fw, fh, interlaced, ctab, nct, g.transparent);
            kfree(idx);

            uint8_t *shot = kmalloc(fsz);
            if (!shot) break;
            memcpy(shot, canvas, fsz);
            frames[nframes].rgba = shot;
            frames[nframes].delay_ms = g.delay_ms;
            nframes++;

            /* Disposal happens after the frame is shown. */
            if (g.disposal == 2) clear_rect(canvas, cw, chh, fx, fy, fw, fh);
            else if (g.disposal == 3) memcpy(canvas, prev, fsz);

            g.delay_ms = 0; g.transparent = -1; g.disposal = 0;
            if (nframes >= maxframes) break;
            continue;
        }
        break;                                            /* unknown block: stop */
    }

    kfree(canvas); kfree(prev);
    if (nframes == 0) { kfree(frames); return -1; }
    out->w = cw; out->h = chh; out->nframes = nframes;
    out->loops = loops; out->frames = frames;
    return 0;
}

static int gif_anim(const uint8_t *p, int n, struct img_anim *out)
{
    return gif_run(p, n, out, MAXFRAMES);
}

/* Still path: the first frame, composited onto the full logical screen. A GIF
 * whose first frame is a sub-rectangle used to decode to the sub-rectangle's
 * size, which is the wrong picture at the wrong size. */
static int gif_decode(const uint8_t *p, int n, struct image *out)
{
    struct img_anim a;
    if (gif_run(p, n, &a, 1) != 0) return -1;
    out->w = a.w; out->h = a.h; out->rgba = a.frames[0].rgba;
    kfree(a.frames);
    return 0;
}

void gif_register(void) { img_register_anim(gif_detect, gif_decode, gif_anim); }
