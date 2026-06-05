#ifndef AQUA_ELF_H
#define AQUA_ELF_H

#include <stdint.h>

/* Load a static ELF64 executable image (already in memory) into the current
 * address space as user pages, copying PT_LOAD segments. Returns the entry
 * virtual address, or 0 on failure. */
uint64_t elf_load(void *image);

#endif /* AQUA_ELF_H */
