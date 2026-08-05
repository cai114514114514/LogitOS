#ifndef LOGIT_VIRTIO_GPU_H
#define LOGIT_VIRTIO_GPU_H

#include <stdint.h>

int       virtio_gpu_init(void);     /* 0 if a virtio-gpu device is present + scanned out */
int       virtio_gpu_present(void);
uint32_t *virtio_gpu_fb(void);       /* the RAM framebuffer (0x00RRGGBB pixels) */
uint32_t  virtio_gpu_width(void);
uint32_t  virtio_gpu_height(void);
void      virtio_gpu_flush(int x, int y, int w, int h);   /* DMA a dirty rect + display */

#endif /* LOGIT_VIRTIO_GPU_H */
