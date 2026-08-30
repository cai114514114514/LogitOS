/* Every load and store width, plus a data segment and a global. */
typedef unsigned char u8; typedef unsigned short u16;
typedef unsigned u32; typedef unsigned long long u64;
static u8 tab[256] = { 1, 2, 3, 4, 5 };
static u32 counter;
u32 widths(u32 i) {
	u8 *b = tab + (i & 63);
	u16 *h = (u16 *)(tab + ((i * 2) & 62));
	u32 *w = (u32 *)(tab + ((i * 4) & 60));
	u64 *q = (u64 *)(tab + ((i * 8) & 56));
	*b = (u8)i; *h = (u16)i; *w = i; *q = i;
	return (u32)*b + *h + *w + (u32)*q;
}
u32 bump(void) { return ++counter; }
double dstore(double d, u32 i) { double *p = (double *)(tab + ((i * 8) & 56)); *p = d; return *p; }
