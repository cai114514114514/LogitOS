#ifndef AETHER_ELF_H
#define AETHER_ELF_H

#include <stdint.h>

/* Load a static ELF64 executable image (already in memory, `image_size` bytes)
 * into the current address space as user pages, copying PT_LOAD segments.
 * Validates all program-header / segment ranges against image_size (the image
 * is untrusted on-disk data). Returns the entry virtual address, or 0 on failure. */
uint64_t elf_load(void *image, uint64_t image_size);

#endif /* AETHER_ELF_H */
