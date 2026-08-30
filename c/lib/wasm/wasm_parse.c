/* WebAssembly MVP binary decoder -- structure only.  See wasm.h for why this
 * exists at all, and wasm_int.h for why the opcode table lives next door.
 *
 * THE LEB128 RULE IS NOT "REJECT OVERLONG ENCODINGS", AND THAT IS WORTH
 * WRITING DOWN because it is the natural guess and it is wrong.  The spec
 * (5.2.2) permits a uN or sN to be PADDED up to ceil(N/7) bytes: 0 as a u32
 * may legally be written "\80\80\80\80\00".  What is malformed is (a) a byte
 * count beyond ceil(N/7) -- "integer representation too long" -- and (b) a
 * final byte carrying bits above N -- "integer too large".  binary-leb128.wast
 * asserts both directions, so an implementation that rejects all padding
 * fails 30-odd cases that a conformant one accepts.
 *
 * EVERY OFFSET IN A MODULE IS ATTACKER CONTROLLED.  Two consequences that are
 * structural rather than stylistic:
 *   - bounds are `off > n || size > n - off`, never `off + size > n`
 *     (c/kernel/module/modelf.c's rule, quoted in its own words there).
 *   - a vector's element count is checked against the BYTES REMAINING before
 *     anything is allocated for it, because every element of every vector in
 *     this format costs at least one byte.  Without that, `\xff\xff\xff\xff\x0f`
 *     as a count is a 4-billion-element allocation request from an eight-byte
 *     file.
 */

#include "wasm_int.h"

/* ---- errors ---------------------------------------------------------- */

const char *wasm_errstr(int e)
{
	switch (e) {
	case WASM_OK:              return "ok";
	case WASM_E_MAGIC:         return "magic header not detected";
	case WASM_E_VERSION:       return "unknown binary version";
	case WASM_E_END:           return "unexpected end";
	case WASM_E_LEB_LONG:      return "integer representation too long";
	case WASM_E_LEB_LARGE:     return "integer too large";
	case WASM_E_SECTION_ID:    return "malformed section id";
	case WASM_E_SECTION_ORDER: return "unexpected content after last section";
	case WASM_E_SECTION_SIZE:  return "section size mismatch";
	case WASM_E_UTF8:          return "malformed UTF-8 encoding";
	case WASM_E_FORM:          return "integer representation too long (bad form byte)";
	case WASM_E_VALTYPE:       return "malformed value type";
	case WASM_E_MUT:           return "malformed mutability";
	case WASM_E_FLAGS:         return "malformed limits flags";
	case WASM_E_IMPORT_KIND:   return "malformed import kind";
	case WASM_E_RESERVED:      return "zero byte expected";
	case WASM_E_OPCODE:        return "illegal opcode";
	case WASM_E_LENGTHS:       return "function and code section have inconsistent lengths";
	case WASM_E_LOCALS:        return "too many locals";
	case WASM_E_JUNK:          return "END opcode expected";
	case WASM_E_TYPE:          return "type mismatch";
	case WASM_E_INDEX:         return "unknown index";
	case WASM_E_LIMITS:        return "size minimum must not be greater than maximum";
	case WASM_E_MEMSIZE:       return "memory size must be at most 65536 pages";
	case WASM_E_DUP_EXPORT:    return "duplicate export name";
	case WASM_E_START:         return "start function must have type [] -> []";
	case WASM_E_CONSTEXPR:     return "constant expression required";
	case WASM_E_MUT_GLOBAL:    return "global is immutable";
	case WASM_E_ALIGN:         return "alignment must not be larger than natural";
	case WASM_E_MULTI_TABLE:   return "multiple tables";
	case WASM_E_MULTI_MEMORY:  return "multiple memories";
	case WASM_E_UNSUPPORTED:   return "post-MVP feature refused by name (not implemented)";
	case WASM_E_NOMEM:         return "APPARATUS: decoder arena exhausted";
	case WASM_E_LIMIT:         return "APPARATUS: decoder implementation limit";
	default:                   return "unknown error";
	}
}

int wasm_err_is_rejection(int e)
{
	if (e == WASM_OK) return 0;
	if (e == WASM_E_NOMEM || e == WASM_E_LIMIT) return 0;  /* about US */
	if (e == WASM_E_UNSUPPORTED) return 0;                 /* also about US */
	return 1;
}

/* ---- the injected allocator ------------------------------------------ */

void wasm_arena_init(struct wasm_arena *a, void *base, uint32_t size)
{
	a->base = (unsigned char *)base;
	a->size = size;
	a->used = 0;
}

void *wasm_arena_alloc(struct wasm_arena *a, uint32_t count, uint32_t elemsize)
{
	uint64_t want = (uint64_t)count * (uint64_t)elemsize;
	uint32_t off, i;

	if (count == 0) return a->base + a->used;   /* a zero-length array is a valid pointer */
	if (elemsize == 0) return a->base + a->used;
	if (want > 0xFFFFFFFFull) return 0;
	off = (a->used + 7u) & ~7u;
	if (off < a->used) return 0;                          /* alignment overflow */
	if (off > a->size || want > (uint64_t)(a->size - off)) return 0;
	a->used = off + (uint32_t)want;
	for (i = off; i < a->used; i++) a->base[i] = 0;
	return a->base + off;
}

/* ---- readers --------------------------------------------------------- */

int wasm_rd_u8(struct rd *r, uint8_t *out)
{
	if (r->i >= r->n) return WASM_E_END;
	*out = r->p[r->i++];
	return WASM_OK;
}

int wasm_rd_skip(struct rd *r, uint32_t nbytes)
{
	if (nbytes > r->n - r->i) return WASM_E_END;
	r->i += nbytes;
	return WASM_OK;
}

/* uN.  `bits` is N. */
static int leb_u(struct rd *r, uint32_t bits, uint64_t *out)
{
	uint64_t res = 0;
	uint32_t shift = 0, k, maxb = (bits + 6) / 7;
#ifdef WASM_NEGCTL_LEB
	(void)maxb;
#endif

	for (k = 0;; k++) {
		uint8_t b;
		(void)k;
		int e = wasm_rd_u8(r, &b);
		if (e) return e;
#ifndef WASM_NEGCTL_LEB
		if (k == maxb - 1) {
			uint32_t rem = bits - shift;   /* 1..7 bits still permitted */
			if (b & 0x80) return WASM_E_LEB_LONG;
			if (rem < 7 && (uint32_t)(b >> rem) != 0) return WASM_E_LEB_LARGE;
		}
#endif
		if (shift < 64) res |= (uint64_t)(b & 0x7F) << shift;
		if (!(b & 0x80)) { *out = res; return WASM_OK; }
		shift += 7;
		if (shift > 128) return WASM_E_LEB_LONG;   /* only reachable under the negctl */
	}
}

/* sN. */
static int leb_s(struct rd *r, uint32_t bits, int64_t *out)
{
	uint64_t res = 0;
	uint32_t shift = 0, k, maxb = (bits + 6) / 7;
#ifdef WASM_NEGCTL_LEB
	(void)maxb;
#endif

	for (k = 0;; k++) {
		uint8_t b;
		(void)k;
		int e = wasm_rd_u8(r, &b);
		if (e) return e;
#ifndef WASM_NEGCTL_LEB
		if (k == maxb - 1) {
			uint32_t rem = bits - shift;   /* 1..7 */
			if (b & 0x80) return WASM_E_LEB_LONG;
			if (rem < 7) {
				uint8_t pay  = (uint8_t)(b & 0x7F);
				uint8_t sign = (uint8_t)((pay >> (rem - 1)) & 1);
				uint8_t up   = (uint8_t)(pay >> rem);            /* 7-rem bits */
				uint8_t want = (uint8_t)(sign ? ((1u << (7 - rem)) - 1u) : 0u);
				if (up != want) return WASM_E_LEB_LARGE;
			}
		}
#endif
		if (shift < 64) res |= (uint64_t)(b & 0x7F) << shift;
		shift += 7;
		if (shift > 128) return WASM_E_LEB_LONG;   /* only reachable under the negctl */
		if (!(b & 0x80)) {
			if (shift < 64 && (b & 0x40))
				res |= ~(uint64_t)0 << shift;
			*out = (int64_t)res;
			return WASM_OK;
		}
	}
}

int wasm_rd_u32(struct rd *r, uint32_t *out)
{
	uint64_t v; int e = leb_u(r, 32, &v);
	if (e) return e;
	*out = (uint32_t)v;
	return WASM_OK;
}

int wasm_rd_s32(struct rd *r, int32_t *out)
{
	int64_t v; int e = leb_s(r, 32, &v);
	if (e) return e;
	*out = (int32_t)v;
	return WASM_OK;
}

int wasm_rd_s33(struct rd *r, int64_t *out) { return leb_s(r, 33, out); }
int wasm_rd_s64(struct rd *r, int64_t *out) { return leb_s(r, 64, out); }

/* ---- UTF-8 ------------------------------------------------------------
 * The spec suite spends 528 cases on this (utf8-invalid-encoding.wast,
 * utf8-import-module.wast, utf8-import-field.wast, utf8-custom-section-id.wast
 * are 176 assertions each), which is a fair share of the whole malformed
 * corpus, so it is written out rather than approximated: no overlong form, no
 * surrogate (U+D800..U+DFFF), nothing above U+10FFFF, and a continuation byte
 * is 0b10xxxxxx and nothing else. */
int wasm_utf8_ok(const uint8_t *p, uint32_t n)
{
	uint32_t i = 0;
	while (i < n) {
		uint8_t b0 = p[i];
		uint32_t need, cp, k;
		if (b0 < 0x80) { i++; continue; }
		if (b0 < 0xC2) return 0;                 /* 0x80..0xBF stray, 0xC0/0xC1 overlong */
		else if (b0 < 0xE0) { need = 1; cp = b0 & 0x1Fu; }
		else if (b0 < 0xF0) { need = 2; cp = b0 & 0x0Fu; }
		else if (b0 < 0xF5) { need = 3; cp = b0 & 0x07u; }
		else return 0;                            /* 0xF5..0xFF: above U+10FFFF */
		if (need > n - i - 1) return 0;
		for (k = 1; k <= need; k++) {
			uint8_t c = p[i + k];
			if ((c & 0xC0) != 0x80) return 0;
			cp = (cp << 6) | (uint32_t)(c & 0x3F);
		}
		if (need == 2 && cp < 0x800) return 0;    /* overlong */
		if (need == 3 && cp < 0x10000) return 0;  /* overlong */
		if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
		if (cp > 0x10FFFF) return 0;
		i += need + 1;
	}
	return 1;
}

int wasm_rd_name(struct rd *r, struct wasm_span *out)
{
	uint32_t len; int e = wasm_rd_u32(r, &len);
	if (e) return e;
	if (len > r->n - r->i) return WASM_E_END;
	out->off = r->i;
	out->len = len;
	if (!wasm_utf8_ok(r->p + r->i, len)) return WASM_E_UTF8;
	r->i += len;
	return WASM_OK;
}

int wasm_rd_valtype(struct rd *r, uint8_t *out)
{
	uint8_t b; int e = wasm_rd_u8(r, &b);
	if (e) return e;
	switch (b) {
	case WASM_VT_I32: case WASM_VT_I64:
	case WASM_VT_F32: case WASM_VT_F64:
		*out = b; return WASM_OK;
	case 0x7B:                 /* v128 */
	case 0x70: case 0x6F:      /* funcref / externref as a VALUE type */
		return WASM_E_UNSUPPORTED;
	default:
		/* Typed function references (0x64 `ref ht`, 0x63 `ref null ht`) and
		 * the GC heap-type shorthands are REAL value types in a later spec,
		 * so calling them "malformed value type" is present-and-wrong in the
		 * one direction this gate cannot see: WASM_E_VALTYPE is counted as a
		 * REJECTION, and a rejection of a module that is valid WebAssembly is
		 * a wrongly-rejected case dressed up as a correct one.  Refused by
		 * name instead, which lands them in the out-of-scope column where a
		 * reader can see how much of the suite an MVP decoder cannot judge.
		 * Measured: this is type-equivalence.wast:5 and :16, and the seven
		 * `(ref func)` tables of elem.wast. */
		if (b == 0x63 || b == 0x64) return WASM_E_UNSUPPORTED;
		if (b >= 0x69 && b <= 0x6E) return WASM_E_UNSUPPORTED;  /* exn..eq */
		if (b >= 0x71 && b <= 0x73) return WASM_E_UNSUPPORTED;  /* null*ref */
		return WASM_E_VALTYPE;
	}
}

/* ---- vector guard -----------------------------------------------------
 * Every element of every vector in this format occupies at least one byte,
 * so a count larger than the bytes remaining cannot be honest.  Checking it
 * BEFORE the allocation is what keeps a five-byte length prefix from turning
 * into a four-gigabyte allocation. */
static int vec_count(struct rd *r, uint32_t *out)
{
	int e = wasm_rd_u32(r, out);
	if (e) return e;
	if (*out > wasm_rd_left(r)) return WASM_E_END;
	return WASM_OK;
}

/* ---- limits, table, memory, global types ------------------------------ */

static int rd_limits(struct rd *r, struct wasm_limits *l)
{
	uint8_t flags; int e = wasm_rd_u8(r, &flags);
	if (e) return e;
	/* 0x02/0x03 mark a SHARED memory (threads); 0x04..0x07 set the index-type
	 * bit that makes this a 64-bit memory or table (memory64).  Both are real
	 * proposals with real semantics, so they are refused BY NAME rather than
	 * called malformed -- a reader who is told "malformed limits flags" about
	 * a perfectly well-formed memory64 module goes looking for the wrong bug.
	 * The current suite leans on this: memory64-imports.wast alone is 62
	 * modules whose only difference from MVP is this byte. */
	if (flags >= 0x02 && flags <= 0x07) return WASM_E_UNSUPPORTED;
	if (flags > 0x01) return WASM_E_FLAGS;
	e = wasm_rd_u32(r, &l->min); if (e) return e;
	l->has_max = flags;
	l->max = 0;
	if (flags) { e = wasm_rd_u32(r, &l->max); if (e) return e; }
	return WASM_OK;
}

static int rd_tabletype(struct rd *r, struct wasm_tabletype *t)
{
	uint8_t et; int e = wasm_rd_u8(r, &et);
	if (e) return e;
	if (et == 0x6F) return WASM_E_UNSUPPORTED;     /* externref: reference types */
	/* 0x40 is not an element type at all: it is the table-with-initialiser
	 * form of the GC proposal, `\40\00` followed by the real table type and a
	 * constant expression (elem.wast:453 and six siblings).  Reading it as an
	 * element type and calling it malformed says the wrong thing about a
	 * well-formed module. */
	if (et == 0x40) return WASM_E_UNSUPPORTED;
	if (et != WASM_ET_FUNCREF) {
		/* Same argument as wasm_rd_valtype's default arm: a table whose
		 * element type is `(ref func)` is a typed-function-references table,
		 * not a malformed one. */
		uint8_t vt;
		r->i--;                                    /* put the byte back */
		e = wasm_rd_valtype(r, &vt);
		return e ? e : WASM_E_VALTYPE;
	}
	t->elemtype = et;
	return rd_limits(r, &t->lim);
}

static int rd_globaltype(struct rd *r, struct wasm_globaltype *g)
{
	uint8_t mut; int e = wasm_rd_valtype(r, &g->valtype);
	if (e) return e;
	e = wasm_rd_u8(r, &mut); if (e) return e;
	if (mut > 1) return WASM_E_MUT;
	g->mut = mut;
	return WASM_OK;
}

/* ---- sections --------------------------------------------------------- */

static int sec_type(struct rd *r, struct wasm_module *m, struct wasm_arena *a)
{
	uint32_t i, j;
	int e = vec_count(r, &m->ntypes);
	if (e) return e;
	m->types = wasm_arena_alloc(a, m->ntypes, sizeof *m->types);
	if (!m->types) return WASM_E_NOMEM;
	for (i = 0; i < m->ntypes; i++) {
		struct wasm_functype *ft = &m->types[i];
		uint8_t form;
		e = wasm_rd_u8(r, &form); if (e) return e;
		if (form != WASM_FORM_FUNC) return WASM_E_FORM;
		e = vec_count(r, &ft->nparams); if (e) return e;
		ft->params = r->p + r->i;
		for (j = 0; j < ft->nparams; j++) {
			uint8_t vt;
			e = wasm_rd_valtype(r, &vt); if (e) return e;
		}
		e = vec_count(r, &ft->nresults); if (e) return e;
		/* MVP: at most one result.  Multi-value is a real proposal and is
		 * refused by name rather than truncated. */
		if (ft->nresults > 1) return WASM_E_UNSUPPORTED;
		ft->results = r->p + r->i;
		for (j = 0; j < ft->nresults; j++) {
			uint8_t vt;
			e = wasm_rd_valtype(r, &vt); if (e) return e;
		}
	}
	return WASM_OK;
}

static int sec_import(struct rd *r, struct wasm_module *m, struct wasm_arena *a)
{
	uint32_t i;
	int e = vec_count(r, &m->nimports);
	if (e) return e;
	m->imports = wasm_arena_alloc(a, m->nimports, sizeof *m->imports);
	if (!m->imports) return WASM_E_NOMEM;
	for (i = 0; i < m->nimports; i++) {
		struct wasm_import *im = &m->imports[i];
		uint8_t kind;
		e = wasm_rd_name(r, &im->module_name); if (e) return e;
		e = wasm_rd_name(r, &im->field_name);  if (e) return e;
		e = wasm_rd_u8(r, &kind);              if (e) return e;
		im->kind = kind;
		switch (kind) {
		case WASM_EXT_FUNC:
			e = wasm_rd_u32(r, &im->typeidx); if (e) return e;
			m->nimp_funcs++;
			break;
		case WASM_EXT_TABLE:
			e = rd_tabletype(r, &im->tt); if (e) return e;
			m->nimp_tables++;
			break;
		case WASM_EXT_MEM:
			e = rd_limits(r, &im->mem); if (e) return e;
			m->nimp_mems++;
			break;
		case WASM_EXT_GLOBAL:
			e = rd_globaltype(r, &im->gt); if (e) return e;
			m->nimp_globals++;
			break;
		case 4:
			return WASM_E_UNSUPPORTED;   /* tag import: exception handling */
		default:
			return WASM_E_IMPORT_KIND;
		}
	}
	return WASM_OK;
}

static int sec_function(struct rd *r, struct wasm_module *m, struct wasm_arena *a)
{
	uint32_t i;
	int e = vec_count(r, &m->nfuncs);
	if (e) return e;
	m->funcs = wasm_arena_alloc(a, m->nfuncs, sizeof *m->funcs);
	if (!m->funcs) return WASM_E_NOMEM;
	for (i = 0; i < m->nfuncs; i++) {
		e = wasm_rd_u32(r, &m->funcs[i]); if (e) return e;
	}
	return WASM_OK;
}

static int sec_table(struct rd *r, struct wasm_module *m, struct wasm_arena *a)
{
	uint32_t i;
	int e = vec_count(r, &m->ntables);
	if (e) return e;
	m->tables = wasm_arena_alloc(a, m->ntables, sizeof *m->tables);
	if (!m->tables) return WASM_E_NOMEM;
	for (i = 0; i < m->ntables; i++) {
		e = rd_tabletype(r, &m->tables[i]); if (e) return e;
	}
	return WASM_OK;
}

static int sec_memory(struct rd *r, struct wasm_module *m, struct wasm_arena *a)
{
	uint32_t i;
	int e = vec_count(r, &m->nmems);
	if (e) return e;
	m->mems = wasm_arena_alloc(a, m->nmems, sizeof *m->mems);
	if (!m->mems) return WASM_E_NOMEM;
	for (i = 0; i < m->nmems; i++) {
		e = rd_limits(r, &m->mems[i]); if (e) return e;
	}
	return WASM_OK;
}

static int sec_global(struct rd *r, struct wasm_module *m, struct wasm_arena *a)
{
	uint32_t i;
	int e = vec_count(r, &m->nglobals);
	if (e) return e;
	m->globals = wasm_arena_alloc(a, m->nglobals, sizeof *m->globals);
	if (!m->globals) return WASM_E_NOMEM;
	for (i = 0; i < m->nglobals; i++) {
		uint32_t start;
		e = rd_globaltype(r, &m->globals[i].gt); if (e) return e;
		start = r->i;
		e = wasm_expr_extent(r); if (e) return e;
		m->globals[i].init.off = start;
		m->globals[i].init.len = r->i - start;
	}
	return WASM_OK;
}

static int sec_export(struct rd *r, struct wasm_module *m, struct wasm_arena *a)
{
	uint32_t i;
	int e = vec_count(r, &m->nexports);
	if (e) return e;
	m->exports = wasm_arena_alloc(a, m->nexports, sizeof *m->exports);
	if (!m->exports) return WASM_E_NOMEM;
	for (i = 0; i < m->nexports; i++) {
		uint8_t kind;
		e = wasm_rd_name(r, &m->exports[i].name); if (e) return e;
		e = wasm_rd_u8(r, &kind); if (e) return e;
		if (kind == 4) return WASM_E_UNSUPPORTED;   /* tag export */
		if (kind > WASM_EXT_GLOBAL) return WASM_E_IMPORT_KIND;
		m->exports[i].kind = kind;
		e = wasm_rd_u32(r, &m->exports[i].index); if (e) return e;
	}
	return WASM_OK;
}

static int sec_start(struct rd *r, struct wasm_module *m)
{
	int e = wasm_rd_u32(r, &m->start);
	if (e) return e;
	m->has_start = 1;
	return WASM_OK;
}

static int sec_element(struct rd *r, struct wasm_module *m, struct wasm_arena *a)
{
	uint32_t i;
	int e = vec_count(r, &m->nelems);
	if (e) return e;
	m->elems = wasm_arena_alloc(a, m->nelems, sizeof *m->elems);
	if (!m->elems) return WASM_E_NOMEM;
	for (i = 0; i < m->nelems; i++) {
		struct wasm_elem *el = &m->elems[i];
		uint32_t start, j, raw0;
		e = wasm_rd_u32(r, &el->tableidx); if (e) return e;
		/* Bulk memory / reference types spell passive and declarative
		 * segments as flags 1..7 in this field.  MVP has only the active
		 * form with table 0, so anything else is a real feature and is
		 * refused by name rather than mis-decoded as a table index. */
		if (el->tableidx != 0) return WASM_E_UNSUPPORTED;
		start = r->i;
		e = wasm_expr_extent(r); if (e) return e;
		el->offset.off = start;
		el->offset.len = r->i - start;
		e = vec_count(r, &el->nfunc); if (e) return e;
		raw0 = r->i;
		for (j = 0; j < el->nfunc; j++) {
			uint32_t f;
			e = wasm_rd_u32(r, &f); if (e) return e;
		}
		el->funcs_raw = r->p + raw0;
		el->funcs_raw_len = r->i - raw0;
	}
	return WASM_OK;
}

static int sec_data(struct rd *r, struct wasm_module *m, struct wasm_arena *a)
{
	uint32_t i;
	int e = vec_count(r, &m->ndatas);
	if (e) return e;
	m->datas = wasm_arena_alloc(a, m->ndatas, sizeof *m->datas);
	if (!m->datas) return WASM_E_NOMEM;
	for (i = 0; i < m->ndatas; i++) {
		struct wasm_data *d = &m->datas[i];
		uint32_t start, len;
		e = wasm_rd_u32(r, &d->memidx); if (e) return e;
		if (d->memidx != 0) return WASM_E_UNSUPPORTED;   /* passive data: bulk memory */
		start = r->i;
		e = wasm_expr_extent(r); if (e) return e;
		d->offset.off = start;
		d->offset.len = r->i - start;
		e = wasm_rd_u32(r, &len); if (e) return e;
		if (len > wasm_rd_left(r)) return WASM_E_END;
		d->bytes.off = r->i;
		d->bytes.len = len;
		r->i += len;
	}
	return WASM_OK;
}

static int sec_code(struct rd *r, struct wasm_module *m, struct wasm_arena *a)
{
	uint32_t i;
	int e = vec_count(r, &m->ncode);
	if (e) return e;
	m->code = wasm_arena_alloc(a, m->ncode, sizeof *m->code);
	if (!m->code) return WASM_E_NOMEM;
	for (i = 0; i < m->ncode; i++) {
		struct wasm_code *c = &m->code[i];
		uint32_t size, j;
		uint64_t total = 0;
		struct rd body;
		e = wasm_rd_u32(r, &size); if (e) return e;
		if (size > wasm_rd_left(r)) return WASM_E_END;
		c->body.off = r->i;
		c->body.len = size;
		body.p = r->p; body.i = r->i; body.n = r->i + size;
		r->i += size;

		e = vec_count(&body, &c->ndecl); if (e) return e;
		c->decl = wasm_arena_alloc(a, c->ndecl, sizeof *c->decl);
		if (!c->decl) return WASM_E_NOMEM;
		for (j = 0; j < c->ndecl; j++) {
			e = wasm_rd_u32(&body, &c->decl[j].count); if (e) return e;
			e = wasm_rd_valtype(&body, &c->decl[j].type); if (e) return e;
			total += c->decl[j].count;
			if (total > 0xFFFFFFFFull) return WASM_E_LOCALS;
		}
		c->nlocals = (uint32_t)total;
		c->expr.off = body.i;
		e = wasm_expr_extent(&body);
		if (e) return e;
		c->expr.len = body.i - c->expr.off;
		/* The body's declared size must be exactly consumed.  Junk after
		 * the closing `end` is "section size mismatch", not a warning. */
		if (body.i != body.n) return WASM_E_SECTION_SIZE;
	}
	return WASM_OK;
}

/* ---- index spaces ----------------------------------------------------- */

static int build_index_spaces(struct wasm_module *m, struct wasm_arena *a)
{
	uint32_t i, nf = 0, ng = 0;

	if ((uint64_t)m->nimp_funcs + m->nfuncs > 0xFFFFFFFFull) return WASM_E_LIMIT;
	m->total_funcs   = m->nimp_funcs + m->nfuncs;
	m->total_tables  = m->nimp_tables + m->ntables;
	m->total_mems    = m->nimp_mems + m->nmems;
	m->total_globals = m->nimp_globals + m->nglobals;

	m->functype_of = wasm_arena_alloc(a, m->total_funcs, sizeof *m->functype_of);
	if (!m->functype_of) return WASM_E_NOMEM;
	m->globaltype_of = wasm_arena_alloc(a, m->total_globals, sizeof *m->globaltype_of);
	if (!m->globaltype_of) return WASM_E_NOMEM;

	for (i = 0; i < m->nimports; i++) {
		if (m->imports[i].kind == WASM_EXT_FUNC)
			m->functype_of[nf++] = m->imports[i].typeidx;
		else if (m->imports[i].kind == WASM_EXT_GLOBAL)
			m->globaltype_of[ng++] = m->imports[i].gt;
	}
	for (i = 0; i < m->nfuncs; i++) m->functype_of[nf++] = m->funcs[i];
	for (i = 0; i < m->nglobals; i++) m->globaltype_of[ng++] = m->globals[i].gt;
	return WASM_OK;
}

/* ---- the preamble and the section loop -------------------------------- */

int wasm_decode(const uint8_t *bytes, uint32_t len,
                struct wasm_module *m, struct wasm_arena *a)
{
	struct rd r;
	uint32_t k;
	int last_id = 0, e;
	int saw_func = 0, saw_code = 0;

	for (k = 0; k < sizeof *m; k++) ((unsigned char *)m)[k] = 0;
	m->bytes = bytes;
	m->len = len;

	if (len < 4 || bytes[0] != 0x00 || bytes[1] != 0x61 ||
	    bytes[2] != 0x73 || bytes[3] != 0x6D) {
		/* "unexpected end" when the file is simply too short to hold a
		 * magic at all -- the suite distinguishes the two and so does
		 * anyone reading a log. */
		uint32_t i;
		static const uint8_t magic[4] = { 0x00, 0x61, 0x73, 0x6D };
		for (i = 0; i < len && i < 4; i++)
			if (bytes[i] != magic[i]) return WASM_E_MAGIC;
		return WASM_E_END;
	}
	if (len < 8) return WASM_E_END;
	if (bytes[4] != 0x01 || bytes[5] || bytes[6] || bytes[7])
		return WASM_E_VERSION;

	r.p = bytes; r.n = len; r.i = 8;

	while (r.i < r.n) {
		uint8_t id;
		uint32_t size;
		struct rd sub;

		e = wasm_rd_u8(&r, &id); if (e) return e;
		/* 12 is the data-count section (bulk memory) and 13 is the tag
		 * section (exception handling).  Both are assigned ids in the live
		 * spec, so 14 is where "malformed section id" starts -- and
		 * binary.wast asserts exactly that boundary (\0e, \7f, \80\01). */
		if (id > 13) return WASM_E_SECTION_ID;
		e = wasm_rd_u32(&r, &size); if (e) return e;
		if (size > wasm_rd_left(&r)) return WASM_E_END;

		sub.p = r.p; sub.i = r.i; sub.n = r.i + size;
		r.i += size;

		if (id == 0) {
			/* custom: a name, then bytes we do not interpret.  The name
			 * is still UTF-8 validated -- utf8-custom-section-id.wast is
			 * 176 assertions about exactly that. */
			struct wasm_span nm;
			e = wasm_rd_name(&sub, &nm); if (e) return e;
			continue;
		}
		if ((int)id <= last_id) return WASM_E_SECTION_ORDER;
		last_id = id;

		switch (id) {
		case 1:  e = sec_type(&sub, m, a); break;
		case 2:  e = sec_import(&sub, m, a); break;
		case 3:  e = sec_function(&sub, m, a); saw_func = 1; break;
		case 4:  e = sec_table(&sub, m, a); break;
		case 5:  e = sec_memory(&sub, m, a); break;
		case 6:  e = sec_global(&sub, m, a); break;
		case 7:  e = sec_export(&sub, m, a); break;
		case 8:  e = sec_start(&sub, m); break;
		case 9:  e = sec_element(&sub, m, a); break;
		case 10: e = sec_code(&sub, m, a); saw_code = 1; break;
		case 11: e = sec_data(&sub, m, a); break;
		case 12: case 13: e = WASM_E_UNSUPPORTED; break;  /* data count / tag */
		default: e = WASM_E_SECTION_ID; break;
		}
		if (e) return e;
		if (sub.i != sub.n) return WASM_E_SECTION_SIZE;
	}

	/* "function and code section have inconsistent lengths" -- and the
	 * absent-section case is part of it: a function section with no code
	 * section is the same defect with a different spelling. */
	if (saw_func != saw_code) {
		if (saw_func && m->nfuncs != 0) return WASM_E_LENGTHS;
		if (saw_code && m->ncode != 0) return WASM_E_LENGTHS;
	}
	if (m->nfuncs != m->ncode) return WASM_E_LENGTHS;

	return build_index_spaces(m, a);
}

int wasm_load(const uint8_t *bytes, uint32_t len,
              struct wasm_module *m, struct wasm_arena *a)
{
	int e = wasm_decode(bytes, len, m, a);
	if (e) return e;
	return wasm_validate(m, a);
}
