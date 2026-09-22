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
 * Serialized by the graphics domain. Missing assets are negatively cached, avoiding
 * filesystem reads on every Dock hover. The session-wide cache is bounded. */
static struct {
    char name[LOGIT_ICON_NAME_MAX];
    struct image image;
} cache[32];
static unsigned count, bytes_used;
#define ICON_BUDGET (24u * 1024u * 1024u)

static unsigned big32(const unsigned char *p)
{ return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]; }

int app_icon_draw(const char *name, int x, int y, int size)
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
        /* Inspect IHDR before decode: a tiny compressed file can advertise an
         * enormous allocation. A post-decode size check is already too late. */
        if (!w || !h || w > 1536 || h > 1536 || w * h * 4 > ICON_BUDGET - bytes_used) return 0;
        unsigned char *data = kmalloc((unsigned)bytes);
        if (!data) return 0;
        int read = vfs_pread(path, data, bytes, 0);
        /* The kernel's codec owner serializes the shared decoder workspace;
         * the graphics lock alone does not exclude a CLI decoding an image. */
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
