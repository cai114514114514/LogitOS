#include "img.h"
#include "inflate.h"

void *kmalloc(unsigned long);
void  kfree(void *);
void *memcpy(void *, const void *, unsigned long);
void *memset(void *, int, unsigned long);

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

static int png_decode(const uint8_t *p, int n, struct image *out)
{
    if (n < 8 + 25) return -1;
    int i = 8;
    int W=0, H=0, depth=0, ctype=0, interlace=0;
    uint8_t palette[256*3]; int npal = 0;
    uint8_t trns[256]; int ntrns = 0;
    /* gather IDAT */
    uint8_t *idat = 0; int idat_len = 0, idat_cap = 0;
    int ok = 0;

    while (i + 8 <= n) {
        uint32_t clen = be32(p + i);
        const uint8_t *type = p + i + 4;
        const uint8_t *data = p + i + 8;
        if (i + 12 + (int)clen > n) break;
        if (type[0]=='I'&&type[1]=='H'&&type[2]=='D'&&type[3]=='R') {
            W = (int)be32(data); H = (int)be32(data+4);
            depth = data[8]; ctype = data[9]; interlace = data[12];
        } else if (type[0]=='P'&&type[1]=='L'&&type[2]=='T'&&type[3]=='E') {
            npal = clen/3; if (npal>256) npal=256; memcpy(palette, data, npal*3);
        } else if (type[0]=='t'&&type[1]=='R'&&type[2]=='N'&&type[3]=='S') {
            ntrns = clen>256?256:(int)clen; memcpy(trns, data, ntrns);
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
        i += 12 + clen;                                   /* len + type + data + crc */
    }
    if (!ok || W<=0 || H<=0 || depth!=8 || interlace!=0) goto fail;
    if (W > 8192 || H > 8192 || W*H > 8192*8192) goto fail;

    int ch = (ctype==0)?1 : (ctype==2)?3 : (ctype==3)?1 : (ctype==4)?2 : (ctype==6)?4 : 0;
    if (!ch) goto fail;

    int stride = W*ch;
    int rawcap = (stride+1)*H;
    uint8_t *raw = kmalloc(rawcap); if (!raw) goto fail;
    int rawlen;
    if (zlib_decompress(idat, idat_len, raw, rawcap, &rawlen) || rawlen < (stride+1)*H) { kfree(raw); goto fail; }

    uint8_t *rgba = kmalloc(W*H*4); if (!rgba) { kfree(raw); goto fail; }
    uint8_t *prev = 0;
    for (int y = 0; y < H; y++) {
        uint8_t *cur = raw + y*(stride+1) + 1;
        int ft = raw[y*(stride+1)];
        for (int x = 0; x < stride; x++) {
            int a = (x>=ch) ? cur[x-ch] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x>=ch) ? prev[x-ch] : 0;
            int v = cur[x];
            switch (ft) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a+b)/2; break;
                case 4: v += paeth(a,b,c); break;
                default: break;
            }
            cur[x] = (uint8_t)v;
        }
        /* expand scanline to RGBA */
        for (int x = 0; x < W; x++) {
            uint8_t *o = rgba + (y*W + x)*4;
            const uint8_t *s = cur + x*ch;
            if (ctype==0)      { o[0]=o[1]=o[2]=s[0]; o[3]=255; }
            else if (ctype==2) { o[0]=s[0]; o[1]=s[1]; o[2]=s[2]; o[3]=255; }
            else if (ctype==4) { o[0]=o[1]=o[2]=s[0]; o[3]=s[1]; }
            else if (ctype==6) { o[0]=s[0]; o[1]=s[1]; o[2]=s[2]; o[3]=s[3]; }
            else /* palette */ { int idx=s[0]; o[0]=palette[idx*3]; o[1]=palette[idx*3+1]; o[2]=palette[idx*3+2];
                                 o[3]=(idx<ntrns)?trns[idx]:255; }
        }
        prev = cur;
    }
    kfree(raw); if (idat) kfree(idat);
    out->w = W; out->h = H; out->rgba = rgba;
    return 0;
fail:
    if (idat) kfree(idat);
    return -1;
}

void png_register(void) { img_register(png_detect, png_decode); }
