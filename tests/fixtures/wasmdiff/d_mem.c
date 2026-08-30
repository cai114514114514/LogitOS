/* Linear memory: every load and store width, both signednesses, at aligned
 * and deliberately unaligned addresses, plus a data segment.
 *
 * ALIGNMENT NEVER TRAPS IN WEBASSEMBLY -- the align immediate is a HINT, and
 * an implementation that honours it as a requirement refuses programs the
 * specification accepts.  So every access below is done at four consecutive
 * byte offsets, which puts a 4- and an 8-byte access on every residue, and the
 * host reads them with the same unaligned loads it always does.
 *
 * The array is EXPORTED and there is a writer, because a static array nothing
 * stores into is proved constant by clang and the whole fixture folds away.
 * That happened to tests/fixtures/wasm/indirect.c and it compiled, validated
 * and exercised nothing.
 */
#include "wdiff.h"

/* A non-zero initialiser makes clang emit a DATA SEGMENT, which is the only
 * way this corpus gets one, and the segment's bytes come back out through the
 * loads below -- so a data segment applied at the wrong offset is visible as a
 * wrong value rather than as nothing at all. */
unsigned char buf[512] = {
	0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x55, 0xAA,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x80, 0x00
};

static void fill(u32 seed)
{
	u32 i, x = seed | 1u;
	for (i = 24; i < 512; i++) {
		x ^= x << 13; x ^= x >> 17; x ^= x << 5;
		buf[i] = (unsigned char)x;
	}
}

static void loads(void)
{
	u32 o;
	for (o = 0; o < 64; o++) {
		signed char *b = (signed char *)(buf + o);
		unsigned char *ub = buf + o;
		short h; unsigned short uh;
		int w; unsigned uw;
		long long q;

		out_i32((u32)(s32)*b);            /* i32.load8_s  */
		out_i32((u32)*ub);                /* i32.load8_u  */
		out_i64((u64)(s64)*b);            /* i64.load8_s  */
		out_i64((u64)*ub);                /* i64.load8_u  */

		wmemcpy(&h, buf + o, 2);  out_i32((u32)(s32)h);   /* i32.load16_s */
		wmemcpy(&uh, buf + o, 2); out_i32((u32)uh);       /* i32.load16_u */
		wmemcpy(&h, buf + o, 2);  out_i64((u64)(s64)h);   /* i64.load16_s */
		wmemcpy(&uh, buf + o, 2); out_i64((u64)uh);       /* i64.load16_u */

		wmemcpy(&w, buf + o, 4);  out_i32((u32)w);        /* i32.load      */
		wmemcpy(&w, buf + o, 4);  out_i64((u64)(s64)w);   /* i64.load32_s  */
		wmemcpy(&uw, buf + o, 4); out_i64((u64)uw);       /* i64.load32_u  */
		wmemcpy(&q, buf + o, 8);  out_i64((u64)q);        /* i64.load      */

		{   /* the float widths, read back as bits */
			float f; double d;
			wmemcpy(&f, buf + o, 4); out_f32(f);
			wmemcpy(&d, buf + o, 8); out_f64(d);
		}
	}
}

static void stores(void)
{
	u32 o;
	for (o = 0; o < 32; o++) {
		u32 v = 0x89ABCDEFu ^ (o * 0x01010101u);
		u64 q = 0x0123456789ABCDEFull ^ ((u64)o << 40);
		unsigned char *p = buf + 128 + o;

		p[0] = (unsigned char)v;                          /* i32.store8  */
		wmemcpy(p + 1, &v, 2);                            /* i32.store16 */
		wmemcpy(p + 3, &v, 4);                            /* i32.store   */
		wmemcpy(p + 7, &q, 8);                            /* i64.store   */
		{
			float f; double d;
			wmemcpy(&f, &v, 4); wmemcpy(p + 15, &f, 4);   /* f32.store */
			wmemcpy(&d, &q, 8); wmemcpy(p + 19, &d, 8);   /* f64.store */
		}
	}
	{   /* read the whole region back so a mis-sized store is a wrong value */
		u32 i;
		for (i = 128; i < 200; i++) out_i32(buf[i]);
	}
}

/* A checksum over the whole array: one loop, several thousand memory
 * accesses, and a single value at the end that no partial correctness can
 * fake. */
static void checksum(void)
{
	u32 i, h = 2166136261u;
	u64 g = 1469598103934665603ull;
	for (i = 0; i < 512; i++) {
		h = (h ^ buf[i]) * 16777619u;
		g = (g ^ buf[i]) * 1099511628211ull;
	}
	out_i32(h);
	out_i64(g);
}

void wbody(void)
{
	loads();
	stores();
	checksum();
	fill(0x1234567u);
	loads();
	stores();
	checksum();
}
