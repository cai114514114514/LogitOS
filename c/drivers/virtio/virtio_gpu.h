#ifndef LOGIT_VIRTIO_GPU_H
#define LOGIT_VIRTIO_GPU_H

#include <stdint.h>

int       virtio_gpu_init(void);     /* 0 if a virtio-gpu device is present + scanned out */
int       virtio_gpu_present(void);
uint32_t *virtio_gpu_fb(void);       /* the RAM framebuffer (0x00RRGGBB pixels) */
uint32_t  virtio_gpu_width(void);
uint32_t  virtio_gpu_height(void);
void      virtio_gpu_flush(int x, int y, int w, int h);   /* DMA a dirty rect + display */

/* ---- the cursor plane -----------------------------------------------------
 * The pointer is a HOST-SIDE plane, not pixels in the scanout. Defining it
 * costs a transfer; moving it costs one 56-byte command on a dedicated queue
 * and touches no framebuffer memory at all -- which is the difference between
 * a pointer that is free to move and one that recomposites the display.
 *
 * The plane is fixed at 64x64 with alpha; virtio_gpu_cursor_define pads (and
 * crops) `argb` into it. Consequence worth knowing before debugging a
 * screenshot: the pointer is NOT in the scanout, so a screendump does not
 * contain it -- exactly like every other OS with a hardware cursor. */
int       virtio_gpu_cursor_ready(void);
int       virtio_gpu_cursor_define(const uint32_t *argb, int w, int h, int hot_x, int hot_y);
void      virtio_gpu_cursor_move(int x, int y);

#endif /* LOGIT_VIRTIO_GPU_H */
