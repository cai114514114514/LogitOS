/* tests/unit/legacy_test.c -- manifest-driven differential harness for the
 * four legacy codecs (Cinepak, MS Video 1, RPZA, QTRLE) against tools/
 * genlegacy.py's corpus.
 *
 * legacy.h's own header comment states the bar this file holds every case
 * to: "All four are deterministic BLOCK-REPLACEMENT schemes ... so 'matches
 * ffmpeg's decoder byte-for-byte' is exactly the right bar." There is no
 * tolerance anywhere in this file -- a mismatch of one byte is a mismatch.
 *
 * A case is either a POSITIVE case (a .manifest naming a `container` + `ref`
 * pair, one or more frames sliced out of the container at the offsets
 * genlegacy.py recorded and fed through the matching legacy_*_decode call
 * IN ORDER, with the SAME context across frames -- every one of these four
 * formats is stateful by construction, see legacy.h) or a NEGATIVE-CONTROL
 * case (a .manifest naming `corrupt`: one standalone frame chunk that must
 * be refused with LEGACY_ERR_CORRUPT on a FRESH context, never accepted and
 * never crash).
 *
 * Two run modes on one binary (the shape tests/vp9.mk's vp9_test already
 * uses for the same reason):
 *   legacy_test <corpus_dir> <case>           exact gate: exit 1 on any
 *                                              mismatch or wrong outcome.
 *   legacy_test --diff <corpus_dir> <case>     always exits 0; prints wrong
 *                                              byte totals per frame, the
 *                                              first mismatch, and the
 *                                              worst frame -- the number
 *                                              this tree bisects with.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "legacy.h"

/* ---------------------------------------------------------- manifest I/O */
#define MAXKV 48
typedef struct { char key[32]; char val[256]; } kv_t;
typedef struct { kv_t items[MAXKV]; int n; } manifest_t;

static int manifest_load(const char *path, manifest_t *m)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    m->n = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *val = eq + 1;
        size_t vl = strlen(val);
        while (vl && (val[vl - 1] == '\n' || val[vl - 1] == '\r')) val[--vl] = 0;
        if (m->n >= MAXKV) break;
        snprintf(m->items[m->n].key, sizeof m->items[m->n].key, "%s", line);
        snprintf(m->items[m->n].val, sizeof m->items[m->n].val, "%s", val);
        m->n++;
    }
    fclose(f);
    return 0;
}

static const char *mget(const manifest_t *m, const char *key)
{
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->items[i].key, key)) return m->items[i].val;
    return NULL;
}

static int mgeti(const manifest_t *m, const char *key, int def)
{
    const char *v = mget(m, key);
    return v ? atoi(v) : def;
}

static uint8_t *read_whole(const char *path, long *outlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)(n > 0 ? n : 1));
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *outlen = n;
    return buf;
}

static char *joinpath(const char *dir, const char *name)
{
    size_t n = strlen(dir) + 1 + strlen(name) + 1;
    char *p = (char *)malloc(n);
    snprintf(p, n, "%s/%s", dir, name);
    return p;
}

/* -------------------------------------------------------- per-frame diff */
/* Compares `got` (N bytes, this frame's decoded output in the layout
 * legacy.h's OUTPUT CONVENTION defines) against `ref + *ref_off`, advancing
 * *ref_off by N (ref_kind "plain") or by N + 1024 (ref_kind "pal8trailer",
 * skipping ffmpeg's AVPALETTE trailer -- genlegacy.py's own header explains
 * why the palette bytes are never part of this decoder's contract). Returns
 * the number of mismatched bytes and records the first one. */
static long compare_frame(const uint8_t *got, long n, const uint8_t *ref, long ref_len,
                          long *ref_off, int pal8trailer, long *first_bad_idx)
{
    long mism = 0;
    *first_bad_idx = -1;
    if (*ref_off + n > ref_len) {
        fprintf(stderr, "  reference too short: need %ld more bytes at offset %ld, have %ld\n",
                n, *ref_off, ref_len);
        *ref_off += n + (pal8trailer ? 1024 : 0);
        return n; /* whole frame counts as wrong -- nothing to compare against */
    }
    const uint8_t *r = ref + *ref_off;
    for (long i = 0; i < n; i++) {
        if (got[i] != r[i]) {
            if (*first_bad_idx < 0) *first_bad_idx = i;
            mism++;
        }
    }
    *ref_off += n + (pal8trailer ? 1024 : 0);
    return mism;
}

/* ------------------------------------------------------------- extractors
 * Each pulls exactly what legacy.h's OUTPUT CONVENTION promises out of a
 * live decoder context into a flat buffer the same shape the reference
 * carries, cropping the codec's own working stride down to the true
 * (width, height) where the two differ (Cinepak's rw/rh block-aligned
 * grid). */
static long extract_cinepak(legacy_cinepak_ctx *c, uint8_t *out)
{
    int stride = c->rw * c->bpp;
    long n = 0;
    for (int y = 0; y < c->height; y++) {
        memcpy(out + n, c->frame + (long)y * stride, (size_t)(c->width * c->bpp));
        n += c->width * c->bpp;
    }
    return n;
}

static long extract_plain(const uint8_t *frame, long bytes)
{
    (void)frame;
    return bytes; /* caller memcpy's directly; kept for symmetry/documentation */
}

/* --------------------------------------------------------------- runners */
typedef struct {
    long total_bytes, mism_bytes;
    int worst_frame; long worst_frame_mism;
    long first_bad_frame, first_bad_idx;
    int decode_error_frame; /* -1 if none */
} diffres_t;

static void diffres_init(diffres_t *d)
{
    memset(d, 0, sizeof(*d));
    d->first_bad_frame = -1;
    d->first_bad_idx = -1;
    d->decode_error_frame = -1;
}

static void diffres_note(diffres_t *d, int frame, long n, long mism, long first_bad_idx)
{
    d->total_bytes += n;
    d->mism_bytes += mism;
    if (mism > d->worst_frame_mism) { d->worst_frame_mism = mism; d->worst_frame = frame; }
    if (mism > 0 && d->first_bad_frame < 0) { d->first_bad_frame = frame; d->first_bad_idx = first_bad_idx; }
}

static int run_positive(const manifest_t *m, const char *dir, diffres_t *out)
{
    const char *codec = mget(m, "codec");
    const char *variant = mget(m, "variant");
    int width = mgeti(m, "width", 0), height = mgeti(m, "height", 0);
    int frames = mgeti(m, "frames", 0);
    int pal8trailer = mget(m, "ref_kind") && !strcmp(mget(m, "ref_kind"), "pal8trailer");

    char *cpath = joinpath(dir, mget(m, "container"));
    char *rpath = joinpath(dir, mget(m, "ref"));
    long clen, rlen;
    uint8_t *cbuf = read_whole(cpath, &clen);
    uint8_t *rbuf = read_whole(rpath, &rlen);
    if (!cbuf || !rbuf) {
        fprintf(stderr, "cannot read %s or %s\n", cpath, rpath);
        free(cpath); free(rpath); free(cbuf); free(rbuf);
        return -1;
    }

    diffres_init(out);
    long ref_off = 0;

    legacy_cinepak_ctx cin; legacy_msvideo1_ctx m1; legacy_rpza_ctx rp; legacy_qtrle_ctx qt;
    int rc = 0;
    if (!strcmp(codec, "cinepak"))
        rc = legacy_cinepak_open(&cin, width, height, !strcmp(variant, "gray"));
    else if (!strcmp(codec, "msvideo1"))
        rc = legacy_msvideo1_open(&m1, width, height, !strcmp(variant, "8bit"));
    else if (!strcmp(codec, "rpza"))
        rc = legacy_rpza_open(&rp, width, height);
    else if (!strcmp(codec, "qtrle"))
        rc = legacy_qtrle_open(&qt, width, height, mgeti(m, "depth", 8));
    else { fprintf(stderr, "unknown codec %s\n", codec); return -1; }
    if (rc != LEGACY_OK) { fprintf(stderr, "open failed rc=%d\n", rc); return -1; }

    uint8_t *scratch = (uint8_t *)malloc((size_t)width * height * 4 + 4096);

    for (int i = 0; i < frames; i++) {
        char key[32];
        snprintf(key, sizeof key, "frame%d_pos", i);
        long pos = mgeti(m, key, -1);
        snprintf(key, sizeof key, "frame%d_size", i);
        long size = mgeti(m, key, -1);
        if (pos < 0 || size < 0 || pos + size > clen) {
            fprintf(stderr, "bad frame%d offsets\n", i);
            return -1;
        }
        int drc;
        long n = 0;
        if (!strcmp(codec, "cinepak")) {
            drc = legacy_cinepak_decode(&cin, cbuf + pos, (int)size);
            if (drc == LEGACY_OK) n = extract_cinepak(&cin, scratch);
        } else if (!strcmp(codec, "msvideo1")) {
            drc = legacy_msvideo1_decode(&m1, cbuf + pos, (int)size);
            if (drc == LEGACY_OK) {
                long bytes = (long)width * height * (m1.mode_8bit ? 1 : 2);
                memcpy(scratch, m1.frame, (size_t)bytes);
                n = extract_plain(m1.frame, bytes);
            }
        } else if (!strcmp(codec, "rpza")) {
            drc = legacy_rpza_decode(&rp, cbuf + pos, (int)size);
            if (drc == LEGACY_OK) {
                long bytes = (long)width * height * 2;
                memcpy(scratch, rp.frame, (size_t)bytes);
                n = extract_plain((const uint8_t *)rp.frame, bytes);
            }
        } else {
            drc = legacy_qtrle_decode(&qt, cbuf + pos, (int)size);
            if (drc == LEGACY_OK) {
                long bytes = (long)width * height * qt.bpp_out;
                memcpy(scratch, qt.frame, (size_t)bytes);
                n = extract_plain(qt.frame, bytes);
            }
        }
        if (drc != LEGACY_OK) {
            fprintf(stderr, "  frame %d: decode returned %d (expected LEGACY_OK)\n", i, drc);
            if (out->decode_error_frame < 0) out->decode_error_frame = i;
            /* Count the whole frame as wrong so the byte total still means
             * something and the case still fails the exact gate. */
            long expect = pal8trailer ? (long)width * height : ((long)width * height *
                          (!strcmp(codec, "cinepak") ? cin.bpp :
                           !strcmp(codec, "msvideo1") ? (m1.mode_8bit ? 1 : 2) :
                           !strcmp(codec, "rpza") ? 2 : qt.bpp_out));
            diffres_note(out, i, expect, expect, 0);
            ref_off += expect + (pal8trailer ? 1024 : 0);
            continue;
        }
        long first_bad;
        long mism = compare_frame(scratch, n, rbuf, rlen, &ref_off, pal8trailer, &first_bad);
        diffres_note(out, i, n, mism, first_bad);
    }

    if (!strcmp(codec, "cinepak")) legacy_cinepak_close(&cin);
    else if (!strcmp(codec, "msvideo1")) legacy_msvideo1_close(&m1);
    else if (!strcmp(codec, "rpza")) legacy_rpza_close(&rp);
    else legacy_qtrle_close(&qt);

    free(scratch); free(cbuf); free(rbuf); free(cpath); free(rpath);
    return 0;
}

static int run_negctl(const manifest_t *m, const char *dir)
{
    const char *codec = mget(m, "codec");
    const char *variant = mget(m, "variant");
    int width = mgeti(m, "width", 0), height = mgeti(m, "height", 0);
    char *ppath = joinpath(dir, mget(m, "corrupt"));
    long plen;
    uint8_t *pbuf = read_whole(ppath, &plen);
    if (!pbuf) { fprintf(stderr, "cannot read %s\n", ppath); free(ppath); return -1; }

    int rc, drc;
    if (!strcmp(codec, "cinepak")) {
        legacy_cinepak_ctx c;
        rc = legacy_cinepak_open(&c, width, height, !strcmp(variant, "gray"));
        drc = legacy_cinepak_decode(&c, pbuf, (int)plen);
        legacy_cinepak_close(&c);
    } else if (!strcmp(codec, "msvideo1")) {
        legacy_msvideo1_ctx c;
        rc = legacy_msvideo1_open(&c, width, height, !strcmp(variant, "8bit"));
        drc = legacy_msvideo1_decode(&c, pbuf, (int)plen);
        legacy_msvideo1_close(&c);
    } else if (!strcmp(codec, "rpza")) {
        legacy_rpza_ctx c;
        rc = legacy_rpza_open(&c, width, height);
        drc = legacy_rpza_decode(&c, pbuf, (int)plen);
        legacy_rpza_close(&c);
    } else {
        legacy_qtrle_ctx c;
        rc = legacy_qtrle_open(&c, width, height, mgeti(m, "depth", 8));
        drc = legacy_qtrle_decode(&c, pbuf, (int)plen);
        legacy_qtrle_close(&c);
    }
    free(pbuf); free(ppath);
    if (rc != LEGACY_OK) { fprintf(stderr, "open failed rc=%d\n", rc); return -1; }
    if (drc != LEGACY_ERR_CORRUPT) {
        fprintf(stderr, "  expected LEGACY_ERR_CORRUPT, got %d\n", drc);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int diff_mode = 0;
    int argi = 1;
    if (argc > argi && !strcmp(argv[argi], "--diff")) { diff_mode = 1; argi++; }
    if (argc != argi + 2) {
        fprintf(stderr, "usage: legacy_test [--diff] <corpus_dir> <case>\n");
        return 2;
    }
    const char *dir = argv[argi], *case_name = argv[argi + 1];

    char *mpath = joinpath(dir, case_name);
    size_t mp_len = strlen(mpath) + 20;
    char *mfile = (char *)malloc(mp_len);
    snprintf(mfile, mp_len, "%s.manifest", mpath);
    manifest_t m;
    if (manifest_load(mfile, &m) != 0) {
        fprintf(stderr, "cannot read %s\n", mfile);
        return 2;
    }

    if (mget(&m, "corrupt")) {
        int r = run_negctl(&m, dir);
        if (diff_mode) {
            printf("%-22s %s\n", case_name, r == 0 ? "CORRUPT-REFUSED-OK" : "CORRUPT-NOT-REFUSED");
            return 0;
        }
        printf("%-22s %s\n", case_name, r == 0 ? "CORRUPT-OK" : "CORRUPT-FAIL");
        return r == 0 ? 0 : 1;
    }

    diffres_t d;
    if (run_positive(&m, dir, &d) != 0) {
        printf("%-22s ERROR\n", case_name);
        return diff_mode ? 0 : 2;
    }

    if (diff_mode) {
        printf("%-22s bytes=%ld/%ld worst_frame=%d(%ld) first_bad=frame%ld@%ld decode_err_frame=%d\n",
               case_name, d.mism_bytes, d.total_bytes, d.worst_frame, d.worst_frame_mism,
               d.first_bad_frame, d.first_bad_idx, d.decode_error_frame);
        return 0;
    }

    if (d.mism_bytes == 0 && d.decode_error_frame < 0) {
        printf("%-22s OK (%ld bytes, %d frames)\n", case_name, d.total_bytes, mgeti(&m, "frames", 0));
        return 0;
    }
    printf("%-22s MISMATCH bytes=%ld/%ld first_bad=frame%ld@%ld\n",
           case_name, d.mism_bytes, d.total_bytes, d.first_bad_frame, d.first_bad_idx);
    return 1;
}
