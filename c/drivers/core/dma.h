/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_DMA_H
#define LOGIT_DMA_H
#include <stddef.h>
#include <stdint.h>

/* No IOMMU yet: device addresses are RAM physical addresses, NEVER CPU
 * pointers. Keeping this a struct makes an accidental pointer assignment a
 * compiler error; extract its value only when writing a hardware field. */
typedef struct { uint64_t value; } dma_addr_t;
static inline uint64_t dma_addr_value(dma_addr_t a) { return a.value; }
static inline dma_addr_t dma_addr_add(dma_addr_t a, size_t n)
{ return (dma_addr_t){a.value + n}; }
#define DMA_MASK_32 UINT64_C(0xffffffff)
#define DMA_MASK_64 UINT64_MAX
#define DMA_MAX_MAPPING (1024u * 1024u)
#define DMA_MAP_CONTIGUOUS 1u

enum dma_direction { DMA_TO_DEVICE, DMA_FROM_DEVICE, DMA_BIDIRECTIONAL };
enum dma_state { DMA_READY, DMA_DEVICE_OWNED, DMA_COMPLETED,
                 DMA_QUIESCED, DMA_QUARANTINED };
struct dma_buffer;
struct dma_mapping;
struct dma_device {
    const char *name;
    uint64_t mask, generation;
    size_t alignment, boundary, max_segment, max_segments;
    int blocked, logged_mapping, logged_high_mapping;
    struct dma_buffer *buffers;
    struct dma_mapping *mappings;
};
struct dma_buffer {
    void *cpu;
    dma_addr_t dma;
    size_t size, pages;
    uint64_t phys, token;
    enum dma_state state;
    struct dma_device *dev;
    struct dma_buffer *next;
};
struct dma_segment { dma_addr_t addr; size_t len; };
struct dma_mapping {
    void *cpu;
    size_t size, valid, npages, nsegments;
    enum dma_direction direction;
    enum dma_state state;
    uint64_t token;
    int bounced;
    uint64_t *phys_pages;
    struct dma_segment *segments;
    struct dma_buffer *bounce;
    struct dma_device *dev;
    struct dma_mapping *next;
};
struct dma_stats {
    uint64_t coherent_buffers, coherent_bytes, active_mappings, pinned_pages;
    uint64_t direct_bytes, bounce_bytes, quarantined_buffers;
    uint64_t quarantined_mappings, quarantined_bytes, rejected_completions;
    uint64_t allocations, allocation_failures, high_allocations, max_dma;
    uint64_t completed_direct_bytes, completed_high_bytes;
};

/* Device contexts/handles must stay at a stable address until all their
 * resources are freed. mask=0 disables DMA until capabilities are known.
 * Constraints are inclusive and validated over the whole transfer; configure
 * before allocating. Defaults: page-compatible alignment, no boundary,
 * max_segment=UINT32_MAX, max_segments=256. Streaming mappings have a separate
 * hard 1 MiB bound; coherent objects also obey max_segment. No allocation in IRQ callbacks. */
void dma_device_init(struct dma_device *dev, const char *name, uint64_t mask);
struct dma_buffer *dma_alloc_coherent(struct dma_device *dev, size_t bytes,
                                      size_t align, size_t boundary);
int dma_free_coherent(struct dma_buffer *buf);
struct dma_mapping *dma_map_kernel(struct dma_device *dev, void *cpu,
                                   size_t bytes, enum dma_direction direction,
                                   unsigned flags);
int dma_unmap(struct dma_mapping *map);
/* Resolve an offset using the actual SG list, for PRPs and descriptor chains.
 * Returns UINT64_MAX in the wrapper on an invalid offset. */
dma_addr_t dma_mapping_addr(const struct dma_mapping *map, size_t offset);

/* Submit returns a fresh nonzero cookie. Complete must use that exact cookie;
 * reset and subsequent submissions invalidate old completions. Completion
 * copies only valid bytes for FROM/BIDIRECTIONAL mappings. TO never writes
 * back into the source. Explicit sync is available for persistent buffers. */
uint64_t dma_buffer_submit(struct dma_buffer *buf);
int dma_buffer_complete(struct dma_buffer *buf, uint64_t token);
uint64_t dma_mapping_submit(struct dma_mapping *map);
int dma_mapping_complete(struct dma_mapping *map, uint64_t token, size_t valid);
int dma_sync_for_device(struct dma_mapping *map);
int dma_sync_for_cpu(struct dma_mapping *map, size_t valid);
void dma_wmb(void);
void dma_rmb(void);

/* Hardware stop/reset must be ACKNOWLEDGED before quiesced. Quarantine blocks
 * new work and retains memory. Neither operation frees driver-visible handles.
 * Direct-map callers must retain their original buffer until quiescence: pins
 * keep pages alive but cannot stop the caller rewriting its own buffer. */
void dma_device_quiesced(struct dma_device *dev);
void dma_device_quarantine(struct dma_device *dev);
int dma_device_resume(struct dma_device *dev);
void dma_get_stats(struct dma_stats *out);
void dma_report(const char *tag);
#endif
