/* tests/unit/display_rrect_test.c -- ol_display_round_rect / blend_round_rect /
 * round_rect_vgrad: coverage only in the corner squares, span fills elsewhere.
 *
 * ref_* below are verbatim copies of the former per-pixel loops (corner_cov for
 * every pixel of the rect). The production output must match them bit for bit
 * for every rect, radius, clip and colour, including clipped corners and
 * radii past h/2 and past GFX_MASK_MAX (the NULL-mask staircase fallback).
 *
 *   display_rrect_test            identity check (FAIL identity ... on mismatch)
 *   display_rrect_test budget     800x600 r=12 must be >= 2x faster than ref_*
 * -DOL_RRECT_FAST_NEGCTL (compiled into openlogit_display.c) writes corners
 * solid, which the identity check must catch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "openlogit_display.h"
#include "display_pixels.h"
#include "gfx_mask.h"
#include "bench_budget.h"

/* Deterministic synthetic corner tile with plenty of partial coverage values;
 * NULL past GFX_MASK_MAX exactly like the real cache's refusal. */
const unsigned char *gfx_mask_corner(int kind, int w, int h, int param)
{
	static unsigned char tile[GFX_MASK_MAX + 1][GFX_MASK_MAX * GFX_MASK_MAX];
	static int built[GFX_MASK_MAX + 1];
	(void)kind; (void)h; (void)param;
	if (w <= 0 || w > GFX_MASK_MAX) return NULL;
	if (!built[w]) {
		for (int cy = 0; cy < w; cy++)
			for (int cx = 0; cx < w; cx++) {
				unsigned v = ((unsigned)cy * 37u + (unsigned)cx * 91u + (unsigned)w * 13u) % 256u;
				if (v % 7 == 0) v = 0;
				if (v % 11 == 0) v = 255;
				tile[w][cy * w + cx] = (unsigned char)v;
			}
		built[w] = 1;
	}
	return tile[w];
}

int gfx_shadow_falloff(long d256, long blur256) { (void)d256; (void)blur256; return 0; }

static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }

/* ---- verbatim former implementations --------------------------------- */
static void ref_round_rect(struct ol_display *ctx, int x, int y, int w, int h, int radius, uint32_t color)
{
	int i0, j0, i1, j1;
	if (!clip_ij(ctx, x, y, w, h, &i0, &j0, &i1, &j1)) return;
	int rad = radius;
	const unsigned char *m = corner_mask_for(w, h, &rad);
	for (int j = j0; j < j1; j++)
		for (int i = i0; i < i1; i++)
			blend_cov(ctx, x + i, y + j, color, corner_cov(i, j, w, h, rad, m));
}

static void ref_blend_round_rect(struct ol_display *ctx, int x, int y, int w, int h, int radius,
				 uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	int i0, j0, i1, j1;
	if (!clip_ij(ctx, x, y, w, h, &i0, &j0, &i1, &j1)) return;
	int rad = radius;
	const unsigned char *m = corner_mask_for(w, h, &rad);
	for (int j = j0; j < j1; j++) {
		for (int i = i0; i < i1; i++) {
			int cov = corner_cov(i, j, w, h, rad, m);
			if (cov <= 0) continue;
			blend_cov(ctx, x + i, y + j, ol_display_rgb(ctx, r, g, b), a * cov / 255);
		}
	}
}

static void ref_round_rect_vgrad(struct ol_display *ctx, int x, int y, int w, int h, int radius, uint32_t top, uint32_t bottom)
{
	int i0, j0, i1, j1;
	if (!clip_ij(ctx, x, y, w, h, &i0, &j0, &i1, &j1)) return;
	int rad = radius;
	const unsigned char *m = corner_mask_for(w, h, &rad);
	for (int j = j0; j < j1; j++) {
		uint32_t c = color_lerp(ctx, top, bottom, j, h > 1 ? h - 1 : 1);
		for (int i = i0; i < i1; i++)
			blend_cov(ctx, x + i, y + j, c, corner_cov(i, j, w, h, rad, m));
	}
}

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static unsigned rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
	return (unsigned)(rs >> 32);
}
static int rr(int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }

#define SW 256
#define SH 200

static void bind(struct ol_display *d, uint32_t *px, int w, int h, unsigned rp, unsigned gp, unsigned bp)
{
	struct ol_pixel_target t;
	memset(d, 0, sizeof *d);
	memset(&t, 0, sizeof t);
	t.px = px; t.w = w; t.h = h;
	d->allocate = t_alloc; d->release = t_free;
	ol_display_bind(d, &t, rp, gp, bp);
}

static int run_identity(void)
{
	static uint32_t a[SW * SH], b[SW * SH], bg[SW * SH];
	int fails = 0, big = 0, clipped = 0;
	for (int it = 0; it < 2000; it++) {
		unsigned rp = (it & 1) ? 16 : 0, bp = (it & 1) ? 0 : 16;
		for (int k = 0; k < SW * SH; k++) bg[k] = rnd() & 0xFFFFFF;
		memcpy(a, bg, sizeof a); memcpy(b, bg, sizeof b);
		struct ol_display da, db;
		bind(&da, a, SW, SH, rp, 8, bp);
		bind(&db, b, SW, SH, rp, 8, bp);
		int x = rr(-40, SW - 5), y = rr(-40, SH - 5);
		int w = (it % 5 == 0) ? rr(150, 240) : rr(1, 100), h = (it % 5 == 0) ? rr(150, 190) : rr(1, 100);
		int rad = (it % 4 == 0) ? rr(60, 130) : (it % 7 == 0 ? rr(-3, 1) : rr(0, 40));
		if (rad > 72 && w >= 150 && h >= 150) big++;
		if (it % 3) {
			int cx0 = rr(-10, SW), cy0 = rr(-10, SH);
			int cx1 = cx0 + rr(-5, SW), cy1 = cy0 + rr(-5, SH);
			da.target.clip_on = db.target.clip_on = 1;
			da.target.clx0 = db.target.clx0 = cx0; da.target.cly0 = db.target.cly0 = cy0;
			da.target.clx1 = db.target.clx1 = cx1; da.target.cly1 = db.target.cly1 = cy1;
			clipped++;
		}
		uint32_t col = rnd() & 0xFFFFFF, col2 = rnd() & 0xFFFFFF;
		uint8_t al = (it % 6 == 0) ? (rnd() & 1 ? 0 : 255) : (uint8_t)rnd();
		uint8_t cr = (uint8_t)rnd(), cg = (uint8_t)rnd(), cb = (uint8_t)rnd();

		ref_round_rect(&da, x, y, w, h, rad, col);
		ol_display_round_rect(&db, x, y, w, h, rad, col);
		if (memcmp(a, b, sizeof a)) {
			printf("FAIL identity round_rect it=%d x=%d y=%d w=%d h=%d r=%d\n", it, x, y, w, h, rad);
			fails++;
		}
		memcpy(a, bg, sizeof a); memcpy(b, bg, sizeof b);
		ref_blend_round_rect(&da, x, y, w, h, rad, cr, cg, cb, al);
		ol_display_blend_round_rect(&db, x, y, w, h, rad, cr, cg, cb, al);
		if (memcmp(a, b, sizeof a)) {
			printf("FAIL identity blend_round_rect it=%d x=%d y=%d w=%d h=%d r=%d a=%d\n", it, x, y, w, h, rad, al);
			fails++;
		}
		memcpy(a, bg, sizeof a); memcpy(b, bg, sizeof b);
		ref_round_rect_vgrad(&da, x, y, w, h, rad, col, col2);
		ol_display_round_rect_vgrad(&db, x, y, w, h, rad, col, col2);
		if (memcmp(a, b, sizeof a)) {
			printf("FAIL identity round_rect_vgrad it=%d x=%d y=%d w=%d h=%d r=%d\n", it, x, y, w, h, rad);
			fails++;
		}
	}
	printf("identity: 2000 combos x3 ops, %d clipped, %d with r>72 on >=150px rects, %d failures\n",
	       clipped, big, fails);
	return fails != 0;
}

/* ---- budget ------------------------------------------------------------ */
static uint32_t *bpx;
static struct ol_display bd;
static void b_ref(void *p) { (void)p; ref_round_rect(&bd, 0, 0, 800, 600, 12, 0x336699); }
static void b_new(void *p) { (void)p; ol_display_round_rect(&bd, 0, 0, 800, 600, 12, 0x336699); }
static void bb_ref(void *p) { (void)p; ref_blend_round_rect(&bd, 0, 0, 800, 600, 12, 30, 90, 150, 128); }
static void bb_new(void *p) { (void)p; ol_display_blend_round_rect(&bd, 0, 0, 800, 600, 12, 30, 90, 150, 128); }

static int run_budget(void)
{
	bpx = calloc(800 * 600, 4);
	bind(&bd, bpx, 800, 600, 16, 8, 0);
	struct bb_stats r = bb_measure(b_ref, 0, 4), n = bb_measure(b_new, 0, 4);
	bb_ratio("round_rect 800x600 r=12 new/ref <= 0.5", n, r, 0.5);
	r = bb_measure(bb_ref, 0, 2); n = bb_measure(bb_new, 0, 2);
	bb_ratio("blend_round_rect 800x600 r=12 new/ref <= 0.5", n, r, 0.5);
	free(bpx);
	return bb_exit();
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "budget")) return run_budget();
	return run_identity();
}
