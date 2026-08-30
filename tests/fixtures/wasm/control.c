/* Branch-heavy: a dense switch becomes br_table, and loops become
 * loop/block/br_if nests -- the control constructs whose label typing is the
 * part of the validator with the most ways to be quietly wrong. */
typedef unsigned u32;
u32 dense(u32 x) {
	switch (x) {
	case 0: return 7; case 1: return 9;  case 2: return 11; case 3: return 13;
	case 4: return 17; case 5: return 19; case 6: return 23; case 7: return 29;
	default: return 0;
	}
}
u32 sparse(u32 x) { switch (x) { case 1: return 1; case 1000: return 2; case 100000: return 3; } return 4; }
u32 nested(u32 a, u32 b) {
	u32 s = 0;
	for (u32 i = 0; i < a; i++) { for (u32 j = 0; j < b; j++) { if (i == j) continue; if (i + j > 40) return s; s += i ^ j; } }
	return s;
}
u32 unreachable_tail(u32 x) { if (x) return 1; return 2; }
