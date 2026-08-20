#ifndef LOGIT_AEX_H
#define LOGIT_AEX_H

#include <stdint.h>

/* AEX -- the Logit native executable.
 *
 * ============================================================================
 * THE DECISION: AEX KEEPS AN ELF INSIDE IT.
 * ============================================================================
 * The other option was for AEX to describe its own segments, and it is
 * genuinely defensible -- the file would stop being two half-formats and could
 * carry exactly what it needs. It was not taken, for three reasons, in order of
 * weight:
 *
 * 1. IT WOULD BE A SECOND LOADER. c/kernel/exec/elf.c is now a real ELF64
 *    implementation: bounds-checked against the image in subtraction form,
 *    fuzzed, with a rejection matrix and a message for every field. An AEX
 *    segment table would need every one of those checks again, in ring 0, on
 *    disk-controlled input, and the two copies would drift. "The loader stops
 *    being two half-formats" is the argument FOR owning the format, and the
 *    honest reading is that owning it produces two WHOLE formats instead --
 *    which is more code in the most dangerous place in the system.
 *
 * 2. IT WOULD NOT ACTUALLY REMOVE THE ELF PARSER, only move it. Every binary
 *    here is produced by ld.lld, which emits ELF. An AEX-native format means
 *    tools/mkaex.py parses the ELF and re-emits it -- so the parse still
 *    happens, just on the host, and the kernel then trusts a format that only
 *    our own tool can produce. M18's stated goal is to "run software not
 *    written for LogitOS"; a private container is a step away from that.
 *
 * 3. THE COMPILER ARGUMENT CUTS THE OTHER WAY. docs/.../aetherscript-2 §P4
 *    wants `as -c --native` to emit a real .aex, and a private segment table
 *    would be about as easy to emit as a minimal ELF -- so that ask does not
 *    decide it. What decides it is what happens NEXT: an ELF from a
 *    from-scratch code generator can be read by readelf, disassembled by
 *    objdump and diffed against a known-good one. A private format cannot,
 *    and a compiler backend whose output nothing else can read is a backend
 *    whose first codegen bug costs a day instead of an hour. tools/mkaex.py
 *    --emit builds that minimal ELF from flat code/data/bss, so the compiler
 *    hands over bytes and one command, and gets a debuggable binary back.
 *
 * WHAT IT COSTS: nothing at the ~30 `-Ttext=` sites, and that is not a dodge --
 * it is the consequence. Keeping ELF keeps the fixed link bases, so the
 * Makefile is untouched except for the new checks. The price is paid honestly
 * elsewhere: the fixed bases stay because ET_DYN and relocation stay refused
 * (elf.c says so out loud), and tests/unit/exec_bases.py now checks the base
 * map against the binaries instead of the map being a comment.
 *
 * ============================================================================
 * THE FILE
 * ============================================================================
 *   [ 64-byte fixed header ][ TLV metadata ][ the ELF64 image ]
 *          @0                    @64            @hdr_size
 *
 * v1 was the 64-byte header and an ELF at a hardcoded +64, with `flags` and
 * `pad[12]` never read and `version` compared for exact equality -- so there
 * was no way to add anything and no way to say "this file is newer than you".
 * v2 changes NOTHING in the first 52 bytes: magic, version, flags, name, ext,
 * icon and colour are at the same offsets with the same meanings, because
 * c/kernel/gui/wm.c's Dock scan reads them out of the raw struct and that call
 * site belongs to another line. pad[12] becomes the fields below, and the file
 * grows a TLV region for everything that is not worth a fixed field.
 *
 * FORWARD AND BACKWARD, stated as rules rather than left to be discovered:
 *   - version 1 is ACCEPTED, and says so on the log. It is what every disk
 *     image built before this change contains. hdr_size is 64, elf_size is
 *     "the rest of the file", and there is no integrity record -- which is the
 *     point of the log line.
 *   - version > AEX_VERSION_MAX is REFUSED, naming the version. A file from the
 *     future is not a file to guess at.
 *   - an unknown TLV tag is IGNORED. An unknown `flags` bit is REFUSED. That
 *     split is the whole forward-compatibility contract: metadata may grow
 *     silently, behaviour may not.
 */

struct aex_header {
    char     magic[4];      /* "AEX1" -- unchanged in v2; `version` distinguishes */
    uint16_t version;       /* 1 = the v1 wrapper, 2 = this            @4  */
    uint16_t flags;         /* AEX_F_* -- READ now, and validated      @6  */
    char     name[32];      /* app display name                        @8  */
    char     ext[8];        /* file extension this app opens, or ""    @40 */
    uint8_t  icon;          /* Dock glyph (0 = use name[0])            @48 */
    uint8_t  icon_r;        /* Dock icon colour (0,0,0 = palette)      @49 */
    uint8_t  icon_g;        /*                                         @50 */
    uint8_t  icon_b;        /*                                         @51 */
    /* --- v1's pad[12], which was the only growth room the format had --- */
    uint16_t hdr_size;      /* bytes before the ELF image (>= 64)      @52 */
    uint16_t stack_pages;   /* user-stack hint; 0 = launcher default   @54 */
    uint8_t  arch;          /* AEX_ARCH_*                              @56 */
    uint8_t  abi;           /* AEX_ABI_*                               @57 */
    uint8_t  category;      /* AEX_CAT_*: what kind of program this is @58 */
    uint8_t  sort;          /* launcher order; ties break on name      @59 */
    uint32_t elf_size;      /* exact size of the embedded ELF image    @60 */
};

#define AEX_MAGIC        "AEX1"
#define AEX_VERSION      2      /* what mkaex.py writes today            */
#define AEX_VERSION_MIN  1      /* the oldest this loader will accept    */
#define AEX_VERSION_MAX  2      /* anything above is refused, by number  */
#define AEX_HDR_SIZE     64     /* the FIXED part. Callers still use this
                                 * as the minimum sensible file size.    */
#define AEX_HDR_MAX      16384  /* hdr_size cap: the header is read into
                                 * kernel memory before anything else is
                                 * known about the file. Four pages, not one.
                                 *
                                 * It was 4096, and mkaex.py now pads hdr_size
                                 * up to the next multiple of 4096 so the ELF
                                 * image starts on a page boundary (see
                                 * elf_file_runs: a file page and a virtual page
                                 * can only be the same page if it does). A file
                                 * whose metadata already reached 4096 would
                                 * then pad to 8192 and be refused by a cap it
                                 * had not grown past -- so the cap has to sit
                                 * above the alignment, not on it. Still four
                                 * pages of kernel memory read from an
                                 * untrusted file, which is the thing the cap
                                 * is for.                               */

/* flags. Unknown bits are REFUSED -- see the contract above. */
#define AEX_F_GUI     0x0001    /* opens a window; a Dock candidate      */
#define AEX_F_CLI     0x0002    /* a command-line program, lives in /bin */
#define AEX_F_KNOWN   0x0003

#define AEX_ARCH_X86_64 1
#define AEX_ABI_LOGIT1  1       /* int 0x80, include/abi/logit_abi.h     */

/* category: what the thing IS, which is a different question from what file
 * type it opens. The Dock has never had an answer to it. */
#define AEX_CAT_NONE     0
#define AEX_CAT_SYSTEM   1
#define AEX_CAT_MEDIA    2
#define AEX_CAT_DEV      3
#define AEX_CAT_NET      4
#define AEX_CAT_UTIL     5
#define AEX_CAT_TEST     6      /* built for a harness, not for a person */

/* TLV records live in [64, hdr_size). Each is {u32 tag, u32 len, u8 val[len]}
 * with the next record starting at the next 8-byte boundary. */
#define AEX_TLV_HDR   8
#define AEX_T_CRC32   0x43524341u  /* "ACRC": CRC-32 of the ELF image. REQUIRED
                                    * for v2 -- this is the "no checksum" gap,
                                    * and a checksum a file may omit is not one. */
#define AEX_T_APPID   0x44495841u  /* "AXID": a stable id ("os.logit.browser").
                                    * The thing the Dock has to sort and colour
                                    * by if it is ever to stop depending on the
                                    * order files landed on the disk.          */
#define AEX_T_TYPES   0x50595441u  /* "ATYP": u16[] of logit_sniff SN_* ids the
                                    * app can open. See the note in aex.c.     */
#define AEX_T_APAD    0x44415041u  /* "APAD": zero bytes, present only to push
                                    * hdr_size to a multiple of 4096 so the ELF
                                    * image starts on a page boundary. Carries
                                    * no information and is READ BY NOTHING --
                                    * a record rather than raw zeroes so the
                                    * header stays self-describing, and a v2
                                    * loader that predates it ignores it under
                                    * the unknown-tag rule instead of choking. */

struct elf_image;

/* Everything the container says, read once, bounded. */
struct aex_info {
    uint16_t version, flags, hdr_size, stack_pages;
    uint8_t  arch, abi, category, sort;
    uint32_t elf_size, crc32;
    const void *elf;            /* into the caller's buffer                */
    const char *app_id;         /* NUL-terminated inside the buffer, or 0  */
    const uint16_t *types;      /* SN_* ids, or 0                          */
    int      ntypes;
    char     name[32], ext[8];
};

/* Validate the container and fill `out` (may be NULL). Returns 0, or a negative
 * AEX_E_* -- and prints a line naming what was wrong. */
int aex_parse(const void *file, uint64_t file_size, struct aex_info *out);

#define AEX_OK          0
#define AEX_E_SHORT    -1   /* smaller than the fixed header                */
#define AEX_E_MAGIC    -2   /* not "AEX1"                                   */
#define AEX_E_VERSION  -3   /* 0, or newer than this loader                 */
#define AEX_E_HDRSIZE  -4   /* hdr_size out of range or past the file       */
#define AEX_E_ELFSIZE  -5   /* elf_size does not fit the file               */
#define AEX_E_ARCH     -6   /* built for another machine or another ABI     */
#define AEX_E_FLAGS    -7   /* a flag bit this loader does not know         */
#define AEX_E_TLV      -8   /* the metadata region is malformed             */
#define AEX_E_CRC      -9   /* the integrity record disagrees with the bytes*/
#define AEX_E_NOCRC   -10   /* a v2 file with no integrity record           */
#define AEX_E_ELF     -11   /* the ELF inside was refused (see [elf] lines) */

/* How many v1 images this boot has accepted. The log line above says so once --
 * a disk full of them would otherwise bury everything else on the serial port --
 * so the COUNT is the machine-readable form, and it is what a test asserts on:
 * "did it print" depends on what ran first, a counter does not. */
uint32_t aex_v1_images(void);

/* Load an in-memory .aex (`file_size` bytes) into the current address space
 * (user pages) and return the entry point (0 on failure). Fills name/ext if
 * non-NULL. out_top (may be NULL) receives the page-aligned top of the image. */
uint64_t aex_load(const void *file, uint64_t file_size, char *out_name, char *out_ext,
                  uint64_t *out_top);

/* The same load, handing back everything the ELF said about itself -- the auxv
 * material, the thread pointer, the stack protection. A caller that has to
 * BUILD the process (exec.c) needs these; one that only needs somewhere to jump
 * does not. Returns 0 on success. */
int aex_load_image(const void *file, uint64_t file_size, char *out_name, char *out_ext,
                   struct elf_image *out);

/* The same two, told which FILE the buffer came from, so the loader can map a
 * program's read-only whole pages straight out of the page cache instead of
 * allocating a frame and copying into them (elf.h's elf_file_runs). `fh` is a
 * c/kernel/mm/pcache.h handle from pcache_file_open(), or -1 for "no identity"
 * -- which is exactly the old behaviour and is what a caller passes when it
 * has no path (see the note above wm_launch's call site).
 *
 * The handle is BORROWED. Each VMA the loader creates takes its own reference;
 * the caller still owns and must put the one it passed in, on success and on
 * failure alike. That is mmsys.c's rule for SYS_MMAP_FILE, unchanged, because
 * an ownership convention that differs between two callers of the same cache
 * is a leak or a double-free waiting for whichever one is read second. */
int aex_load_image_ex(const void *file, uint64_t file_size, char *out_name, char *out_ext,
                      struct elf_image *out, int fh);
uint64_t aex_load_ex(const void *file, uint64_t file_size, char *out_name, char *out_ext,
                     uint64_t *out_top, int fh);

/* Where the embedded ELF image starts and how long it is. The ELF used to begin
 * at a hardcoded +64 at every call site; it begins where the header says. */
int aex_elf_range(const void *file, uint64_t file_size, const void **elf, uint64_t *elf_size);

/* Validate + read the display metadata without loading. Returns 0 on success.
 * Kept at its v1 signature: c/kernel/gui/wm.c and c/kernel/exec/exec.c call it
 * with a buffer and no length, and both belong to other lines. */
int aex_info(const void *file, char *out_name, char *out_ext);

/* The user-stack hint, in pages, or 0 if the file does not carry one.
 *
 * THIS IS THE ANSWER TO A BUG NOBODY COULD SEE. c/kernel/gui/wm.c picks the
 * browser's 2048-page stack with `streq(name, "browser.aex")` -- and `name` is
 * the header's DISPLAY name, which is "Browser". The comparison has never once
 * matched, so the browser has been running on 1024 pages for as long as the
 * line has existed, with a comment beside it saying it needs 2048. The file can
 * state its own requirement now; the one-line change at that call site is a
 * negotiation with the line that owns wm.c. */
uint16_t aex_stack_pages(const void *file, uint64_t file_size);

#endif /* LOGIT_AEX_H */
