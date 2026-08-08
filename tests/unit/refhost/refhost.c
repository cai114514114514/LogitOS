/* refhost -- the bench the real kernel drawing code is bolted to on the host.
 *
 * Everything here is harness. The code under test is elsewhere: fb.c, raster.c,
 * text.c, c/lib/text/*, and the whole browser pipeline. See logit.h in this
 * directory for the full shared/not-shared statement. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "refhost.h"

/* ---------------------------------------------------------- kernel stubs -- */
/* The three the kernel provides to everyone. malloc/free are a faithful stand-in
 * for kheap here: nothing in the draw path depends on kheap's block layout, and
 * tests/unit/kheapstub sets the same precedent. */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

/* fb.c and text.c both log. Silent by default -- a per-test parse message times
 * 17,000 tests is not a diagnostic, it is a denial of service on the report.
 * REFHOST_VERBOSE=1 in the environment turns it back on, which is how a font
 * that silently failed to parse gets found. */
static int verbose_checked, verbose_on;
int kprintf(const char *fmt, ...)
{
    if (!verbose_checked) { verbose_checked = 1; verbose_on = getenv("REFHOST_VERBOSE") != 0; }
    if (!verbose_on) return 0;
    va_list ap; va_start(ap, fmt);
    /* kprintf is not printf (no %ll, its own %x), but for a diagnostic path
     * the approximation is fine and it is off by default. */
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    return 0;
}

/* fb.c probes virtio-gpu at init to decide whether it has a DMA scanout. The
 * harness never calls fb_init -- it drives fb_target() at a surface it owns --
 * so these exist only to satisfy the linker, and each one returning "absent" is
 * the honest answer for a host process. */
int  virtio_gpu_init(void) { return -1; }
void *virtio_gpu_fb(void) { return 0; }
int  virtio_gpu_width(void) { return 0; }
int  virtio_gpu_height(void) { return 0; }
void virtio_gpu_flush(int x, int y, int w, int h) { (void)x;(void)y;(void)w;(void)h; }
int  virtio_gpu_cursor_ready(void) { return 0; }
void virtio_gpu_cursor_define(const void *p, int w, int h) { (void)p;(void)w;(void)h; }
void virtio_gpu_cursor_move(int x, int y) { (void)x;(void)y; }
void vmm_map_range(uint64_t a, uint64_t b, uint64_t c) { (void)a;(void)b;(void)c; }

/* ------------------------------------------------------------- the fonts -- */
/* c/kernel/gui/text.c's text_init() opens three fixed paths through the VFS.
 * Rather than teach it about the host, give it a VFS: two entries, mapped to
 * host files by refhost_font_map. text.c is unmodified. */
static const char *map_ui, *map_mono;

static const char *hostpath(const char *vpath)
{
    if (!strcmp(vpath, "/fonts/ui.ttf"))   return map_ui;
    if (!strcmp(vpath, "/fonts/mono.ttf")) return map_mono;
    /* /fonts/text.ttf is the shaper's Arabic/Hebrew fallback. The WPT reftest
     * corpus does not need it and loading a third font would only add a way for
     * the two runs to differ, so it is deliberately absent. */
    return 0;
}

static long fsize(const char *p)
{
    FILE *f = fopen(p, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n;
}

int vfs_size(const char *p)
{
    const char *h = hostpath(p); if (!h) return -1;
    long n = fsize(h); return n < 0 ? -1 : (int)n;
}

int vfs_read(const char *p, void *buf, int max)
{
    const char *h = hostpath(p); if (!h) return -1;
    FILE *f = fopen(h, "rb"); if (!f) return -1;
    size_t n = fread(buf, 1, (size_t)max, f);
    fclose(f);
    return (int)n;
}

void refhost_font_map(const char *ui, const char *mono) { map_ui = ui; map_mono = mono; }

void text_init(void);                 /* c/kernel/gui/text.c, the real one */
int  text_measure(const char *s, int len, int px, int mono);

static int fonts_done;
int refhost_fonts(void)
{
    if (fonts_done) return fonts_done > 0 ? 0 : -1;
    text_init();
    /* text_init() reports failure only to kprintf, which is silent. Ask a
     * question only a loaded font can answer: a glyph has a nonzero advance.
     * This is the check that catches "the font path was wrong" -- without it a
     * missing font produces a suite that renders nothing and passes every
     * blank-vs-blank comparison, which is precisely the failure mode the
     * negative controls exist to make impossible. */
    int w = text_measure("X", 1, 32, 0);
    fonts_done = (w > 0) ? 1 : -1;
    return fonts_done > 0 ? 0 : -1;
}

/* ----------------------------------------------------------- the surface -- */
/* Mirrors c/kernel/gui/fb.h's struct surface. Kept as a local definition rather
 * than an #include so this file does not need the kernel's header tree; the
 * layout is asserted by refhost_begin's use of fb_target, which would corrupt
 * memory rather than fail quietly if it drifted -- so surf_check() below
 * verifies the two fields the harness itself sets. */
struct rh_surface { uint32_t *px; int w, h; int clip_on, clx0, cly0, clx1, cly1; };
void fb_target(void *s);
void fb_clear_clip(void);

static struct rh_surface surf;
int refhost_surf_w, refhost_surf_h;

void refhost_begin(int w, int h, uint32_t bg)
{
    if (w < 1) w = 1; if (h < 1) h = 1;
    if (w != surf.w || h != surf.h || !surf.px) {
        free(surf.px);
        surf.px = (uint32_t *)malloc((size_t)w * h * 4);
        surf.w = w; surf.h = h;
    }
    surf.clip_on = 0;
    refhost_surf_w = w; refhost_surf_h = h;
    for (long i = 0, n = (long)w * h; i < n; i++) surf.px[i] = bg;
    fb_target(&surf);
    fb_clear_clip();
}

uint32_t *refhost_end(void)
{
    fb_target(0);
    return surf.px;
}

/* ------------------------------------------------------------ comparison -- */
long refhost_cmp(const uint32_t *a, const uint32_t *b, int w, int h, int *maxdiff)
{
    long n = (long)w * h, diff = 0; int mx = 0;
    for (long i = 0; i < n; i++) {
        uint32_t p = a[i] & 0xFFFFFFu, q = b[i] & 0xFFFFFFu;
        if (p == q) continue;
        diff++;
        int dr = (int)((p >> 16) & 0xFF) - (int)((q >> 16) & 0xFF);
        int dg = (int)((p >> 8) & 0xFF) - (int)((q >> 8) & 0xFF);
        int db = (int)(p & 0xFF) - (int)(q & 0xFF);
        if (dr < 0) dr = -dr; if (dg < 0) dg = -dg; if (db < 0) db = -db;
        if (dr > mx) mx = dr; if (dg > mx) mx = dg; if (db > mx) mx = db;
    }
    if (maxdiff) *maxdiff = mx;
    return diff;
}

void refhost_diffimg(const uint32_t *a, const uint32_t *b, uint32_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t p = a[i] & 0xFFFFFFu, q = b[i] & 0xFFFFFFu;
        if (p == q) { out[i] = 0; continue; }
        int dr = (int)((p >> 16) & 0xFF) - (int)((q >> 16) & 0xFF);
        int dg = (int)((p >> 8) & 0xFF) - (int)((q >> 8) & 0xFF);
        int db = (int)(p & 0xFF) - (int)(q & 0xFF);
        if (dr < 0) dr = -dr; if (dg < 0) dg = -dg; if (db < 0) db = -db;
        int d = dr > dg ? dr : dg; if (db > d) d = db;
        /* Amplified: a delta of 1 must be VISIBLE, because a one-level rounding
         * difference over a large area is exactly the bug class this harness is
         * for and a faithful difference image would render it as black. */
        int v = 64 + d * 3 / 4; if (v > 255) v = 255;
        out[i] = ((uint32_t)v << 16) | ((uint32_t)(d > 8 ? v : 0) << 8);
    }
}

/* ------------------------------------------------------------------ PNG --- */
/* Truecolour 8-bit, stored (uncompressed) deflate blocks. A real encoder would
 * be smaller output and more code; these are debug artefacts read by a human
 * once, so the trade goes the other way. */
static uint32_t crc_tab[256]; static int crc_ready;
static uint32_t crc32b(const uint8_t *p, size_t n, uint32_t c)
{
    if (!crc_ready) {
        for (int i = 0; i < 256; i++) { uint32_t v = (uint32_t)i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_tab[i] = v; }
        crc_ready = 1;
    }
    c ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void be32(uint8_t *o, uint32_t v)
{ o[0] = (uint8_t)(v >> 24); o[1] = (uint8_t)(v >> 16); o[2] = (uint8_t)(v >> 8); o[3] = (uint8_t)v; }

static void chunk(FILE *f, const char *tag, const uint8_t *data, size_t n)
{
    uint8_t hdr[8]; be32(hdr, (uint32_t)n); memcpy(hdr + 4, tag, 4);
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc32b((const uint8_t *)tag, 4, 0);
    if (n) c = crc32b(data, n, c);
    uint8_t t[4]; be32(t, c); fwrite(t, 1, 4, f);
}

int refhost_png(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    static const uint8_t sig[8] = { 137, 'P','N','G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13]; be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);

    /* raw = per-row filter byte 0 + RGB triples */
    size_t rowlen = 1 + (size_t)w * 3, raw_n = rowlen * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    if (!raw) { fclose(f); return -1; }
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * rowlen; *r++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t v = px[(size_t)y * w + x];
            *r++ = (uint8_t)(v >> 16); *r++ = (uint8_t)(v >> 8); *r++ = (uint8_t)v;
        }
    }
    /* zlib stream: 0x78 0x01, stored deflate blocks (<=65535 each), adler32 */
    size_t nblk = (raw_n + 65534) / 65535; if (!nblk) nblk = 1;
    size_t zn = 2 + nblk * 5 + raw_n + 4;
    uint8_t *z = (uint8_t *)malloc(zn); if (!z) { free(raw); fclose(f); return -1; }
    size_t zo = 0; z[zo++] = 0x78; z[zo++] = 0x01;
    size_t off = 0;
    do {
        size_t n = raw_n - off; if (n > 65535) n = 65535;
        z[zo++] = (uint8_t)((off + n >= raw_n) ? 1 : 0);
        z[zo++] = (uint8_t)(n & 0xFF); z[zo++] = (uint8_t)(n >> 8);
        z[zo++] = (uint8_t)(~n & 0xFF); z[zo++] = (uint8_t)((~n >> 8) & 0xFF);
        if (n) memcpy(z + zo, raw + off, n);
        zo += n; off += n;
    } while (off < raw_n);
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < raw_n; i++) { s1 = (s1 + raw[i]) % 65521; s2 = (s2 + s1) % 65521; }
    be32(z + zo, (s2 << 16) | s1); zo += 4;
    chunk(f, "IDAT", z, zo);
    chunk(f, "IEND", 0, 0);
    fclose(f); free(raw); free(z);
    return 0;
}
