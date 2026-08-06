#ifndef LOGIT_ELF_H
#define LOGIT_ELF_H

#include <stdint.h>

/* Load a static ELF64 executable image (already in memory, `image_size` bytes)
 * into the current address space as user pages, copying PT_LOAD segments.
 * Validates all program-header / segment ranges against image_size (the image
 * is untrusted on-disk data). Returns the entry virtual address, or 0 on failure.
 * When out_top is non-NULL it receives the page-aligned top of the highest
 * mapped segment -- callers place the user stack ABOVE it (a fixed offset from
 * the entry point collides with BSS once an app's static footprint grows past
 * that offset: the browser's 96 MiB arena landed on top of its own stack). */
uint64_t elf_load(void *image, uint64_t image_size, uint64_t *out_top);

#endif /* LOGIT_ELF_H */
