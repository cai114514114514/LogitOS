#include "part.h"
#include "crc32.h"

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
int   memcmp(const void *, const void *, size_t);

/* ---------------------------------------------------------------------------
 * Little-endian field readers. Both table formats are little-endian on disk and
 * this kernel only runs on x86, but reading through these instead of casting a
 * struct over the buffer costs nothing and removes the alignment question
 * entirely -- a GPT entry array is read at an arbitrary offset in a sector
 * buffer, and a 64-bit load off an odd address is undefined C even where the
 * hardware would have allowed it.
 * ------------------------------------------------------------------------- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

const char *part_scheme_name(int scheme)
{
    return scheme == PART_GPT ? "GPT" : scheme == PART_MBR ? "MBR" : "none";
}

static char hexd(unsigned v) { return (char)(v < 10 ? '0' + v : 'A' + (v - 10)); }

/* A GUID's first three fields are stored little-endian and the last two big-
 * endian. Printing all sixteen bytes in order is the classic way to produce a
 * GUID nobody can match against a table. */
void part_guid_str(const uint8_t g[16], char out[37])
{
    static const int order[16] = { 3,2,1,0, 5,4, 7,6, 8,9, 10,11,12,13,14,15 };
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hexd(g[order[i]] >> 4);
        out[o++] = hexd(g[order[i]] & 0xF);
    }
    out[o] = 0;
}

/* --------------------------------------------------------------------------
 * Accepting an entry
 * ------------------------------------------------------------------------ */

/* Does [s,s+c) fall entirely inside a device of `dev` sectors, with a non-zero
 * length and not starting at LBA 0? Written so nothing can overflow: the
 * obvious `s + c > dev` wraps for a crafted 64-bit length and would admit a
 * partition that then indexes the whole address space. */
static int in_range(uint64_t s, uint64_t c, uint64_t dev)
{
    if (c == 0 || s == 0) return 0;
    if (s >= dev) return 0;
    return c <= dev - s;
}

static int overlaps(const struct part_table *t, uint64_t s, uint64_t c)
{
    int n = 0;
    for (int i = 0; i < t->count; i++) {
        uint64_t a = t->e[i].start, b = a + t->e[i].count;
        if (s < b && a < s + c) n++;
    }
    return n;
}

/* Append, counting a rejected entry as `skipped` rather than failing the scan:
 * one bad row in a table is a fact about the disk to report, not a reason to
 * refuse to mount the other three. */
static void add(struct part_table *t, const struct part_entry *e, uint64_t dev)
{
    if (!in_range(e->start, e->count, dev)) { t->skipped++; return; }
    if (t->count >= PART_MAX)               { t->truncated++; t->skipped++; return; }
    t->overlaps += overlaps(t, e->start, e->count);
    t->e[t->count++] = *e;
}

/* --------------------------------------------------------------------------
 * MBR
 * ------------------------------------------------------------------------ */

#define MBR_TABLE_OFF  0x1BE
#define MBR_SIG_OFF    0x1FE

static int is_extended(uint8_t type) { return type == 0x05 || type == 0x0F || type == 0x85; }
static int mbr_signed(const uint8_t *sec) { return sec[MBR_SIG_OFF] == 0x55 && sec[MBR_SIG_OFF + 1] == 0xAA; }

static void mbr_entry(const uint8_t *raw, struct part_entry *e)
{
    memset(e, 0, sizeof *e);
    e->bootable = (raw[0] == 0x80);
    e->type_mbr = raw[4];
    e->start    = rd32(raw + 8);
    e->count    = rd32(raw + 12);
}

/* Walk the extended-partition chain.
 *
 * The chain is a linked list built out of sectors the disk itself supplies, so
 * it is trivially forgeable into a cycle -- an EBR whose "next" pointer names an
 * EBR already visited, or itself. Three independent things stop that here, and
 * the belt-and-braces is deliberate because the failure mode is a kernel that
 * hangs at boot with no output:
 *   1. the next EBR must lie strictly AFTER the current one, which makes the
 *      walk monotonically increasing and so finite;
 *   2. every EBR must lie inside the extended partition that contains it;
 *   3. a hard iteration cap, so even a bug in (1)/(2) terminates.
 * Real chains are written in ascending order, so (1) costs nothing legitimate. */
static void mbr_chain(part_read_fn rd, void *ctx, uint64_t dev,
                      uint64_t ext_start, uint64_t ext_count, struct part_table *t)
{
    uint8_t sec[PART_SECTOR];
    uint64_t ebr = ext_start;
    uint64_t prev = 0;

    for (int guard = 0; guard < 128; guard++) {
        if (ebr <= prev && prev != 0) break;              /* (1) must move forward */
        if (ebr < ext_start || ebr >= ext_start + ext_count) break;   /* (2) inside the container */
        if (ebr >= dev) break;
        if (rd(ctx, ebr, 1, sec) != 0) break;
        if (!mbr_signed(sec)) break;

        struct part_entry e;
        mbr_entry(sec + MBR_TABLE_OFF, &e);               /* slot 0: the logical partition */
        if (e.count != 0 && e.type_mbr != 0 && !is_extended(e.type_mbr)) {
            uint64_t s = ebr + e.start;                   /* relative to THIS EBR */
            if (e.start != 0 && s >= ebr) {
                e.start = s;
                e.logical = 1;
                add(t, &e, dev);
            } else {
                t->skipped++;
            }
        } else if (e.count != 0) {
            t->skipped++;
        }

        struct part_entry nx;
        mbr_entry(sec + MBR_TABLE_OFF + 16, &nx);         /* slot 1: the next EBR */
        if (nx.count == 0 || !is_extended(nx.type_mbr) || nx.start == 0) break;
        prev = ebr;
        ebr  = ext_start + nx.start;                      /* relative to the CONTAINER, not the EBR */
    }
}

static int mbr_scan(part_read_fn rd, void *ctx, uint64_t dev, const uint8_t *sec0,
                    struct part_table *t)
{
    t->scheme = PART_MBR;
    for (int i = 0; i < 4; i++) {
        struct part_entry e;
        mbr_entry(sec0 + MBR_TABLE_OFF + i * 16, &e);
        if (e.type_mbr == 0 || e.count == 0) continue;    /* an empty slot is not an error */
        if (is_extended(e.type_mbr)) {
            /* The container itself is not a partition; only its chain is. Bound
             * it against the device first so a bogus container can't send the
             * walk outside the disk. */
            if (in_range(e.start, e.count, dev)) mbr_chain(rd, ctx, dev, e.start, e.count, t);
            else t->skipped++;
            continue;
        }
        add(t, &e, dev);
    }
    if (t->count == 0 && t->skipped == 0) t->scheme = PART_NONE;   /* signature only, no table */
    return t->count;
}

/* --------------------------------------------------------------------------
 * GPT
 * ------------------------------------------------------------------------ */

#define GPT_HDR_MIN   92
#define GPT_ENT_MIN   128
#define GPT_ENT_MAX   PART_SECTOR    /* an entry must fit in one sector read */
#define GPT_NUM_MAX   1024           /* 128 is the norm; this bounds the CRC walk */

static int gpt_hdr_ok(const uint8_t *h)
{
    static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    if (memcmp(h, sig, 8) != 0) return 0;
    uint32_t hsz = rd32(h + 12);
    if (hsz < GPT_HDR_MIN || hsz > PART_SECTOR) return 0;

    /* The header's own CRC is computed with the CRC field read as zero, so it
     * has to be zeroed in a copy before checksumming. Doing it in place would
     * corrupt the buffer the caller is about to parse. */
    uint8_t copy[PART_SECTOR];
    memcpy(copy, h, hsz);
    memset(copy + 16, 0, 4);
    return crc32(copy, hsz) == rd32(h + 16);
}

/* Read the entry array, checksum it, and parse it -- in ONE streaming pass.
 *
 * A 128-entry array is 16 KiB; holding it would be a bigger buffer than the
 * kernel stack has to spare, and the array is only ever consumed once. So the
 * CRC is folded sector by sector while a 512-byte assembly window reconstructs
 * entries that straddle a sector boundary (legal: entry_size need not divide
 * 512). Entries are parsed as they stream past, into `t`, and `t` is discarded
 * wholesale if the CRC turns out wrong -- which is the only correct order,
 * because an array that fails its CRC must yield nothing at all, not the
 * prefix that happened to look plausible. */
static int gpt_entries(part_read_fn rd, void *ctx, uint64_t dev, const uint8_t *h,
                       struct part_table *t)
{
    uint64_t ent_lba = rd64(h + 72);
    uint32_t num     = rd32(h + 80);
    uint32_t esz     = rd32(h + 84);
    uint32_t want    = rd32(h + 88);

    if (num == 0 || num > GPT_NUM_MAX) return -1;
    if (esz < GPT_ENT_MIN || esz > GPT_ENT_MAX || (esz & 7) != 0) return -1;
    if (ent_lba == 0 || ent_lba >= dev) return -1;

    uint64_t total = (uint64_t)num * esz;
    uint64_t nsec  = (total + PART_SECTOR - 1) / PART_SECTOR;
    if (nsec > dev - ent_lba) return -1;

    uint8_t  sec[PART_SECTOR], ent[GPT_ENT_MAX];
    uint32_t fill = 0;
    uint32_t crc  = CRC32_INIT;

    for (uint64_t s = 0; s < nsec; s++) {
        if (rd(ctx, ent_lba + s, 1, sec) != 0) return -1;
        uint64_t left = total - s * PART_SECTOR;
        uint32_t n = left < PART_SECTOR ? (uint32_t)left : PART_SECTOR;
        crc = crc32_update(crc, sec, n);

        for (uint32_t o = 0; o < n; ) {
            uint32_t take = esz - fill;
            if (take > n - o) take = n - o;
            memcpy(ent + fill, sec + o, take);
            fill += take;
            o    += take;
            if (fill < esz) continue;
            fill = 0;

            /* An all-zero type GUID means "unused slot" -- the normal state of
             * 120 of the 128 rows, so it is not counted as skipped. */
            int used = 0;
            for (int i = 0; i < 16; i++) if (ent[i]) { used = 1; break; }
            if (!used) continue;

            struct part_entry e;
            memset(&e, 0, sizeof e);
            memcpy(e.type_guid, ent, 16);
            uint64_t first = rd64(ent + 32), last = rd64(ent + 40);
            if (last < first) { t->skipped++; continue; }   /* inverted range */
            e.start    = first;
            e.count    = last - first + 1;                  /* last_lba is INCLUSIVE */
            e.bootable = (uint8_t)(rd64(ent + 48) & 0x4) ? 1 : 0;   /* legacy BIOS bootable */

            /* Name: 36 UTF-16LE units. Transliterated, not decoded -- this string
             * exists to be read in a boot log, and the kernel log is ASCII. */
            int o2 = 0;
            for (int i = 0; i < 36 && o2 < PART_NAME_MAX - 1; i++) {
                uint16_t u = rd16(ent + 56 + i * 2);
                if (u == 0) break;
                e.name[o2++] = (u >= 0x20 && u < 0x7F) ? (char)u : '?';
            }
            e.name[o2] = 0;
            add(t, &e, dev);
        }
    }

    if (crc32_final(crc) != want) return -1;
    return t->count;
}

/* Try the header at `lba`. On success the table is filled; on failure `t` is
 * left clean so the caller can try the backup header without inheriting a
 * half-parsed table from the primary. */
static int gpt_try(part_read_fn rd, void *ctx, uint64_t dev, uint64_t lba, struct part_table *t)
{
    uint8_t hdr[PART_SECTOR];
    if (lba >= dev) return -1;
    if (rd(ctx, lba, 1, hdr) != 0) return -1;
    if (!gpt_hdr_ok(hdr)) return -1;

    int saved_prot = t->protective;
    memset(t, 0, sizeof *t);
    t->protective = saved_prot;
    t->scheme = PART_GPT;
    if (gpt_entries(rd, ctx, dev, hdr, t) < 0) {
        int p = t->protective;
        memset(t, 0, sizeof *t);
        t->protective = p;
        return -1;
    }
    return t->count;
}

/* --------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------ */

int part_scan(part_read_fn rd, void *ctx, uint64_t dev_sectors, struct part_table *t)
{
    if (!rd || !t) return -1;
    memset(t, 0, sizeof *t);
    if (dev_sectors < 2) return 0;

    uint8_t sec0[PART_SECTOR];
    if (rd(ctx, 0, 1, sec0) != 0) return 0;
    if (!mbr_signed(sec0)) return 0;          /* no boot signature: not a partitioned disk */

    int protective = 0;
    for (int i = 0; i < 4; i++)
        if (sec0[MBR_TABLE_OFF + i * 16 + 4] == 0xEE) protective = 1;

    /* A protective MBR says "GPT lives here"; try GPT first when we see one.
     * But a disk can also carry a real GPT behind a non-protective MBR (a hybrid
     * layout, and some partitioners get the 0xEE wrong), so GPT is attempted
     * either way -- the header signature and two CRCs are strong enough evidence
     * that a false positive is not a practical worry, and they are exactly the
     * check a "trust the 0xEE byte" implementation skips. */
    t->protective = protective;
    if (gpt_try(rd, ctx, dev_sectors, 1, t) >= 0) return t->count;

    /* The primary header is where a half-finished write or a bad sector lands
     * first; the backup at the last LBA is the whole reason GPT writes two. */
    if (gpt_try(rd, ctx, dev_sectors, dev_sectors - 1, t) >= 0) {
        t->backup_used = 1;
        return t->count;
    }

    /* No usable GPT. If the MBR was protective there is nothing else to read --
     * its single 0xEE row covers the disk and is not a partition. */
    memset(t, 0, sizeof *t);
    t->protective = protective;
    if (protective) return 0;

    return mbr_scan(rd, ctx, dev_sectors, sec0, t);
}
