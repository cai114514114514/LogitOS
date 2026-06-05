#include "img.h"
#include "inflate.h"

void *kmalloc(unsigned long);
void  kfree(void *);
void *memcpy(void *, const void *, unsigned long);
void *memset(void *, int, unsigned long);

/* A complete-ish PNG decoder: all colour types (gray/RGB/palette/gray+alpha/RGBA),
 * all bit depths (1/2/4/8/16), the five filters, Adam7 interlacing, and tRNS
 * colour-key transparency for gray/RGB (plus per-index alpha for palette). 16-bit
 * samples are reduced to 8-bit; output is straight RGBA8. */

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

static int png_detect(const uint8_t *p, int n)
{
    static const uint8_t sig[8] = {0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a};
    if (n < 8) return 0;
    for (int i = 0; i < 8; i++) if (p[i] != sig[i]) return 0;
    return 1;
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p>a?p-a:a-p, pb = p>b?p-b:b-p, pc = p>c?p-c:c-p;
    return (pa<=pb && pa<=pc) ? a : (pb<=pc ? b : c);
}

/* raw sample `s` of a scanline at the given bit depth (1/2/4 -> 0..2^d-1,
 * 8 -> byte, 16 -> 0..65535, MSB-first packing for sub-byte depths). */
static int sample(const uint8_t *line, int depth, int s)
{
    if (depth == 16) return ((int)line[s*2] << 8) | line[s*2+1];
    if (depth == 8)  return line[s];
    int bit = s*depth, byte = bit >> 3, shift = 8 - depth - (bit & 7), mask = (1<<depth) - 1;
    return (line[byte] >> shift) & mask;
}
static int to8(int v, int depth) { return depth==16 ? (v>>8) : depth==8 ? v : v*255/((1<<depth)-1); }

/* unfilter `rows` scanlines in place; each is 1 filter byte + `stride` data bytes.
 * `bpp` = bytes per pixel for the a/c taps (ceil(channels*depth/8), >=1). */
static void unfilter(uint8_t *buf, int rows, int stride, int bpp)
{
    uint8_t *prev = 0;
    for (int y = 0; y < rows; y++) {
        uint8_t *cur = buf + y*(stride+1) + 1;
        int ft = buf[y*(stride+1)];
        for (int x = 0; x < stride; x++) {
            int a = (x>=bpp) ? cur[x-bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x>=bpp) ? prev[x-bpp] : 0;
            int v = cur[x];
            switch (ft) { case 1: v+=a; break; case 2: v+=b; break; case 3: v+=(a+b)/2; break; case 4: v+=paeth(a,b,c); break; }
            cur[x] = (uint8_t)v;
        }
        prev = cur;
    }
}

/* write pixel `col` of an unfiltered scanline to rgba[(y*W+x)]. */
static void emit(uint8_t *rgba, int W, int x, int y, const uint8_t *line, int col,
                 int ch, int depth, int ctype, const uint8_t *pal, const uint8_t *pala,
                 int key, int kr, int kg, int kb)
{
    uint8_t *o = rgba + (y*W + x)*4;
    int base = col*ch;
    if (ctype == 3) {                        /* palette index */
        int idx = sample(line, depth, base);
        o[0]=pal[idx*3]; o[1]=pal[idx*3+1]; o[2]=pal[idx*3+2]; o[3]=pala[idx];
    } else if (ctype == 0) {                  /* grayscale (+ optional colour key) */
        int g = sample(line, depth, base);
        o[0]=o[1]=o[2]=to8(g,depth); o[3] = (key && g==kr) ? 0 : 255;
    } else if (ctype == 4) {                  /* grayscale + alpha */
        int g = sample(line, depth, base), a = sample(line, depth, base+1);
        o[0]=o[1]=o[2]=to8(g,depth); o[3]=to8(a,depth);
    } else if (ctype == 2) {                  /* RGB (+ optional colour key) */
        int r=sample(line,depth,base), g=sample(line,depth,base+1), b=sample(line,depth,base+2);
        o[0]=to8(r,depth); o[1]=to8(g,depth); o[2]=to8(b,depth);
        o[3] = (key && r==kr && g==kg && b==kb) ? 0 : 255;
    } else {                                  /* RGBA */
        int r=sample(line,depth,base), g=sample(line,depth,base+1), b=sample(line,depth,base+2), a=sample(line,depth,base+3);
        o[0]=to8(r,depth); o[1]=to8(g,depth); o[2]=to8(b,depth); o[3]=to8(a,depth);
    }
}

/* depth allowed for each colour type? */
static int depth_ok(int ctype, int depth)
{
    switch (ctype) {
    case 0: return depth==1||depth==2||depth==4||depth==8||depth==16;   /* gray */
    case 3: return depth==1||depth==2||depth==4||depth==8;              /* palette */
    case 2: case 4: case 6: return depth==8||depth==16;                /* rgb / ga / rgba */
    }
    return 0;
}

static int png_decode(const uint8_t *p, int n, struct image *out)
{
    if (n < 8 + 25) return -1;
    int i = 8;
    int W=0, H=0, depth=0, ctype=0, interlace=0;
    uint8_t pal[256*3]; uint8_t pala[256]; int npal = 0;
    uint8_t trns[6]; int ntrns = 0;
    uint8_t *idat = 0; int idat_len = 0, idat_cap = 0;
    int ok = 0;
    memset(pala, 255, sizeof pala);

    while (i + 8 <= n) {
        uint32_t clen = be32(p + i);
        const uint8_t *type = p + i + 4;
        const uint8_t *data = p + i + 8;
        if (i + 12 + (int)clen > n) break;
        if (type[0]=='I'&&type[1]=='H'&&type[2]=='D'&&type[3]=='R') {
            W=(int)be32(data); H=(int)be32(data+4); depth=data[8]; ctype=data[9]; interlace=data[12];
        } else if (type[0]=='P'&&type[1]=='L'&&type[2]=='T'&&type[3]=='E') {
            npal = clen/3; if (npal>256) npal=256; memcpy(pal, data, npal*3);
        } else if (type[0]=='t'&&type[1]=='R'&&type[2]=='N'&&type[3]=='S') {
            if (ctype==3) { int k = clen>256?256:(int)clen; for (int j=0;j<k;j++) pala[j]=data[j]; }
            else { ntrns = clen>6?6:(int)clen; memcpy(trns, data, ntrns); }
        } else if (type[0]=='I'&&type[1]=='D'&&type[2]=='A'&&type[3]=='T') {
            if (idat_len + (int)clen > idat_cap) {
                int nc = (idat_cap*2 > idat_len+(int)clen) ? idat_cap*2 : idat_len+(int)clen+1024;
                uint8_t *nb = kmalloc(nc); if (!nb) goto fail;
                if (idat) { memcpy(nb, idat, idat_len); kfree(idat); }
                idat = nb; idat_cap = nc;
            }
            memcpy(idat + idat_len, data, clen); idat_len += clen;
        } else if (type[0]=='I'&&type[1]=='E'&&type[2]=='N'&&type[3]=='D') {
            ok = 1; break;
        }
        i += 12 + clen;
    }
    if (!ok || W<=0 || H<=0 || !depth_ok(ctype, depth)) goto fail;
    if (W > 8192 || H > 8192 || W*H > 8192*8192) goto fail;

    int ch = (ctype==0||ctype==3)?1 : (ctype==2)?3 : (ctype==4)?2 : 4;
    int bpp = (ch*depth + 7) / 8; if (bpp < 1) bpp = 1;

    /* colour-key transparency for gray (2 bytes) / RGB (6 bytes) */
    int key=0, kr=0, kg=0, kb=0;
    if (ntrns && ctype==0) { key=1; kr = (trns[0]<<8)|trns[1]; }
    else if (ntrns>=6 && ctype==2) { key=1; kr=(trns[0]<<8)|trns[1]; kg=(trns[2]<<8)|trns[3]; kb=(trns[4]<<8)|trns[5]; }

    /* Adam7 pass geometry (pass 0 = whole image when not interlaced). */
    static const int ox[7]={0,4,0,2,0,1,0}, oy[7]={0,0,4,0,2,0,1};
    static const int sx[7]={8,8,4,4,2,2,1}, sy[7]={8,8,8,4,4,2,2};
    int npass = interlace ? 7 : 1;
    int pcols[7], prows[7], pstride[7];
    long raw_total = 0;
    for (int pass = 0; pass < npass; pass++) {
        int cols, rows;
        if (interlace) {
            cols = W>ox[pass] ? (W - ox[pass] + sx[pass]-1)/sx[pass] : 0;
            rows = H>oy[pass] ? (H - oy[pass] + sy[pass]-1)/sy[pass] : 0;
        } else { cols = W; rows = H; }
        pcols[pass]=cols; prows[pass]=rows;
        pstride[pass] = (cols*ch*depth + 7) / 8;
        if (cols && rows) raw_total += (long)(pstride[pass]+1) * rows;
    }

    uint8_t *raw = kmalloc((unsigned long)raw_total); if (!raw) goto fail;
    int rawlen;
    if (zlib_decompress(idat, idat_len, raw, (int)raw_total, &rawlen) || rawlen < raw_total) { kfree(raw); goto fail; }

    uint8_t *rgba = kmalloc((unsigned long)W*H*4); if (!rgba) { kfree(raw); goto fail; }

    long off = 0;
    for (int pass = 0; pass < npass; pass++) {
        int cols = pcols[pass], rows = prows[pass], stride = pstride[pass];
        if (!cols || !rows) continue;
        uint8_t *block = raw + off;
        off += (long)(stride+1) * rows;
        unfilter(block, rows, stride, bpp);
        for (int r = 0; r < rows; r++) {
            const uint8_t *line = block + r*(stride+1) + 1;
            int y = interlace ? oy[pass] + r*sy[pass] : r;
            for (int c = 0; c < cols; c++) {
                int x = interlace ? ox[pass] + c*sx[pass] : c;
                emit(rgba, W, x, y, line, c, ch, depth, ctype, pal, pala, key, kr, kg, kb);
            }
        }
    }
    (void)npal;
    kfree(raw); if (idat) kfree(idat);
    out->w = W; out->h = H; out->rgba = rgba;
    return 0;
fail:
    if (idat) kfree(idat);
    return -1;
}

void png_register(void) { img_register(png_detect, png_decode); }
