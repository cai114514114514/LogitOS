/* Host-test stub for the kernel pmm: kheap_test.c controls pmm_alloc_contig
 * (real aligned host allocations + on-demand failure injection). Shadows the
 * real c/kernel/mm/pmm.h via -I order. */
#ifndef KHEAPSTUB_PMM_H
#define KHEAPSTUB_PMM_H

#include <stdint.h>
#include <stddef.h>

#define FRAME_SIZE 4096

uint64_t pmm_alloc_contig(size_t n);

#endif
