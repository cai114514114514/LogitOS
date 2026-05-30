#ifndef AQUA_AEX_H
#define AQUA_AEX_H

#include <stdint.h>

/* AEX -- the Aqua native executable format.
 *   [ 64-byte header ][ a standard ELF64 image ]
 * The header carries the app's display name and the file extension it opens;
 * the ELF is loaded by the kernel's ELF loader. Built by tools/mkaex.py. */
struct aex_header {
    char     magic[4];      /* "AEX1" */
    char     name[32];      /* app display name */
    char     ext[8];        /* file extension this app opens (e.g. "txt"), or "" */
    uint32_t flags;
    uint32_t reserved;
    char     pad[12];       /* -> 64 bytes total; ELF follows */
};

#define AEX_HDR_SIZE 64

/* Load an in-memory .aex into the current address space (user pages) and
 * return the entry point (0 on failure). Fills name/ext if non-NULL. */
uint64_t aex_load(const void *file, char *out_name, char *out_ext);

/* Read just the metadata (for the app registry). Returns 0 on success. */
int aex_info(const void *file, char *out_name, char *out_ext);

#endif /* AQUA_AEX_H */
