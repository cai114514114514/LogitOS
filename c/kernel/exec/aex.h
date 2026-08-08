#ifndef LOGIT_AEX_H
#define LOGIT_AEX_H

#include <stdint.h>

/* AEX -- the Logit native executable format (v1).
 *   [ 64-byte header ][ a standard ELF64 image ]
 * The header carries metadata the system needs without loading the program:
 * display name, the file extension it opens, and a Dock icon (glyph + color).
 * Built on the host by tools/mkaex.py. */
struct aex_header {
    char     magic[4];      /* "AEX1"                                   @0  */
    uint16_t version;       /* format version (AEX_VERSION)            @4  */
    uint16_t flags;         /*                                         @6  */
    char     name[32];      /* app display name                       @8  */
    char     ext[8];        /* file extension this app opens, or ""    @40 */
    uint8_t  icon;          /* Dock glyph (0 = use name[0])            @48 */
    uint8_t  icon_r;        /* Dock icon color (0,0,0 = palette)       @49 */
    uint8_t  icon_g;        /*                                         @50 */
    uint8_t  icon_b;        /*                                         @51 */
    char     pad[12];       /* -> 64 bytes; ELF follows                @52 */
};

#define AEX_MAGIC    "AEX1"
#define AEX_VERSION  1
#define AEX_HDR_SIZE 64

struct elf_image;

/* Load an in-memory .aex (`file_size` bytes) into the current address space
 * (user pages) and return the entry point (0 on failure). Fills name/ext if
 * non-NULL. file_size bounds the embedded ELF parse against the buffer.
 * out_top (may be NULL) receives the page-aligned top of the loaded image. */
uint64_t aex_load(const void *file, uint64_t file_size, char *out_name, char *out_ext,
                  uint64_t *out_top);

/* The same load, but handing back everything the ELF said about itself -- the
 * auxv material, the thread pointer, the stack protection. A caller that has to
 * BUILD the process (exec.c) needs these; a caller that only needs somewhere to
 * jump (the two above) does not. Returns 0 on success. */
int aex_load_image(const void *file, uint64_t file_size, char *out_name, char *out_ext,
                   struct elf_image *out);

/* Where the embedded ELF image starts and how long it is. The ELF used to begin
 * at a hardcoded +64 at every call site; it begins where the header says it
 * does. Returns 0 on success. */
int aex_elf_range(const void *file, uint64_t file_size, const void **elf, uint64_t *elf_size);

/* Validate + read metadata without loading. Returns 0 on success. */
int aex_info(const void *file, char *out_name, char *out_ext);

#endif /* LOGIT_AEX_H */
