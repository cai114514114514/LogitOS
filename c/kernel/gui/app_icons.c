/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "icons.h"
#include "img.h"
#include "vfs.h"
#include "fb.h"
#include "kheap.h"
#include "kprintf.h"
#include "logit_app_icon.h"
#include "syscall.h"

/* Raster artwork is a disk resource, /icons/<executable stem>.png. No app
 * name is special-cased: adding another icon needs an asset, not kernel art.
 * Missing assets are negatively cached, avoiding filesystem reads on every
 * Dock hover. The session-wide cache is bounded by ICON_BUDGET. */
#define ICON_BUDGET (24u * 1024u * 1024u)

/* The tiled path needs the kernel spinlock and gfx headers. The host loader
 * test (tests/unit/app_icon_test.py) compiles this file against stub headers
 * that have neither, and exercises the legacy decoded-image path below. */
#if defined(__has_include)
# if __has_include("spinlock.h")
#  define APP_ICON_TILED 1
# endif
#endif

static unsigned big32(const unsigned char *p)
{ return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }

/* Validate "<dir>/<stem>.aex"; returns the file-name length of the stem
 * including ".aex" (>= 5) or 0 when the name is not an app name. */
static unsigned icon_base(const char *name, const char **base_out)
{
    const char *base = name;
    for (const char *p = name; *p; p++) if (*p == '/') base = p + 1;
    unsigned length = 0;
    while (base[length]) {
        char c = base[length];
        if (length >= LOGIT_ICON_NAME_MAX - 1 ||
            !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
        length++;
    }
    if (length < 5 || base[length - 4] != '.' || base[length - 3] != 'a' ||
        base[length - 2] != 'e' || base[length - 1] != 'x') return 0;
    *base_out = base;
    return length;
}

#ifdef APP_ICON_TILED
#include "spinlock.h"
#include "app_icon_cache.h"

/* LOCKING. app_icon_draw runs in compositor bands on several cores.
 * icon_lock (spin_lock_irqsave: a spinlock must never be held across a
 * preemption, see text.c:353-357) protects ONLY: the name table, aic_find,
 * aic_publish and the tile blit. It is never held across vfs_*, kmalloc of the
 * decode buffers, kernel_img_decode or the BOX scale; those run on a miss with
 * the lock dropped, into private buffers. Tiles are immutable once published
 * and are evicted only inside aic_publish under the lock, so a blit under the
 * lock never sees a freed tile. Two bands that miss the same key both build;
 * the loser's publish finds the key present and frees its tile.
 * Lock order: icon_lock is a leaf (fb_blit_rgba takes no other lock; the
 * kheap lock is taken inside it only through the display blit's small, cached
 * scratch). */
static spinlock_t icon_lock = SPINLOCK_INIT;
enum { APP_UNKNOWN, APP_PRESENT, APP_ABSENT };
static struct { char name[LOGIT_ICON_NAME_MAX]; unsigned char state; } apps[32];
static unsigned app_count;
static struct aic tiles;
static int tiles_ready;

static void *icon_alloc(unsigned long n) { return kmalloc((unsigned)n); }
static void icon_release(void *p) { kfree(p); }

/* Caller holds icon_lock. Returns the slot or -1 when the table is full. */
static int app_slot(const char *base, unsigned length)
{
    for (unsigned s = 0; s < app_count; s++) {
        unsigned i = 0;
        while (apps[s].name[i] && apps[s].name[i] == base[i]) i++;
        if (!apps[s].name[i] && !base[i]) return (int)s;
    }
    if (app_count == 32) return -1;
    unsigned s = app_count++;
    for (unsigned i = 0; i <= length; i++) apps[s].name[i] = base[i];
    apps[s].state = APP_UNKNOWN;
    return (int)s;
}

/* Unlocked miss path: read, decode and box-scale to a px x px tile. Sets
 * *absent when the asset itself is unusable (as opposed to out of memory). */
static uint8_t *icon_make_tile(const char *base, unsigned length, int px, int *absent)
{
    *absent = 1;
    char path[64] = "/icons/";
    unsigned end = 7;
    for (unsigned i = 0; i < length - 4; i++) path[end++] = base[i];
    const char *suffix = ".png";
    for (unsigned i = 0; i <= 4; i++) path[end + i] = suffix[i];
    int bytes = vfs_size(path);
    if (bytes < 33 || bytes > 8 * 1024 * 1024) return 0;
    unsigned char header[24];
    if (vfs_pread(path, header, sizeof header, 0) != sizeof header ||
        big32(header) != 0x89504E47u || big32(header + 4) != 0x0D0A1A0Au ||
        big32(header + 8) != 13 || big32(header + 12) != 0x49484452u) return 0;
    unsigned w = big32(header + 16), h = big32(header + 20);
    /* Inspect IHDR before decode: a tiny compressed file can advertise an
     * enormous allocation. A post-decode size check is already too late. */
    if (!w || !h || w > 1536 || h > 1536 || w * h * 4 > ICON_BUDGET) return 0;
    unsigned char *data = kmalloc((unsigned)bytes);
    if (!data) { *absent = 0; return 0; }
    int read = vfs_pread(path, data, bytes, 0);
    struct image image = {0, 0, 0};
    /* The kernel's codec owner serializes the shared decoder workspace;
     * the graphics lock alone does not exclude a CLI decoding an image. */
    int decoded = read == bytes ? kernel_img_decode(data, bytes, &image) : -1;
    kfree(data);
    if (decoded || !image.rgba) return 0;
    uint8_t *tile = aic_build_tile(image.rgba, image.w, image.h, px, icon_alloc, icon_release);
    img_free(&image);
    if (!tile) { *absent = 0; return 0; }
    kprintf("[icons] raster %s %ux%u -> %dpx\n", base, w, h, px);
    return tile;
}

int app_icon_draw(const char *name, int x, int y, int size)
{
    const char *base;
    unsigned length = icon_base(name, &base);
    if (!length || size <= 0 || size > AIC_MAX_PX) return 0;

    unsigned long f = spin_lock_irqsave(&icon_lock);
    if (!tiles_ready) { aic_init(&tiles, ICON_BUDGET); tiles_ready = 1; }
    int slot = app_slot(base, length);
    if (slot < 0 || apps[slot].state == APP_ABSENT) { spin_unlock_irqrestore(&icon_lock, f); return 0; }
    const uint8_t *tile = aic_find(&tiles, (unsigned)slot, size);
    if (tile) {
        fb_blit_rgba(x, y, size, size, tile, size, size);
        spin_unlock_irqrestore(&icon_lock, f);
        return 1;
    }
    spin_unlock_irqrestore(&icon_lock, f);

    int absent;
    uint8_t *fresh = icon_make_tile(base, length, size, &absent);

    void *evicted[AIC_SLOTS];
    unsigned nev = 0;
    int drawn = 0;
    f = spin_lock_irqsave(&icon_lock);
    if (!fresh) {
        if (absent) apps[slot].state = APP_ABSENT;
    } else {
        apps[slot].state = APP_PRESENT;
        int r = aic_publish(&tiles, (unsigned)slot, size, fresh, evicted, &nev);
        if (r == 1) {
            fb_blit_rgba(x, y, size, size, fresh, size, size);
            fresh = 0;                       /* now owned by the cache */
            drawn = 1;
        } else if (r == 0) {                 /* another band published first */
            tile = aic_find(&tiles, (unsigned)slot, size);
            if (tile) { fb_blit_rgba(x, y, size, size, tile, size, size); drawn = 1; }
        }
    }
    spin_unlock_irqrestore(&icon_lock, f);
    for (unsigned i = 0; i < nev; i++) kfree(evicted[i]);
    if (fresh) {
        /* Duplicate (freed) or uncacheable (-1): a private tile is safe to
         * blit without the lock. */
        if (!drawn) { fb_blit_rgba(x, y, size, size, fresh, size, size); drawn = 1; }
        kfree(fresh);
    }
    return drawn;
}

#else /* host loader test: legacy decoded-image cache, nearest-neighbour blit */

/* Serialized by the graphics domain. */
static struct {
    char name[LOGIT_ICON_NAME_MAX];
    struct image image;
} cache[32];
static unsigned count, bytes_used;

int app_icon_draw(const char *name, int x, int y, int size)
{
    const char *base;
    unsigned length = icon_base(name, &base);
    if (!length) return 0;
    unsigned slot;
    for (slot = 0; slot < count; slot++) {
        unsigned i = 0;
        while (cache[slot].name[i] && cache[slot].name[i] == base[i]) i++;
        if (!cache[slot].name[i] && !base[i]) break;
    }
    if (slot == count) {
        if (count == 32) return 0;
        count++;
        for (unsigned i = 0; i <= length; i++) cache[slot].name[i] = base[i];
        char path[64] = "/icons/";
        unsigned end = 7;
        for (unsigned i = 0; i < length - 4; i++) path[end++] = base[i];
        const char *suffix = ".png";
        for (unsigned i = 0; i <= 4; i++) path[end + i] = suffix[i];
        int bytes = vfs_size(path);
        if (bytes < 33 || bytes > 8 * 1024 * 1024) return 0;
        unsigned char header[24];
        if (vfs_pread(path, header, sizeof header, 0) != sizeof header ||
            big32(header) != 0x89504E47u || big32(header + 4) != 0x0D0A1A0Au ||
            big32(header + 8) != 13 || big32(header + 12) != 0x49484452u) return 0;
        unsigned w = big32(header + 16), h = big32(header + 20);
        if (!w || !h || w > 1536 || h > 1536 || w * h * 4 > ICON_BUDGET - bytes_used) return 0;
        unsigned char *data = kmalloc((unsigned)bytes);
        if (!data) return 0;
        int read = vfs_pread(path, data, bytes, 0);
        int decoded = read == bytes ? kernel_img_decode(data, bytes, &cache[slot].image) : -1;
        kfree(data);
        if (decoded) return 0;
        bytes_used += w * h * 4;
        kprintf("[icons] raster %s %ux%u\n", base, w, h);
    }
    struct image *image = &cache[slot].image;
    if (!image->rgba) return 0;
    fb_blit_rgba(x, y, size, size, image->rgba, image->w, image->h);
    return 1;
}
#endif
