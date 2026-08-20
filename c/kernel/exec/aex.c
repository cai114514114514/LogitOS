#include <stdint.h>
#include "aex.h"
#include "elf.h"
#include "crc32.h"      /* c/drivers/block: the one CRC-32 in the tree */
#include "kprintf.h"

/* WEAK for the reason elf.c gives at its own copy: tests/unit/exechost links
 * this file with no filesystem under it, so the symbol is absent, the streaming
 * entry points refuse, and the memory ones are unaffected. */
int vfs_pread(const char *path, void *buf, int max, long long off) __attribute__((weak));

/* See aex.h for the format, the version rules, and why AEX keeps an ELF inside
 * it rather than describing its own segments. This file is the reader. */

/* One refusal, one line, one code -- the same discipline elf.c holds to, and
 * for the same reason: on a machine with no debugger, a loader that says no
 * without saying why is a loader you cannot work on. */
static int reject(int code, const char *what, uint64_t a, uint64_t b)
{
    kprintf("[aex] refused (%d): %s (%p, %p)\n", code, what, (void *)a, (void *)b);
    return code;
}

static void copy_field(char *dst, const char *src, int n)
{
    int i = 0;
    for (; i < n - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Printed at most once per boot per kind, because a disk full of v1 files would
 * otherwise bury everything else on the serial log. The point of the line is
 * that the acceptance is DELIBERATE and visible, not that it is loud. */
static int g_said_v1;
static uint32_t g_v1_images;

uint32_t aex_v1_images(void) { return g_v1_images; }

/* THE CONTAINER READER, over the same source the ELF loader uses.
 *
 * `rd->mem` non-NULL is the old shape and behaves exactly as it did, down to
 * `out->elf` and the two metadata pointers aiming into the caller's buffer.
 * `rd->path` is the streaming shape, and there ARE two things it cannot report
 * -- see the comment on app_id below. Everything else, including every refusal
 * and every bound, is one code path: the container's checks are the ones that
 * decide whether disk-controlled bytes get loaded in ring 0, and a second copy
 * of them that only the streaming caller runs is exactly the shape CLAUDE.md
 * records for the cookie jar (one rule, two doors, and the gate knew one). */
static int aex_parse_src(const struct elf_reader *rd, struct aex_info *out)
{
    struct aex_info tmp;
    if (!out) out = &tmp;
    for (unsigned i = 0; i < sizeof *out; i++) ((uint8_t *)out)[i] = 0;

    uint64_t file_size = rd->size;
    const uint8_t *p = rd->mem;          /* NULL on the streaming path */
    struct aex_header hb;
    const struct aex_header *h = &hb;

    if (file_size >= AEX_HDR_SIZE && elf_read(rd, 0, &hb, AEX_HDR_SIZE) < 0)
        return reject(AEX_E_SHORT, "could not read the AEX header", 0, AEX_HDR_SIZE);

    if (file_size < AEX_HDR_SIZE)
        return reject(AEX_E_SHORT, "file shorter than the AEX header", file_size, AEX_HDR_SIZE);
    if (h->magic[0] != 'A' || h->magic[1] != 'E' || h->magic[2] != 'X' || h->magic[3] != '1')
        return reject(AEX_E_MAGIC, "not an AEX1 file", h->magic[0], h->magic[1]);
    if (h->version < AEX_VERSION_MIN || h->version > AEX_VERSION_MAX)
        return reject(AEX_E_VERSION, "AEX version this loader does not implement",
                      h->version, AEX_VERSION_MAX);

    copy_field(out->name, h->name, 32);
    copy_field(out->ext, h->ext, 8);
    out->version = h->version;

    if (h->version == 1) {
        /* THE OLD FORMAT, ACCEPTED ON PURPOSE.
         *
         * v1 is a 64-byte header and an ELF at a hardcoded +64, with no size,
         * no checksum, no architecture and no metadata -- and it is what every
         * disk image built before this change holds. Refusing it would make a
         * stale build unbootable for a reason that has nothing to do with the
         * program; mis-loading it silently is the thing the format change is
         * supposed to end. So: accepted, with the v1 rules spelled out here
         * rather than inherited by accident, and said out loud once.
         *
         * `flags`, `hdr_size`, `stack_pages`, `arch`, `category` and `elf_size`
         * are NOT read for a v1 file. In v1 those twelve bytes were `pad` and
         * mkaex.py wrote zeroes into them, but "the tool wrote zeroes" is not a
         * guarantee the format made, and reading them would be reading padding. */
        out->hdr_size = AEX_HDR_SIZE;
        out->elf_size = (uint32_t)(file_size - AEX_HDR_SIZE);
        out->arch = AEX_ARCH_X86_64;
        out->abi  = AEX_ABI_LOGIT1;
        out->elf  = p ? p + AEX_HDR_SIZE : 0;
        if (file_size <= AEX_HDR_SIZE)
            return reject(AEX_E_ELFSIZE, "v1 file with no ELF after the header",
                          file_size, AEX_HDR_SIZE);
        g_v1_images++;
        if (!g_said_v1) {
            g_said_v1 = 1;
            kprintf("[aex] '%s' is a v1 image: no size, no architecture and no "
                    "integrity record. Accepted; rebuild to get one.\n", out->name);
        }
        return AEX_OK;
    }

    /* ---- v2 ------------------------------------------------------------ */
    if (h->flags & ~(uint16_t)AEX_F_KNOWN)
        return reject(AEX_E_FLAGS, "a flag bit this loader does not know",
                      h->flags, AEX_F_KNOWN);
    if (h->arch != AEX_ARCH_X86_64)
        return reject(AEX_E_ARCH, "built for another machine", h->arch, AEX_ARCH_X86_64);
    if (h->abi != AEX_ABI_LOGIT1)
        return reject(AEX_E_ARCH, "built against another syscall ABI", h->abi, AEX_ABI_LOGIT1);
    if (h->hdr_size < AEX_HDR_SIZE || h->hdr_size > AEX_HDR_MAX || (h->hdr_size & 7))
        return reject(AEX_E_HDRSIZE, "hdr_size out of range or not 8-aligned",
                      h->hdr_size, AEX_HDR_MAX);
    if ((uint64_t)h->hdr_size > file_size)
        return reject(AEX_E_HDRSIZE, "hdr_size past the end of the file",
                      h->hdr_size, file_size);
    if ((uint64_t)h->elf_size > file_size - h->hdr_size)
        return reject(AEX_E_ELFSIZE, "elf_size does not fit after the header",
                      h->elf_size, file_size - h->hdr_size);
    if (h->elf_size == 0)
        return reject(AEX_E_ELFSIZE, "elf_size is 0", 0, 0);

    out->flags = h->flags;
    out->hdr_size = h->hdr_size;
    out->stack_pages = h->stack_pages;
    out->arch = h->arch;
    out->abi = h->abi;
    out->category = h->category;
    out->sort = h->sort;
    out->elf_size = h->elf_size;
    out->elf = p ? p + h->hdr_size : 0;

    /* ---- the TLV region -------------------------------------------------
     * {u32 tag, u32 len, u8 val[len]}, each record advancing to the next
     * 8-byte boundary. Bounded exactly like the program-header table in elf.c:
     * `len` is disk-controlled, so it is compared against what REMAINS rather
     * than added to the offset. An unknown tag is skipped -- metadata is
     * allowed to grow without breaking an old loader, which is the half of the
     * compatibility story `flags` deliberately does not get. */
    int have_crc = 0;
    uint32_t want_crc = 0;
    for (uint32_t off = AEX_HDR_SIZE; off + AEX_TLV_HDR <= h->hdr_size; ) {
        uint8_t rec[AEX_TLV_HDR];
        if (elf_read(rd, off, rec, AEX_TLV_HDR) < 0)
            return reject(AEX_E_TLV, "could not read a metadata record header", off, 0);
        uint32_t tag = le32(rec);
        uint32_t len = le32(rec + 4);
        if (len > (uint32_t)h->hdr_size - off - AEX_TLV_HDR)
            return reject(AEX_E_TLV, "a metadata record runs past the header", off, len);
        /* Only on the memory path is there anything to point AT. */
        const uint8_t *val = p ? p + off + AEX_TLV_HDR : 0;
        uint64_t voff = (uint64_t)off + AEX_TLV_HDR;

        switch (tag) {
        case AEX_T_CRC32: {
            if (len != 4) return reject(AEX_E_TLV, "the CRC record is not 4 bytes", off, len);
            uint8_t v[4];
            if (elf_read(rd, voff, v, 4) < 0)
                return reject(AEX_E_TLV, "could not read the CRC record", voff, 4);
            want_crc = le32(v);
            have_crc = 1;
            break;
        }
        case AEX_T_APPID: {
            /* The NUL is CHECKED on both paths and the string is only REPORTED
             * on one. The check is what stops a malformed record reaching a
             * caller as an unterminated string, and skipping it on the
             * streaming path would mean the container's validation depends on
             * which door the loader came through -- so one byte is read for it.
             *
             * The pointer is the part that cannot be honoured: it would have to
             * aim into a buffer somebody keeps alive for the caller, and the
             * whole point here is that no such buffer exists. Nothing in the
             * tree reads app_id or types today (grepped, 2026-08-20 -- wm.c's
             * `next_app_id` is an unrelated counter), so this reports 0 rather
             * than growing struct aex_info a 64-byte inline copy for a field
             * with no consumer. Whoever adds the first consumer adds the copy. */
            uint8_t last;
            if (!len) return reject(AEX_E_TLV, "the app id is not NUL-terminated", off, len);
            if (elf_read(rd, voff + len - 1, &last, 1) < 0)
                return reject(AEX_E_TLV, "could not read the app id", voff, len);
            if (last != 0)
                return reject(AEX_E_TLV, "the app id is not NUL-terminated", off, len);
            if (val) out->app_id = (const char *)val;
            break;
        }
        case AEX_T_TYPES:
            if (len & 1) return reject(AEX_E_TLV, "the type list is not a u16 array", off, len);
            if (val) {
                out->types = (const uint16_t *)(const void *)val;
                out->ntypes = (int)(len / 2);
            }
            break;
        default:
            break;                       /* unknown tag: ignored, on purpose */
        }
        off += (AEX_TLV_HDR + len + 7u) & ~7u;
    }

    /* THE INTEGRITY RECORD IS NOT OPTIONAL for v2. "No size, no checksum, no
     * signature" was the survey's finding, and a checksum a file may leave out
     * is not a checksum -- it is a checksum for the files that already work.
     *
     * What it is and is not: it is CRC-32 over the ELF image, so a truncated
     * write, a half-flushed disk or a bit-rotted block is refused by name
     * instead of arriving in ring 3 as a fault with no explanation. It is NOT
     * a signature. There is deliberately none: a signature needs a key this
     * system has nowhere to keep and a policy for what to do when it fails,
     * and a CRC that reads as authentication is worse than no CRC. The related
     * gap stays open and is not this line's to close -- tools/mkfs.py stores no
     * mode bit, so "executable" still means "the bytes start with AEX1", and
     * anything a program can write is launchable.
     *
     * COST, MEASURED AND NOT GUESSED, because the first draft of this comment
     * said "/bin/sh is ~100 KiB, which is noise" and exec.c's own per-exec
     * report says otherwise: execve went from ~700 kcycles to ~2000, of which
     * ~1750 is now inside aex_load. One pass over the image at two shifts a
     * byte is roughly 14 cycles per byte, and /bin/sh is 110 KiB, so the CRC is
     * most of an exec now.
     *
     * Kept anyway, and the reasoning is about what an exec costs in absolute
     * terms rather than in ratios: 2 Mcycles is under a millisecond on any real
     * core, the shell does one per command, and nothing here is at a rate where
     * that is visible. The ratio looks bad because exec had already been made
     * cheap (the 256-page eager stack became a VMA reservation), which is a
     * good problem. If it ever does matter the fix is not to drop the check: it
     * is c/drivers/block/crc32.c's 16-entry nibble table, chosen when the only
     * caller was GPT reading a few kilobytes per boot. A 256-entry table halves
     * this, and that file belongs to the block line. */
    if (!have_crc)
        return reject(AEX_E_NOCRC, "a v2 image with no integrity record", h->version, 0);
    /* Through the reader, so the streaming path folds the image 16 KiB at a
     * time instead of needing all of it at once. On the memory path this is
     * the same single crc32() call it always was. */
    if (elf_read_crc32(rd, h->hdr_size, out->elf_size, &out->crc32) < 0)
        return reject(AEX_E_CRC, "could not read the image to check its CRC-32",
                      h->hdr_size, out->elf_size);
    if (out->crc32 != want_crc)
        return reject(AEX_E_CRC, "the image does not match its CRC-32",
                      out->crc32, want_crc);

    return AEX_OK;
}

int aex_parse(const void *file, uint64_t file_size, struct aex_info *out)
{
    struct elf_reader rd = { .mem = (const uint8_t *)file, .path = 0,
                             .base = 0, .size = file_size };
    return aex_parse_src(&rd, out);
}

int aex_parse_path(const char *path, uint64_t file_size, struct aex_info *out)
{
    struct elf_reader rd = { .mem = 0, .path = path, .base = 0, .size = file_size };
    return aex_parse_src(&rd, out);
}

int aex_info(const void *file, char *out_name, char *out_ext)
{
    /* No length, so this can validate the FIXED header and nothing else: no
     * TLV walk, no CRC. Kept at that signature because c/kernel/gui/wm.c's Dock
     * scan and exec.c both call it this way and both belong to other lines.
     * Everything it reads is at an offset v1 and v2 agree on. */
    const struct aex_header *h = file;
    if (h->magic[0] != 'A' || h->magic[1] != 'E' || h->magic[2] != 'X' || h->magic[3] != '1')
        return -1;
    if (h->version < AEX_VERSION_MIN || h->version > AEX_VERSION_MAX)
        return -1;
    if (out_name) copy_field(out_name, h->name, 32);
    if (out_ext)  copy_field(out_ext, h->ext, 8);
    return 0;
}

int aex_elf_range(const void *file, uint64_t file_size, const void **elf, uint64_t *elf_size)
{
    struct aex_info in;
    if (aex_parse(file, file_size, &in) != AEX_OK)
        return -1;
    if (elf)      *elf = in.elf;
    if (elf_size) *elf_size = in.elf_size;
    return 0;
}

uint16_t aex_stack_pages(const void *file, uint64_t file_size)
{
    struct aex_info in;
    if (aex_parse(file, file_size, &in) != AEX_OK) return 0;
    return in.stack_pages;
}

int aex_load_image(const void *file, uint64_t file_size, char *out_name, char *out_ext,
                   struct elf_image *out)
{
    return aex_load_image_ex(file, file_size, out_name, out_ext, out, -1);
}

/* The container's half of the file-backed path, and the only thing it adds is
 * ARITHMETIC: the ELF's p_offset is relative to the ELF image, and the page
 * cache indexes the FILE, so somebody has to add hdr_size. This is the one
 * place that knows it -- exec.c has the path and the size but not the header,
 * elf.c has the header but not the file. `fh` is borrowed: the caller opened
 * it and the caller puts it, whatever happens here.
 *
 * `file_size` is the caller's buffer length, which is the file rounded UP to a
 * sector (exec.c reads in 512-byte units), so file_pages is derived from the
 * true byte count -- hdr_size + elf_size -- and never from the padding. A page
 * index past the real end of the file reads back as zeroes through
 * pcache_get()'s tail handling, which for an executable page means a program
 * that jumps into 4 KiB of `add [rax],al`. */
int aex_load_image_ex(const void *file, uint64_t file_size, char *out_name, char *out_ext,
                      struct elf_image *out, int fh)
{
    struct aex_info in;
    if (aex_parse(file, file_size, &in) != AEX_OK)
        return -1;
    if (out_name) copy_field(out_name, in.name, 32);
    if (out_ext)  copy_field(out_ext, in.ext, 8);

    uint64_t bytes = (uint64_t)in.hdr_size + in.elf_size;
    struct elf_src src = {
        .fh = fh,
        .base_off = in.hdr_size,
        .file_pages = (bytes + 0xFFF) >> 12,
    };
    return elf_load_image_ex((void *)in.elf, in.elf_size, out, &src) == ELF_OK
               ? 0 : AEX_E_ELF;
}

/* THE STREAMING LOAD -- the same function with no buffer behind it.
 *
 * `file_size` is the file's TRUE byte count (vfs_size), not a sector-rounded
 * buffer length, because there is no buffer: every read is bounded against the
 * ELF image's own size and a short read is a refusal, so a rounded-up length
 * would let the container claim bytes the file does not have.
 *
 * That is a real difference from aex_load_image_ex above, whose `file_size` IS
 * the rounded buffer length (exec.c read in 512-byte units) and which derives
 * file_pages from hdr_size + elf_size for exactly that reason. Here the two
 * agree and the derivation is kept identical anyway, so the page-cache runs
 * this produces are byte-for-byte the ones the eager path produced. */

/* ...and its instrument, which the function below needs, hence the two blocks.
 *
 * THE SPLIT, because the average was hiding the answer.
 *
 * exec.c's per-exec report is one number for the whole load, and on this path
 * it is dominated by something that is not the loader: the container's
 * integrity record is a CRC-32 over the ENTIRE ELF image, so a 64 MiB program
 * is 64 MiB of disk read and 64 MiB of CRC before a single page is mapped --
 * while the mapping itself, with the page cache doing its job, touches three
 * pages. Two numbers say that; one number says "loading is slow".
 *
 * Bounded at the first 24 loads for the reason exec_note_load gives about its
 * own line: a boot that never reaches a shell does two loads. */
static uint32_t g_ld_seen;
#define AEX_LOAD_REPORT_MAX 24

static inline uint64_t aex_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int aex_load_path(const char *path, uint64_t file_size, char *out_name, char *out_ext,
                  struct elf_image *out, int fh)
{
    uint64_t t0 = aex_rdtsc();
    struct aex_info in;
    if (aex_parse_path(path, file_size, &in) != AEX_OK)
        return -1;
    uint64_t t1 = aex_rdtsc();
    if (out_name) copy_field(out_name, in.name, 32);
    if (out_ext)  copy_field(out_ext, in.ext, 8);

    uint64_t bytes = (uint64_t)in.hdr_size + in.elf_size;
    struct elf_src src = {
        .fh = fh,
        .base_off = in.hdr_size,
        .file_pages = (bytes + 0xFFF) >> 12,
    };
    struct elf_reader rd = { .mem = 0, .path = path,
                             .base = in.hdr_size, .size = in.elf_size };
    int rc = elf_load_reader(&rd, out, &src) == ELF_OK ? 0 : AEX_E_ELF;
    uint64_t t2 = aex_rdtsc();
    if (g_ld_seen++ < AEX_LOAD_REPORT_MAX)
        kprintf("[aex] %s: %d KiB, container+crc %d Mcyc, elf %d Mcyc\n",
                path, (int)(file_size / 1024),
                (int)((t1 - t0) / 1000000), (int)((t2 - t1) / 1000000));
    return rc;
}

/* aex_info() without a buffer: the 64 bytes every version agrees on, read from
 * the file. Same contract, same refusals -- it validates the FIXED header and
 * nothing else, no TLV walk and no CRC, because its callers (the Dock scan, and
 * exec.c's early "is this even a program" check before it tears down an address
 * space) want a cheap yes/no and a display name. */
int aex_info_path(const char *path, char *out_name, char *out_ext)
{
    struct aex_header h;
    if (!vfs_pread) return -1;
    if (vfs_pread(path, &h, AEX_HDR_SIZE, 0) != AEX_HDR_SIZE) return -1;
    return aex_info(&h, out_name, out_ext);
}

uint64_t aex_load(const void *file, uint64_t file_size, char *out_name, char *out_ext,
                  uint64_t *out_top)
{
    return aex_load_ex(file, file_size, out_name, out_ext, out_top, -1);
}

uint64_t aex_load_ex(const void *file, uint64_t file_size, char *out_name, char *out_ext,
                     uint64_t *out_top, int fh)
{
    struct elf_image img;
    if (out_top) *out_top = 0;
    if (aex_load_image_ex(file, file_size, out_name, out_ext, &img, fh) != 0)
        return 0;
    if (out_top) *out_top = img.top;
    return img.entry;
}
