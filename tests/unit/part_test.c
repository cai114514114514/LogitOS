/* Host unit tests for the partition-table parser (c/drivers/block/part.c).
 *
 * Everything here is a synthetic sector image built in memory and handed to
 * part_scan through the same read callback the kernel uses. That is the whole
 * reason part.c takes a callback: the parsing is where the risk is -- every
 * field comes off a disk somebody else formatted -- and none of that risk needs
 * a controller, a QEMU, or a boot to exercise.
 *
 * The cases that matter are the malformed ones. A well-formed GPT is a fixture;
 * a GPT whose entry-array CRC is one bit wrong, or an extended MBR chain that
 * points at itself, is a test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "part.h"
#include "crc32.h"

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void eq(long long got, long long want, const char *what)
{
    checks++;
    if (got != want) { failures++; printf("  FAIL: %s (got %lld, want %lld)\n", what, got, want); }
}

/* --------------------------------------------------------------------------
 * A disk made of sectors in memory
 * ------------------------------------------------------------------------ */

#define DISK_SECTORS 262144u              /* 128 MiB */

struct disk {
    uint8_t *data;
    uint64_t sectors;
    uint64_t backed;                      /* sectors actually allocated */
    int      fail_lba;                    /* read this LBA -> I/O error (-1 = none) */
    int      reads;
};

static struct disk *disk_new(uint64_t sectors, uint64_t backed)
{
    struct disk *d = calloc(1, sizeof *d);
    d->sectors = sectors;
    d->backed  = backed;
    d->data    = calloc((size_t)backed, 512);
    d->fail_lba = -1;
    return d;
}
static void disk_free(struct disk *d) { free(d->data); free(d); }
static uint8_t *sec(struct disk *d, uint64_t lba) { return d->data + lba * 512; }

static int disk_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    struct disk *d = ctx;
    d->reads++;
    if (d->fail_lba >= 0 && lba == (uint64_t)d->fail_lba) return -1;
    if (lba + count > d->backed) return -1;
    memcpy(buf, sec(d, lba), (size_t)count * 512);
    return 0;
}

static void wr32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

/* --------------------------------------------------------------------------
 * MBR builders
 * ------------------------------------------------------------------------ */

static void mbr_sign(uint8_t *s) { s[510] = 0x55; s[511] = 0xAA; }

static void mbr_put(uint8_t *s, int slot, uint8_t type, uint32_t start, uint32_t count, int boot)
{
    uint8_t *e = s + 0x1BE + slot * 16;
    e[0] = boot ? 0x80 : 0x00;
    e[4] = type;
    wr32(e + 8, start);
    wr32(e + 12, count);
}

/* --------------------------------------------------------------------------
 * GPT builders
 * ------------------------------------------------------------------------ */

static const uint8_t TYPE_LINUX[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};

struct gptspec {
    uint64_t hdr_lba;
    uint64_t ent_lba;
    uint32_t num;
    uint32_t esz;
    int      nparts;
    uint64_t first[8], last[8];
    const char *name[8];
};

/* Writes a valid GPT (entries + header, both CRCs correct). The corruption
 * tests then poke one byte and re-run, which is the only honest way to show a
 * CRC check is load-bearing: same image, one bit different. */
static void gpt_build(struct disk *d, const struct gptspec *g)
{
    uint32_t esz = g->esz, num = g->num;
    uint8_t *arr = calloc(num, esz);
    for (int i = 0; i < g->nparts; i++) {
        uint8_t *e = arr + (size_t)i * esz;
        memcpy(e, TYPE_LINUX, 16);
        memset(e + 16, 0x11 + i, 16);              /* unique GUID: any non-zero */
        wr64(e + 32, g->first[i]);
        wr64(e + 40, g->last[i]);
        wr64(e + 48, 0);
        if (g->name[i])
            for (int k = 0; g->name[i][k] && k < 36; k++) {
                e[56 + k * 2]     = (uint8_t)g->name[i][k];
                e[56 + k * 2 + 1] = 0;
            }
    }
    memcpy(sec(d, g->ent_lba), arr, (size_t)num * esz);

    uint8_t *h = sec(d, g->hdr_lba);
    memset(h, 0, 512);
    memcpy(h, "EFI PART", 8);
    wr32(h + 8, 0x00010000);
    wr32(h + 12, 92);
    wr32(h + 16, 0);                               /* header CRC placeholder */
    wr64(h + 24, g->hdr_lba);
    wr64(h + 32, d->sectors - 1);
    wr64(h + 40, 34);
    wr64(h + 48, d->sectors - 34);
    memset(h + 56, 0xAB, 16);
    wr64(h + 72, g->ent_lba);
    wr32(h + 80, num);
    wr32(h + 84, esz);
    wr32(h + 88, crc32(arr, (size_t)num * esz));
    wr32(h + 16, crc32(h, 92));
    free(arr);
}

static void protective_mbr(struct disk *d)
{
    uint8_t *s = sec(d, 0);
    memset(s, 0, 512);
    mbr_sign(s);
    mbr_put(s, 0, 0xEE, 1, 0xFFFFFFFFu, 0);
}

/* --------------------------------------------------------------------------
 * Cases
 * ------------------------------------------------------------------------ */

static void t_no_table(void)
{
    printf("no table\n");
    struct disk *d = disk_new(DISK_SECTORS, 4096);
    struct part_table t;

    /* All zeroes: no boot signature. */
    eq(part_scan(disk_read, d, d->sectors, &t), 0, "zeroed disk yields nothing");
    eq(t.scheme, PART_NONE, "scheme none");

    /* A raw filesystem image: real content in sector 0, no 0x55AA. This is what
     * every disk image in this tree looks like today, and it must keep scanning
     * as "no table" rather than as garbage partitions. */
    memset(sec(d, 0), 0xA5, 512);
    sec(d, 0)[510] = 0; sec(d, 0)[511] = 0;
    eq(part_scan(disk_read, d, d->sectors, &t), 0, "raw fs image yields nothing");
    eq(t.scheme, PART_NONE, "raw fs image: scheme none");

    /* A signature but four empty slots is still no table. */
    memset(sec(d, 0), 0, 512);
    mbr_sign(sec(d, 0));
    eq(part_scan(disk_read, d, d->sectors, &t), 0, "signature with empty slots");
    eq(t.scheme, PART_NONE, "empty MBR reported as none");

    disk_free(d);
}

static void t_mbr_primary(void)
{
    printf("MBR primaries\n");
    struct disk *d = disk_new(DISK_SECTORS, 4096);
    uint8_t *s = sec(d, 0);
    mbr_sign(s);
    mbr_put(s, 0, 0x83, 2048, 20480, 1);
    mbr_put(s, 1, 0x0C, 22528, 20480, 0);
    mbr_put(s, 3, 0x83, 43008, 20480, 0);       /* out of order slots are legal */

    struct part_table t;
    eq(part_scan(disk_read, d, d->sectors, &t), 3, "three primaries");
    eq(t.scheme, PART_MBR, "scheme MBR");
    eq((long long)t.e[0].start, 2048, "p1 start");
    eq((long long)t.e[0].count, 20480, "p1 count");
    eq(t.e[0].bootable, 1, "p1 bootable");
    eq(t.e[0].type_mbr, 0x83, "p1 type");
    eq((long long)t.e[2].start, 43008, "p3 start (slot 3)");
    eq(t.overlaps, 0, "no overlaps");
    eq(t.skipped, 0, "nothing skipped");
    disk_free(d);
}

static void t_mbr_extended(void)
{
    printf("MBR extended chain\n");
    struct disk *d = disk_new(DISK_SECTORS, 200000);
    uint8_t *s = sec(d, 0);
    mbr_sign(s);
    mbr_put(s, 0, 0x83, 2048, 20480, 0);
    mbr_put(s, 1, 0x05, 40960, 100000, 0);      /* extended container */

    /* Three logicals. Slot 0 of each EBR is relative to that EBR; slot 1 is
     * relative to the CONTAINER. Getting those two bases the same way is the
     * classic extended-partition bug and it produces plausible-looking garbage,
     * so the offsets here are deliberately different from each other. */
    uint8_t *e1 = sec(d, 40960);
    mbr_sign(e1);
    mbr_put(e1, 0, 0x83, 2048, 10000, 0);       /* -> 43008 */
    mbr_put(e1, 1, 0x05, 20000, 80000, 0);      /* next EBR at 40960+20000 = 60960 */

    uint8_t *e2 = sec(d, 60960);
    mbr_sign(e2);
    mbr_put(e2, 0, 0x82, 63, 10000, 0);         /* -> 61023 */
    mbr_put(e2, 1, 0x05, 50000, 40000, 0);      /* next EBR at 40960+50000 = 90960 */

    uint8_t *e3 = sec(d, 90960);
    mbr_sign(e3);
    mbr_put(e3, 0, 0x83, 2048, 20000, 0);       /* -> 93008 */
    /* slot 1 empty: chain ends */

    struct part_table t;
    eq(part_scan(disk_read, d, d->sectors, &t), 4, "1 primary + 3 logicals");
    eq((long long)t.e[1].start, 43008, "logical 1 start is EBR-relative");
    eq(t.e[1].logical, 1, "logical 1 flagged");
    eq((long long)t.e[2].start, 61023, "logical 2 start");
    eq((long long)t.e[3].start, 93008, "logical 3 start is container-relative EBR");
    eq(t.e[0].logical, 0, "primary not flagged logical");
    disk_free(d);
}

static void t_mbr_chain_loops(void)
{
    printf("MBR extended chain that loops\n");

    /* Self-reference: EBR 0's "next" points back at itself. A walker without a
     * termination rule spins here forever, in the kernel, at boot, with no
     * output -- which is why this test exists and why the rule in part.c is
     * "strictly forward", not "not equal to the previous one". */
    {
        struct disk *d = disk_new(DISK_SECTORS, 200000);
        uint8_t *s = sec(d, 0);
        mbr_sign(s);
        mbr_put(s, 1, 0x05, 40960, 100000, 0);
        uint8_t *e1 = sec(d, 40960);
        mbr_sign(e1);
        mbr_put(e1, 0, 0x83, 2048, 10000, 0);
        mbr_put(e1, 1, 0x05, 0, 80000, 0);       /* offset 0 -> points at itself */
        struct part_table t;
        eq(part_scan(disk_read, d, d->sectors, &t), 1, "self-loop terminates with 1 logical");
        disk_free(d);
    }

    /* Two-EBR cycle: A -> B -> A. */
    {
        struct disk *d = disk_new(DISK_SECTORS, 200000);
        uint8_t *s = sec(d, 0);
        mbr_sign(s);
        mbr_put(s, 1, 0x05, 40960, 100000, 0);
        uint8_t *a = sec(d, 40960);
        mbr_sign(a);
        mbr_put(a, 0, 0x83, 2048, 10000, 0);
        mbr_put(a, 1, 0x05, 20000, 80000, 0);    /* -> 60960 */
        uint8_t *b = sec(d, 60960);
        mbr_sign(b);
        mbr_put(b, 0, 0x83, 2048, 10000, 0);
        mbr_put(b, 1, 0x05, 0, 80000, 0);        /* -> 40960, backwards */
        struct part_table t;
        eq(part_scan(disk_read, d, d->sectors, &t), 2, "A->B->A terminates with 2 logicals");
        disk_free(d);
    }

    /* A chain longer than the iteration cap: 300 EBRs, each pointing forward.
     * Terminates because of the cap alone, which is the backstop for a bug in
     * the ordering rule rather than for a malicious disk. */
    {
        struct disk *d = disk_new(DISK_SECTORS, 200000);
        uint8_t *s = sec(d, 0);
        mbr_sign(s);
        mbr_put(s, 1, 0x05, 1000, 150000, 0);
        for (int i = 0; i < 300; i++) {
            uint8_t *e = sec(d, 1000 + (uint64_t)i * 400);
            mbr_sign(e);
            mbr_put(e, 0, 0x83, 100, 200, 0);
            mbr_put(e, 1, 0x05, (uint32_t)((i + 1) * 400), 150000, 0);
        }
        struct part_table t;
        int n = part_scan(disk_read, d, d->sectors, &t);
        ok(n >= 0 && n <= PART_MAX, "300-EBR chain terminates and is capped");
        ok(t.truncated > 0, "truncation reported rather than silently dropped");
        disk_free(d);
    }
}

static void t_mbr_out_of_range(void)
{
    printf("MBR entries that overflow the device\n");
    struct disk *d = disk_new(4096, 4096);      /* a 2 MiB disk */
    uint8_t *s = sec(d, 0);
    mbr_sign(s);
    mbr_put(s, 0, 0x83, 2048, 1024, 0);         /* fine */
    mbr_put(s, 1, 0x83, 3072, 99999, 0);        /* runs off the end */
    mbr_put(s, 2, 0x83, 999999, 100, 0);        /* starts off the end */
    mbr_put(s, 3, 0x83, 0, 100, 0);             /* starts at the MBR itself */

    struct part_table t;
    eq(part_scan(disk_read, d, d->sectors, &t), 1, "only the in-range entry accepted");
    eq(t.skipped, 3, "three entries rejected");
    eq((long long)t.e[0].start, 2048, "the survivor is the right one");
    disk_free(d);
}

static void t_mbr_zero_and_overlap(void)
{
    printf("MBR zero-length and overlapping entries\n");
    struct disk *d = disk_new(DISK_SECTORS, 4096);
    uint8_t *s = sec(d, 0);
    mbr_sign(s);
    mbr_put(s, 0, 0x83, 2048, 0, 0);            /* zero length: not a partition */
    mbr_put(s, 1, 0x83, 2048, 20480, 0);
    mbr_put(s, 2, 0x83, 10240, 20480, 0);       /* overlaps slot 1 */

    struct part_table t;
    eq(part_scan(disk_read, d, d->sectors, &t), 2, "zero-length entry ignored");
    eq(t.overlaps, 1, "one overlapping pair reported");
    disk_free(d);
}

static void t_gpt_good(void)
{
    printf("GPT, well formed\n");
    struct disk *d = disk_new(DISK_SECTORS, 4096);
    protective_mbr(d);
    struct gptspec g = { 1, 2, 128, 128, 2,
                         { 2048, 43008 }, { 42999, 100000 },
                         { "LOGITOS", "data" } };
    gpt_build(d, &g);

    struct part_table t;
    eq(part_scan(disk_read, d, d->sectors, &t), 2, "two GPT partitions");
    eq(t.scheme, PART_GPT, "scheme GPT");
    eq(t.protective, 1, "protective MBR noticed");
    eq(t.backup_used, 0, "primary header used");
    eq((long long)t.e[0].start, 2048, "p1 start");
    eq((long long)t.e[0].count, 42999 - 2048 + 1, "p1 count: last_lba is inclusive");
    ok(strcmp(t.e[0].name, "LOGITOS") == 0, "p1 name decoded from UTF-16");
    ok(strcmp(t.e[1].name, "data") == 0, "p2 name decoded");
    ok(memcmp(t.e[0].type_guid, TYPE_LINUX, 16) == 0, "type GUID carried through");

    char guid[37];
    part_guid_str(t.e[0].type_guid, guid);
    ok(strcmp(guid, "0FC63DAF-8483-4772-8E79-3D69D8477DE4") == 0, "GUID printed mixed-endian");
    disk_free(d);
}

static void t_gpt_bad_header_crc(void)
{
    printf("GPT with a corrupted header CRC\n");
    struct disk *d = disk_new(DISK_SECTORS, 4096);
    protective_mbr(d);
    struct gptspec g = { 1, 2, 128, 128, 1, { 2048 }, { 42999 }, { "LOGITOS" } };
    gpt_build(d, &g);

    /* Flip one bit in a field the CRC covers but the parser would otherwise
     * happily believe: the first usable LBA. Without a header CRC check this
     * disk parses exactly as the good one did. */
    sec(d, 1)[40] ^= 0x01;

    struct part_table t;
    eq(part_scan(disk_read, d, d->sectors, &t), 0, "bad header CRC yields nothing");
    eq(t.count, 0, "no partitions leak through");
    eq(t.protective, 1, "still reports the protective MBR");
    disk_free(d);
}

static void t_gpt_bad_entry_crc(void)
{
    printf("GPT with a corrupted entry-array CRC\n");
    struct disk *d = disk_new(DISK_SECTORS, 4096);
    protective_mbr(d);
    struct gptspec g = { 1, 2, 128, 128, 2,
                         { 2048, 43008 }, { 42999, 100000 }, { "one", "two" } };
    gpt_build(d, &g);

    /* Corrupt the SECOND entry. The first is still perfectly well formed, so a
     * parser that checksums lazily -- or that returns what it parsed before the
     * check failed -- hands back one partition and looks like it worked. The
     * assertion is that it hands back none. */
    sec(d, 2)[128 + 32] ^= 0x08;

    struct part_table t;
    eq(part_scan(disk_read, d, d->sectors, &t), 0, "bad entry CRC yields nothing");
    eq(t.count, 0, "not even the undamaged first entry");
    disk_free(d);
}

static void t_gpt_backup_header(void)
{
    printf("GPT falling back to the backup header\n");
    uint64_t sectors = 4096;
    struct disk *d = disk_new(sectors, sectors);
    protective_mbr(d);

    /* Backup header at the last LBA, sharing the primary's entry array (which
     * is what makes this test about the HEADER and nothing else). */
    struct gptspec g = { sectors - 1, 2, 128, 128, 1, { 2048 }, { 3000 }, { "recovered" } };
    gpt_build(d, &g);

    /* Primary header: present, signed, and wrong. */
    memcpy(sec(d, 1), "EFI PART", 8);
    memset(sec(d, 1) + 8, 0x5A, 84);

    struct part_table t;
    eq(part_scan(disk_read, d, d->sectors, &t), 1, "backup header recovers the table");
    eq(t.backup_used, 1, "backup use is reported, not silent");
    ok(strcmp(t.e[0].name, "recovered") == 0, "the recovered entry is the right one");
    disk_free(d);
}

static void t_gpt_absurd_geometry(void)
{
    printf("GPT with absurd header fields\n");
    struct part_table t;

    /* num_entries * entry_size larger than the disk. A parser that trusts these
     * reads (or allocates) gigabytes off a 2 MiB disk. */
    {
        struct disk *d = disk_new(4096, 4096);
        protective_mbr(d);
        struct gptspec g = { 1, 2, 128, 128, 1, { 2048 }, { 3000 }, { "x" } };
        gpt_build(d, &g);
        uint8_t *h = sec(d, 1);
        wr32(h + 80, 1000000);                  /* num_entries */
        wr32(h + 16, 0); wr32(h + 16, crc32(h, 92));   /* keep the header CRC valid */
        eq(part_scan(disk_read, d, d->sectors, &t), 0, "absurd entry count rejected");
        disk_free(d);
    }

    /* entry_size smaller than a GPT entry. */
    {
        struct disk *d = disk_new(4096, 4096);
        protective_mbr(d);
        struct gptspec g = { 1, 2, 128, 128, 1, { 2048 }, { 3000 }, { "x" } };
        gpt_build(d, &g);
        uint8_t *h = sec(d, 1);
        wr32(h + 84, 64);
        wr32(h + 16, 0); wr32(h + 16, crc32(h, 92));
        eq(part_scan(disk_read, d, d->sectors, &t), 0, "undersized entry_size rejected");
        disk_free(d);
    }

    /* Entry array pointing past the end of the device. */
    {
        struct disk *d = disk_new(4096, 4096);
        protective_mbr(d);
        struct gptspec g = { 1, 2, 128, 128, 1, { 2048 }, { 3000 }, { "x" } };
        gpt_build(d, &g);
        uint8_t *h = sec(d, 1);
        wr64(h + 72, 999999);
        wr32(h + 16, 0); wr32(h + 16, crc32(h, 92));
        eq(part_scan(disk_read, d, d->sectors, &t), 0, "entry array off the end rejected");
        disk_free(d);
    }

    /* An entry whose last_lba precedes its first_lba, and one that runs off the
     * device. Both are inside a CRC-valid array, so only the range checks catch
     * them. */
    {
        struct disk *d = disk_new(4096, 4096);
        protective_mbr(d);
        struct gptspec g = { 1, 2, 128, 128, 3,
                             { 2048, 3000, 3500 }, { 2500, 100, 999999 },
                             { "good", "inverted", "overrun" } };
        gpt_build(d, &g);
        eq(part_scan(disk_read, d, d->sectors, &t), 1, "only the sane entry survives");
        eq(t.skipped, 2, "the other two are counted as rejected");
        ok(strcmp(t.e[0].name, "good") == 0, "survivor identified");
        disk_free(d);
    }
}

static void t_gpt_straddling_entries(void)
{
    printf("GPT entry_size that does not divide the sector\n");
    /* entry_size 256 divides 512; entry_size 384 does not, so entries straddle
     * sector boundaries and the streaming reader's assembly window is what makes
     * them parse. A partitioner is allowed to write this. */
    struct disk *d = disk_new(DISK_SECTORS, 4096);
    protective_mbr(d);
    struct gptspec g = { 1, 2, 8, 384, 3,
                         { 2048, 20000, 40000 }, { 19999, 39999, 59999 },
                         { "a", "b", "c" } };
    gpt_build(d, &g);

    struct part_table t;
    eq(part_scan(disk_read, d, d->sectors, &t), 3, "384-byte entries parse across sectors");
    eq((long long)t.e[2].start, 40000, "third entry, which straddles, is right");
    disk_free(d);
}

static void t_read_failure(void)
{
    printf("I/O errors\n");
    struct part_table t;

    /* A failing sector 0 must mean "no table", never "a table of zeroes". */
    {
        struct disk *d = disk_new(DISK_SECTORS, 4096);
        protective_mbr(d);
        d->fail_lba = 0;
        eq(part_scan(disk_read, d, d->sectors, &t), 0, "unreadable sector 0 -> no table");
        eq(t.scheme, PART_NONE, "and no scheme claimed");
        disk_free(d);
    }

    /* A GPT whose entry array cannot be read must not fall through to a partial
     * table. */
    {
        struct disk *d = disk_new(DISK_SECTORS, 4096);
        protective_mbr(d);
        struct gptspec g = { 1, 2, 128, 128, 1, { 2048 }, { 3000 }, { "x" } };
        gpt_build(d, &g);
        d->fail_lba = 2;
        eq(part_scan(disk_read, d, d->sectors, &t), 0, "unreadable entry array -> nothing");
        disk_free(d);
    }
}

static void t_tiny_device(void)
{
    printf("degenerate devices\n");
    struct part_table t;
    struct disk *d = disk_new(1, 1);
    eq(part_scan(disk_read, d, 0, &t), 0, "zero-sector device");
    eq(part_scan(disk_read, d, 1, &t), 0, "one-sector device");
    disk_free(d);
    eq(part_scan(NULL, NULL, 100, &t), -1, "NULL reader rejected");
}

static void t_crc32(void)
{
    printf("crc32\n");
    /* The published check value for this polynomial. If this is wrong every GPT
     * test above is testing the parser against its own mistake. */
    eq(crc32("123456789", 9), 0xCBF43926u, "CRC-32 check value");
    eq(crc32("", 0), 0u, "empty input");

    /* Streaming in pieces must equal the one-shot -- the property the GPT entry
     * array actually relies on. */
    const char *s = "the quick brown fox jumps over the lazy dog, twice, at length";
    uint32_t c = CRC32_INIT;
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i += 7)
        c = crc32_update(c, s + i, (n - i) < 7 ? (n - i) : 7);
    eq(crc32_final(c), crc32(s, n), "streaming equals one-shot");
}

int main(void)
{
    t_crc32();
    t_no_table();
    t_mbr_primary();
    t_mbr_extended();
    t_mbr_chain_loops();
    t_mbr_out_of_range();
    t_mbr_zero_and_overlap();
    t_gpt_good();
    t_gpt_bad_header_crc();
    t_gpt_bad_entry_crc();
    t_gpt_backup_header();
    t_gpt_absurd_geometry();
    t_gpt_straddling_entries();
    t_read_failure();
    t_tiny_device();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
