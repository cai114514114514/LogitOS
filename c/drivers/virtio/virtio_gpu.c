#include <stdint.h>
#include <stddef.h>
#include "virtio.h"
#include "virtio_gpu.h"
#include "pmm.h"
#include "kprintf.h"

void *memset(void *, int, size_t);

/* virtio-gpu 2D: a host-side scanout resource backed by a normal-RAM framebuffer.
 * present = TRANSFER_TO_HOST_2D(dirty rect) + RESOURCE_FLUSH -- a DMA the host
 * pulls, instead of the CPU byte-copying into uncached VGA MMIO. */

#define GPU_CMD_GET_DISPLAY_INFO     0x0100
#define GPU_CMD_RESOURCE_CREATE_2D   0x0101
#define GPU_CMD_RESOURCE_UNREF       0x0102
#define GPU_CMD_SET_SCANOUT          0x0103
#define GPU_CMD_RESOURCE_FLUSH       0x0104
#define GPU_CMD_TRANSFER_TO_HOST_2D  0x0105
#define GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define GPU_CMD_UPDATE_CURSOR        0x0300
#define GPU_CMD_MOVE_CURSOR          0x0301
#define GPU_RESP_OK_NODATA           0x1100
#define GPU_RESP_OK_DISPLAY_INFO     0x1101
#define GPU_FORMAT_B8G8R8X8          2     /* mem bytes B,G,R,X == our 0x00RRGGBB */
#define GPU_FORMAT_B8G8R8A8          1     /* ... with alpha == our 0xAARRGGBB */
#define RESID 1

/* ---- the cursor plane ------------------------------------------------------
 *
 * virtio-gpu has a SECOND virtqueue whose only job is the pointer, and that is
 * the whole reason this exists. A cursor that lives in the scanout has to be
 * composited, which means every pixel the pointer passes over is a pixel the
 * CPU rewrites and the device re-transfers -- at 1920x1200 that is 2.3 M pixels
 * of work to move an arrow eight of them. On the cursor queue it is one 56-byte
 * command and the host draws the arrow on its own plane: no framebuffer write,
 * no DMA, no recomposite.
 *
 * The 64x64 is not a choice. The device allocates a fixed 64x64 cursor and
 * refuses a resource of any other size (QEMU's virtio_gpu_update_cursor_data
 * compares width/height and silently returns), so the image is padded, not
 * scaled -- a smaller arrow occupies the top-left corner and the rest is
 * transparent. B8G8R8A8 rather than the scanout's X8 format: the pointer's
 * surround has to be see-through, and that is what the A is for. */
#define CURSOR_QIDX  1
#define CURSOR_RESID 2
#define CURSOR_DIM   64

/* The mode to program when the host cannot tell us a usable one. It matches the
 * Makefile's QEMU_GPU default on purpose: an unrealized window should come up as
 * the desktop we ship, not as a fallback that looks like a different product. */
#define GPU_DEFAULT_W 1920
#define GPU_DEFAULT_H 1200
/* Below this the desktop's own windows do not fit -- see the note at the check. */
#define GPU_MIN_W     1280
#define GPU_MIN_H     800

struct gpu_hdr { uint32_t type, flags; uint64_t fence_id; uint32_t ctx_id, padding; } __attribute__((packed));
struct gpu_rect { uint32_t x, y, width, height; } __attribute__((packed));

struct gpu_disp_info {
    struct gpu_hdr hdr;
    struct { struct gpu_rect r; uint32_t enabled, flags; } pmodes[16];
} __attribute__((packed));
struct gpu_create_2d { struct gpu_hdr hdr; uint32_t resource_id, format, width, height; } __attribute__((packed));
struct gpu_attach_backing { struct gpu_hdr hdr; uint32_t resource_id, nr_entries;
                            uint64_t addr; uint32_t length, padding; } __attribute__((packed));
struct gpu_set_scanout { struct gpu_hdr hdr; struct gpu_rect r; uint32_t scanout_id, resource_id; } __attribute__((packed));
struct gpu_transfer { struct gpu_hdr hdr; struct gpu_rect r; uint64_t offset; uint32_t resource_id, padding; } __attribute__((packed));
struct gpu_flush { struct gpu_hdr hdr; struct gpu_rect r; uint32_t resource_id, padding; } __attribute__((packed));

struct gpu_cursor_pos { uint32_t scanout_id, x, y, padding; } __attribute__((packed));
struct gpu_update_cursor {
    struct gpu_hdr hdr;
    struct gpu_cursor_pos pos;
    uint32_t resource_id, hot_x, hot_y, padding;
} __attribute__((packed));
/* The device checks the LENGTH of a cursor command and drops anything that is
 * not exactly this struct -- without an error, because there is no response
 * buffer to put one in. A short write is therefore a pointer that silently
 * never moves, which is a miserable thing to debug from the outside. */
_Static_assert(sizeof(struct gpu_update_cursor) == 56, "virtio_gpu_update_cursor is 56 bytes");

static struct virtio_dev gpudev;
static struct virtq      gpuvq;
static struct virtq      gpucurvq;
static int               gpu_ready;
static int               gpu_cursor_ready;     /* the cursor queue + resource are up */
static uint32_t          gpu_w, gpu_h;
static uint32_t         *gpu_fb;        /* the RAM framebuffer (identity-mapped) */
static uint32_t         *gpu_cursor_px; /* 64x64 0xAARRGGBB backing (identity-mapped) */

/* Static request/response staging (single outstanding, identity-mapped). */
static uint8_t cmdbuf[256];
static uint8_t respbuf[256];
/* The cursor queue gets its own staging buffer rather than sharing cmdbuf.
 * Requests are synchronous and single-threaded today, so sharing would work --
 * but a pointer move is issued from the input path and a scanout flush from the
 * compositor, and the day those stop being the same thread the failure would be
 * a corrupted command, not a compile error. */
static uint8_t curbuf[64];

/* Send a command (cmd_len bytes from cmdbuf), receive the response into respbuf.
 * Returns the response type, or 0 on failure. */
static uint32_t gpu_cmd(int cmd_len)
{
    memset(respbuf, 0, sizeof respbuf);
    struct virtio_buf b[2] = {
        { (uint64_t)(uintptr_t)cmdbuf,  (uint32_t)cmd_len, 0 },
        { (uint64_t)(uintptr_t)respbuf, sizeof respbuf,    1 },
    };
    if (virtio_request(&gpudev, &gpuvq, 0, b, 2) < 0) return 0;
    return ((struct gpu_hdr *)respbuf)->type;
}

/* Release the RAM framebuffer (init failure unwind; pmm_free is per-frame). */
static void gpu_free_fb(uint64_t frames)
{
    if (!gpu_fb) return;
    for (uint64_t i = 0; i < frames; i++)
        pmm_free((uint64_t)(uintptr_t)gpu_fb + i * 4096);
    gpu_fb = NULL;
}

int virtio_gpu_init(void)
{
    if (virtio_init(VIRTIO_DEV_GPU, &gpudev, 0) != 0) return -1;
    if (virtio_queue_setup(&gpudev, 0, &gpuvq) != 0) return -1;   /* controlq */
    /* The cursor queue must be programmed BEFORE DRIVER_OK, like every other
     * queue -- a device is entitled to stop accepting queue configuration once
     * the driver declares itself ready. Its absence is not fatal: the compositor
     * falls back to drawing the pointer into the scanout. */
    int have_cursor_q = (virtio_queue_setup(&gpudev, CURSOR_QIDX, &gpucurvq) == 0);
    virtio_driver_ok(&gpudev);

    /* 1. Display info -> preferred resolution.
     *
     * WHATEVER THE DEVICE REPORTS AT OR ABOVE THE MINIMUM IS TAKEN AS IS, and a
     * bigger mode is now a genuinely better desktop rather than a smaller-looking
     * one: the compositor picks a backing scale from this size (fb.c pick_scale)
     * and keeps the logical desktop at the design canvas, so extra pixels buy
     * density. The driver still reads the mode exactly once. */
    gpu_w = GPU_DEFAULT_W; gpu_h = GPU_DEFAULT_H;
    { struct gpu_hdr *h = (struct gpu_hdr *)cmdbuf; memset(h, 0, sizeof *h);
      h->type = GPU_CMD_GET_DISPLAY_INFO;
      if (gpu_cmd(sizeof *h) == GPU_RESP_OK_DISPLAY_INFO) {
          struct gpu_disp_info *di = (struct gpu_disp_info *)respbuf;
          if (di->pmodes[0].enabled && di->pmodes[0].r.width) {
              gpu_w = di->pmodes[0].r.width; gpu_h = di->pmodes[0].r.height;
          }
      }
    }
    /* The floor is the DESIGN CANVAS, not an arbitrary threshold: the desktop is
     * authored in 1280x800 points and the browser alone asks for a 1180-point
     * window, so a smaller scanout is not a smaller desktop -- it is a desktop
     * whose windows do not fit, and scaling cannot rescue that (the scale factor
     * only ever goes up). A not-yet-realized host window reports 640x480 and is
     * exactly this case. Program the default via SET_SCANOUT instead; QEMU honors
     * it and grows the console/window to match. */
    if (gpu_w < GPU_MIN_W || gpu_h < GPU_MIN_H) { gpu_w = GPU_DEFAULT_W; gpu_h = GPU_DEFAULT_H; }

    /* 2. RAM framebuffer (contiguous, identity-mapped). */
    uint64_t bytes = (uint64_t)gpu_w * gpu_h * 4;
    uint64_t frames = (bytes + 4095) / 4096;
    gpu_fb = (uint32_t *)pmm_alloc_contig(frames);
    if (!gpu_fb) return -1;
    memset(gpu_fb, 0, (size_t)bytes);

    /* 3. Create a 2D resource, attach our RAM as its backing, bind to scanout 0. */
    { struct gpu_create_2d *c = (struct gpu_create_2d *)cmdbuf; memset(c, 0, sizeof *c);
      c->hdr.type = GPU_CMD_RESOURCE_CREATE_2D;
      c->resource_id = RESID; c->format = GPU_FORMAT_B8G8R8X8; c->width = gpu_w; c->height = gpu_h;
      if (gpu_cmd(sizeof *c) != GPU_RESP_OK_NODATA) { gpu_free_fb(frames); return -1; } }
    { struct gpu_attach_backing *a = (struct gpu_attach_backing *)cmdbuf; memset(a, 0, sizeof *a);
      a->hdr.type = GPU_CMD_RESOURCE_ATTACH_BACKING;
      a->resource_id = RESID; a->nr_entries = 1;
      a->addr = (uint64_t)(uintptr_t)gpu_fb; a->length = (uint32_t)bytes;
      if (gpu_cmd(sizeof *a) != GPU_RESP_OK_NODATA) { gpu_free_fb(frames); return -1; } }
    { struct gpu_set_scanout *s = (struct gpu_set_scanout *)cmdbuf; memset(s, 0, sizeof *s);
      s->hdr.type = GPU_CMD_SET_SCANOUT;
      s->r.width = gpu_w; s->r.height = gpu_h; s->scanout_id = 0; s->resource_id = RESID;
      if (gpu_cmd(sizeof *s) != GPU_RESP_OK_NODATA) { gpu_free_fb(frames); return -1; } }

    gpu_ready = 1;

    /* 4. The cursor plane: a second 64x64 resource with alpha, never scanned
     *    out -- the device composites it over the scanout itself. */
    if (have_cursor_q) {
        gpu_cursor_px = (uint32_t *)pmm_alloc_contig((CURSOR_DIM * CURSOR_DIM * 4 + 4095) / 4096);
        if (gpu_cursor_px) {
            memset(gpu_cursor_px, 0, CURSOR_DIM * CURSOR_DIM * 4);
            int ok = 1;
            { struct gpu_create_2d *c = (struct gpu_create_2d *)cmdbuf; memset(c, 0, sizeof *c);
              c->hdr.type = GPU_CMD_RESOURCE_CREATE_2D;
              c->resource_id = CURSOR_RESID; c->format = GPU_FORMAT_B8G8R8A8;
              c->width = CURSOR_DIM; c->height = CURSOR_DIM;
              if (gpu_cmd(sizeof *c) != GPU_RESP_OK_NODATA) ok = 0; }
            if (ok) { struct gpu_attach_backing *a = (struct gpu_attach_backing *)cmdbuf; memset(a, 0, sizeof *a);
              a->hdr.type = GPU_CMD_RESOURCE_ATTACH_BACKING;
              a->resource_id = CURSOR_RESID; a->nr_entries = 1;
              a->addr = (uint64_t)(uintptr_t)gpu_cursor_px; a->length = CURSOR_DIM * CURSOR_DIM * 4;
              if (gpu_cmd(sizeof *a) != GPU_RESP_OK_NODATA) ok = 0; }
            if (ok) gpu_cursor_ready = 1;
        }
    }
    kprintf("[virtio-gpu] %dx%d, RAM fb @ %p, cursor plane %s\n",
            gpu_w, gpu_h, (void *)gpu_fb, gpu_cursor_ready ? "yes" : "no");
    return 0;
}

int       virtio_gpu_present(void) { return gpu_ready; }
uint32_t *virtio_gpu_fb(void)      { return gpu_fb; }
uint32_t  virtio_gpu_width(void)   { return gpu_w; }
uint32_t  virtio_gpu_height(void)  { return gpu_h; }

/* Push a dirty rect: DMA it from the backing to the host resource, then display. */
void virtio_gpu_flush(int x, int y, int w, int h)
{
    if (!gpu_ready) return;
    if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
    if (x + w > (int)gpu_w) w = (int)gpu_w - x;
    if (y + h > (int)gpu_h) h = (int)gpu_h - y;
    if (w <= 0 || h <= 0) return;

    { struct gpu_transfer *t = (struct gpu_transfer *)cmdbuf; memset(t, 0, sizeof *t);
      t->hdr.type = GPU_CMD_TRANSFER_TO_HOST_2D;
      t->r.x = x; t->r.y = y; t->r.width = w; t->r.height = h;
      t->offset = (uint64_t)y * gpu_w * 4 + (uint64_t)x * 4;
      t->resource_id = RESID;
      gpu_cmd(sizeof *t); }
    { struct gpu_flush *f = (struct gpu_flush *)cmdbuf; memset(f, 0, sizeof *f);
      f->hdr.type = GPU_CMD_RESOURCE_FLUSH;
      f->r.x = x; f->r.y = y; f->r.width = w; f->r.height = h; f->resource_id = RESID;
      gpu_cmd(sizeof *f); }
}

/* ---- cursor plane --------------------------------------------------------- */

int virtio_gpu_cursor_ready(void) { return gpu_cursor_ready; }

/* One command on the cursor queue. The device consumes it and completes with a
 * zero-length used element -- there is no response structure, so unlike gpu_cmd
 * there is nothing to read back and nothing to check beyond "it completed". */
static int gpu_cursor_cmd(uint32_t type, int x, int y, uint32_t resid, int hot_x, int hot_y)
{
    struct gpu_update_cursor *u = (struct gpu_update_cursor *)curbuf;
    memset(u, 0, sizeof *u);
    u->hdr.type = type;
    u->pos.scanout_id = 0;
    u->pos.x = (uint32_t)(x < 0 ? 0 : x);
    u->pos.y = (uint32_t)(y < 0 ? 0 : y);
    u->resource_id = resid;
    u->hot_x = (uint32_t)hot_x; u->hot_y = (uint32_t)hot_y;
    struct virtio_buf b = { (uint64_t)(uintptr_t)u, (uint32_t)sizeof *u, 0 };
    return virtio_request(&gpudev, &gpucurvq, CURSOR_QIDX, &b, 1);
}

/* Publish a new pointer image. `argb` is w*h pixels of 0xAARRGGBB placed at the
 * top-left of the 64x64 plane; anything past it stays transparent. Oversized
 * images are CROPPED rather than rejected, because the alternative at a scale
 * this driver does not control is no pointer at all. */
int virtio_gpu_cursor_define(const uint32_t *argb, int w, int h, int hot_x, int hot_y)
{
    if (!gpu_cursor_ready || !argb || w <= 0 || h <= 0) return -1;
    if (w > CURSOR_DIM) w = CURSOR_DIM;
    if (h > CURSOR_DIM) h = CURSOR_DIM;
    memset(gpu_cursor_px, 0, CURSOR_DIM * CURSOR_DIM * 4);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            gpu_cursor_px[j * CURSOR_DIM + i] = argb[j * w + i];

    /* The device reads the cursor image out of its own copy of the resource, so
     * the backing has to be transferred exactly like a scanout rect -- on the
     * CONTROL queue. Skipping this is the classic invisible-cursor bug: the
     * UPDATE_CURSOR below succeeds and points at 64x64 of zeroes. */
    { struct gpu_transfer *t = (struct gpu_transfer *)cmdbuf; memset(t, 0, sizeof *t);
      t->hdr.type = GPU_CMD_TRANSFER_TO_HOST_2D;
      t->r.x = 0; t->r.y = 0; t->r.width = CURSOR_DIM; t->r.height = CURSOR_DIM;
      t->offset = 0; t->resource_id = CURSOR_RESID;
      if (gpu_cmd(sizeof *t) != GPU_RESP_OK_NODATA) return -1; }

    return gpu_cursor_cmd(GPU_CMD_UPDATE_CURSOR, 0, 0, CURSOR_RESID, hot_x, hot_y) < 0 ? -1 : 0;
}

/* Move the pointer. THIS is the hot path -- one 56-byte descriptor, no
 * framebuffer write and no transfer of any kind. */
void virtio_gpu_cursor_move(int x, int y)
{
    if (!gpu_cursor_ready) return;
    gpu_cursor_cmd(GPU_CMD_MOVE_CURSOR, x, y, CURSOR_RESID, 0, 0);
}
