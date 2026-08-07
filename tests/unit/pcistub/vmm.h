#ifndef LOGIT_VMM_H
#define LOGIT_VMM_H
/* Host stub: the ECAM window is a malloc'd buffer, so "mapping" is a no-op and
 * physical == virtual is trivially true. */
#include <stdint.h>

#define VMM_WRITABLE 0x2
#define VMM_USER     0x4
#define VMM_NOCACHE  (0x8 | 0x10)

static inline void vmm_map_page(uint64_t v, uint64_t p, uint64_t f) { (void)v; (void)p; (void)f; }
static inline void vmm_map_range(uint64_t v, uint64_t p, uint64_t s, uint64_t f)
{ (void)v; (void)p; (void)s; (void)f; }

#endif
