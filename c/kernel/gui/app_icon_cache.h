/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_APP_ICON_CACHE_H
#define LOGIT_APP_ICON_CACHE_H
/* Pure tile cache for app icons: no locking, no I/O, no kernel includes.
 *
 * A tile is the icon pre-scaled to (px x px) device pixels as straight RGBA
 * bytes, keyed by (app slot, px). A scale or mode change changes px, so it
 * naturally re-keys; nothing has to be flushed. The cache is a small LRU that
 * keeps the sum of tile bytes within `budget`.
 *
 * THREADING CONTRACT (the caller, app_icons.c, owns the lock):
 *   - aic_find / aic_publish / aic_init touch cache state and must be called
 *     with the app-icon lock held. A tile pointer returned by aic_find is
 *     valid only until the lock is dropped, because eviction (inside
 *     aic_publish) frees it; blit it before unlocking.
 *   - Tiles are immutable once published.
 *   - aic_publish hands back the tiles it evicted; the caller frees them.
 *   - aic_build_tile touches no cache state and runs unlocked. */
#include <stdint.h>
#include "openlogit_display_blit.h"

#define AIC_SLOTS 64u
#define AIC_MAX_PX 512

struct aic_ent { void *tile; unsigned bytes, stamp; unsigned short app, px; };
struct aic {
    struct aic_ent e[AIC_SLOTS];
    unsigned used_bytes, budget, clock;
};

static inline void aic_init(struct aic *c, unsigned budget)
{
    for (unsigned i = 0; i < AIC_SLOTS; i++) c->e[i].tile = 0;
    c->used_bytes = 0; c->budget = budget; c->clock = 0;
}

static inline unsigned aic_tile_bytes(int px) { return (unsigned)px * (unsigned)px * 4u; }

static inline unsigned aic_count(const struct aic *c)
{
    unsigned n = 0;
    for (unsigned i = 0; i < AIC_SLOTS; i++) if (c->e[i].tile) n++;
    return n;
}

/* Lookup; refreshes the entry's LRU stamp. */
static inline const uint8_t *aic_find(struct aic *c, unsigned app, int px)
{
    for (unsigned i = 0; i < AIC_SLOTS; i++) {
        struct aic_ent *e = &c->e[i];
        if (e->tile && e->app == app && e->px == px) { e->stamp = ++c->clock; return e->tile; }
    }
    return 0;
}

/* Publish a freshly built tile. Returns
 *   1  published; `out[0..*n)` are evicted tiles the caller must free.
 *   0  the key already exists (another band published first): the caller
 *      frees its own tile and uses aic_find. Exactly one tile per key.
 *  -1  the tile alone exceeds the budget or px is out of range: not cached,
 *      the caller may blit its private tile and must then free it.
 * `out` must have room for AIC_SLOTS pointers. */
static inline int aic_publish(struct aic *c, unsigned app, int px, void *tile,
                              void **out, unsigned *n)
{
    unsigned bytes = aic_tile_bytes(px);
    *n = 0;
    if (px <= 0 || px > AIC_MAX_PX || bytes > c->budget) return -1;
    for (unsigned i = 0; i < AIC_SLOTS; i++)
        if (c->e[i].tile && c->e[i].app == app && c->e[i].px == px) return 0;
    for (;;) {
        unsigned freeslot = AIC_SLOTS, lru = AIC_SLOTS;
        for (unsigned i = 0; i < AIC_SLOTS; i++) {
            if (!c->e[i].tile) { if (freeslot == AIC_SLOTS) freeslot = i; continue; }
            /* Wrap-safe age: the oldest stamp is the largest clock - stamp. */
            if (lru == AIC_SLOTS || c->clock - c->e[i].stamp > c->clock - c->e[lru].stamp) lru = i;
        }
        if (freeslot != AIC_SLOTS && c->used_bytes + bytes <= c->budget) {
            struct aic_ent *e = &c->e[freeslot];
            e->tile = tile; e->bytes = bytes; e->app = (unsigned short)app;
            e->px = (unsigned short)px; e->stamp = ++c->clock;
            c->used_bytes += bytes;
            return 1;
        }
        /* lru is valid: bytes <= budget means an empty cache always fits. */
        out[(*n)++] = c->e[lru].tile;
        c->used_bytes -= c->e[lru].bytes;
        c->e[lru].tile = 0;
    }
}

/* Scale a straight-alpha RGBA source to a px x px straight-alpha tile with an
 * exact area-average (ol_display_blit_rgba_ex BOX). The display blit writes
 * opaque native pixels and carries no alpha, so the source is composited
 * twice, over black (B = premultiplied colour) and over white
 * (W = B + 255 - alpha); alpha = 255 - (W - B) and colour = B * 255 / alpha.
 * Opaque pixels round-trip exactly. Returns an `alloc`'d px*px*4 tile or 0.
 * Temporaries (two px*px*4 planes and the blit's scratch) are freed here. */
static inline uint8_t *aic_build_tile(const uint8_t *rgba, int w, int h, int px,
                                      void *(*alloc)(unsigned long), void (*release)(void *))
{
    if (!rgba || w <= 0 || h <= 0 || px <= 0 || px > AIC_MAX_PX) return 0;
#ifdef APP_ICON_NEAREST        /* negative control: point sampling instead of BOX */
    const enum ol_filter filter = OL_FILTER_NEAREST;
#else
    const enum ol_filter filter = OL_FILTER_BOX;
#endif
    unsigned long n = (unsigned long)px * (unsigned long)px;
    uint32_t *plane[2];
    plane[0] = alloc(n * 4); plane[1] = alloc(n * 4);
    if (!plane[0] || !plane[1]) {
        if (plane[0]) release(plane[0]);
        if (plane[1]) release(plane[1]);
        return 0;
    }
    int ok = 1;
    for (int k = 0; k < 2 && ok; k++) {
        for (unsigned long i = 0; i < n; i++) plane[k][i] = k ? 0x00FFFFFFu : 0;
        struct ol_display d = {0};
        struct ol_pixel_target t = { plane[k], px, px, 0, 0, 0, 0, 0 };
        d.allocate = alloc; d.release = release;
        if (ol_display_bind(&d, &t, 0, 8, 16) != OL_OK ||
            ol_display_blit_rgba_ex(&d, rgba, w, h, w * 4, 0, 0, w, h, 0, 0,
                                    px * 256, px * 256, 255, filter) != OL_OK) ok = 0;
        ol_display_destroy(&d);
    }
    if (!ok) { release(plane[0]); release(plane[1]); return 0; }
    uint8_t *out = (uint8_t *)plane[0];       /* convert black plane in place */
    for (unsigned long i = 0; i < n; i++) {
        uint32_t b = plane[0][i], wv = plane[1][i];
        int ch[3], dsum = 0;
        for (int q = 0; q < 3; q++) {
            ch[q] = (int)((b >> (8 * q)) & 255);
            int dv = (int)((wv >> (8 * q)) & 255) - ch[q];
            dsum += dv < 0 ? 0 : dv;
        }
        int a = 255 - (dsum + 1) / 3;
        if (a < 0) a = 0;
        uint8_t *o = out + i * 4;
        if (!a) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
        for (int q = 0; q < 3; q++) {
            int c = (ch[q] * 255 + a / 2) / a;
            o[q] = (uint8_t)(c > 255 ? 255 : c);
        }
        o[3] = (uint8_t)a;
    }
    release(plane[1]);
    return out;
}
#endif /* LOGIT_APP_ICON_CACHE_H */
