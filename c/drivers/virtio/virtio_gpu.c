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
#define GPU_RESP_OK_NODATA           0x1100
#define GPU_RESP_OK_DISPLAY_INFO     0x1101
#define GPU_FORMAT_B8G8R8X8          2     /* mem bytes B,G,R,X == our 0x00RRGGBB */
#define RESID 1

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

static struct virtio_dev gpudev;
static struct virtq      gpuvq;
static int               gpu_ready;
static uint32_t          gpu_w, gpu_h;
static uint32_t         *gpu_fb;        /* the RAM framebuffer (identity-mapped) */

/* Static request/response staging (single outstanding, identity-mapped). */
static uint8_t cmdbuf[256];
static uint8_t respbuf[256];

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

int virtio_gpu_init(void)
{
    if (virtio_init(VIRTIO_DEV_GPU, &gpudev, 0) != 0) return -1;
    if (virtio_queue_setup(&gpudev, 0, &gpuvq) != 0) return -1;   /* controlq */
    virtio_driver_ok(&gpudev);

    /* 1. Display info -> preferred resolution (fall back to 1280x800). */
    gpu_w = 1280; gpu_h = 800;
    { struct gpu_hdr *h = (struct gpu_hdr *)cmdbuf; memset(h, 0, sizeof *h);
      h->type = GPU_CMD_GET_DISPLAY_INFO;
      if (gpu_cmd(sizeof *h) == GPU_RESP_OK_DISPLAY_INFO) {
          struct gpu_disp_info *di = (struct gpu_disp_info *)respbuf;
          if (di->pmodes[0].enabled && di->pmodes[0].r.width) {
              gpu_w = di->pmodes[0].r.width; gpu_h = di->pmodes[0].r.height;
          }
      }
    }

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
      if (gpu_cmd(sizeof *c) != GPU_RESP_OK_NODATA) return -1; }
    { struct gpu_attach_backing *a = (struct gpu_attach_backing *)cmdbuf; memset(a, 0, sizeof *a);
      a->hdr.type = GPU_CMD_RESOURCE_ATTACH_BACKING;
      a->resource_id = RESID; a->nr_entries = 1;
      a->addr = (uint64_t)(uintptr_t)gpu_fb; a->length = (uint32_t)bytes;
      if (gpu_cmd(sizeof *a) != GPU_RESP_OK_NODATA) return -1; }
    { struct gpu_set_scanout *s = (struct gpu_set_scanout *)cmdbuf; memset(s, 0, sizeof *s);
      s->hdr.type = GPU_CMD_SET_SCANOUT;
      s->r.width = gpu_w; s->r.height = gpu_h; s->scanout_id = 0; s->resource_id = RESID;
      if (gpu_cmd(sizeof *s) != GPU_RESP_OK_NODATA) return -1; }

    gpu_ready = 1;
    kprintf("[virtio-gpu] %dx%d, RAM fb @ %p\n", gpu_w, gpu_h, (void *)gpu_fb);
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
