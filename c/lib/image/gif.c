#include "img.h"

void *kmalloc(unsigned long);
void  kfree(void *);
void *memcpy(void *, const void *, unsigned long);

static int gif_detect(const uint8_t *p, int n)
{ return n >= 6 && p[0]=='G'&&p[1]=='I'&&p[2]=='F'&&p[3]=='8'; }

/* LZW decode GIF image data into `idx` (capacity cap). Returns #indices or -1.
 * `data` is the raw stream starting at the LZW-min-code-size byte; sub-blocks
 * are length-prefixed and 0-terminated. */
static int gif_lzw(const uint8_t *data, int dlen, uint8_t *idx, int cap)
{
    if (dlen < 1) return -1;
    int mincode = data[0];
    if (mincode < 2 || mincode > 11) return -1;   /* legal min code size is 2-8; 12+ overflows the 4096-entry dict, >=31 is UB shift */
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
        if (cur > next) goto done;                       /* corrupt */
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
    return oi;
}

static int gif_decode(const uint8_t *p, int n, struct image *out)
{
    if (n < 13) return -1;
    int pos = 6;                                          /* after "GIF8xa" */
    int packed = p[pos+4];
    int gct = (packed & 0x80) ? (2 << (packed & 7)) : 0;
    pos += 7;
    const uint8_t *gctab = p + pos; pos += gct*3;
    if (pos > n) return -1;                               /* global color table must fit the file */
    int transparent = -1;

    while (pos < n) {
        int b = p[pos++];
        if (b == 0x3B) break;                             /* trailer */
        if (b == 0x21) {                                  /* extension */
            if (pos >= n) break;
            int label = p[pos++];
            if (label == 0xF9 && pos+5 <= n) {            /* graphic control */
                int gpacked = p[pos+1];
                if (gpacked & 1) transparent = p[pos+4];
            }
            while (pos < n) { int sz = p[pos++]; if (!sz) break; pos += sz; }   /* skip sub-blocks */
            continue;
        }
        if (b == 0x2C) {                                  /* image descriptor */
            if (pos + 9 > n) return -1;                    /* need the 9-byte image descriptor */
            int iw = p[pos+4] | (p[pos+5]<<8), ih = p[pos+6] | (p[pos+7]<<8);
            int ipacked = p[pos+8];
            pos += 9;
            const uint8_t *ctab = gctab; int nct = gct;
            if (ipacked & 0x80) { nct = 2 << (ipacked & 7); if (pos + nct*3 > n) return -1; ctab = p + pos; pos += nct*3; }
            if (iw <= 0 || ih <= 0 || iw > 8192 || ih > 8192) return -1;
            uint8_t *idx = kmalloc(iw*ih); if (!idx) return -1;
            int got = gif_lzw(p + pos, n - pos, idx, iw*ih);
            if (got < iw*ih) { /* pad short data with 0 */ for (int k=got<0?0:got;k<iw*ih;k++) idx[k]=0; }
            uint8_t *rgba = kmalloc(iw*ih*4); if (!rgba) { kfree(idx); return -1; }
            for (int k = 0; k < iw*ih; k++) {
                int ix = idx[k]; uint8_t *o = rgba + k*4;
                if (ix < nct) { o[0]=ctab[ix*3]; o[1]=ctab[ix*3+1]; o[2]=ctab[ix*3+2]; }
                else { o[0]=o[1]=o[2]=0; }
                o[3] = (ix == transparent) ? 0 : 255;
            }
            kfree(idx);
            out->w = iw; out->h = ih; out->rgba = rgba;
            return 0;
        }
        /* unknown block: stop */
        break;
    }
    return -1;
}

void gif_register(void) { img_register(gif_detect, gif_decode); }
