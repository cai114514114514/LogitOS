//! RFC 1951 DEFLATE inflate + RFC 1950 zlib, ported from `src/lib/image/inflate.c`
//! to **safe Rust**. This is the first real hybrid C+Rust module: it processes
//! UNTRUSTED input (PNG/GIF IDAT streams), exactly the bug class the security audit
//! kept finding in the C parsers. All indexing is bounds-checked; malformed input
//! returns -1 and NEVER panics, aborts, or reads/writes out of bounds.
//!
//! Same C ABI as the original (drop-in: the C inflate.o is excluded from the build,
//! these `#[no_mangle]` symbols replace `inflate_raw` / `zlib_decompress`).

use core::slice;

const MAXLEN: usize = 16;     // max canonical code length + 1
const MAXSYM: usize = 288;    // literal/length alphabet size

/// LSB-first bit reader over a borrowed input slice.
struct BitR<'a> {
    p: &'a [u8],
    pos: usize,
    bits: u32,
    nbits: i32,
}

impl<'a> BitR<'a> {
    fn new(p: &'a [u8]) -> Self { BitR { p, pos: 0, bits: 0, nbits: 0 } }

    fn refill(&mut self, need: i32) {
        while self.nbits < need && self.pos < self.p.len() {
            self.bits |= (self.p[self.pos] as u32) << self.nbits;
            self.pos += 1;
            self.nbits += 8;
        }
    }
    fn getbit(&mut self) -> i32 {
        if self.nbits == 0 {
            if self.pos >= self.p.len() { return -1; }
            self.bits = self.p[self.pos] as u32;
            self.pos += 1;
            self.nbits = 8;
        }
        let v = (self.bits & 1) as i32;
        self.bits >>= 1;
        self.nbits -= 1;
        v
    }
    fn getbits(&mut self, n: i32) -> i32 {
        if n <= 0 { return 0; }
        if self.nbits < n { self.refill(n); }
        if self.nbits < n { return -1; }
        let mask = (1u32 << n) - 1;
        let v = (self.bits & mask) as i32;
        self.bits >>= n;
        self.nbits -= n;
        v
    }
}

/// Canonical Huffman table (per-length counts + flat symbol list).
struct Huff {
    count: [i32; MAXLEN],
    sym: [i32; MAXSYM],
}

/// Build from code lengths. Defensive vs the C version: a length >= MAXLEN or a
/// symbol-table overflow returns None (the C trusts its callers) -> the caller
/// turns that into a -1, no out-of-bounds.
fn huff_build(lengths: &[u8]) -> Option<Huff> {
    let mut h = Huff { count: [0; MAXLEN], sym: [0; MAXSYM] };
    for &l in lengths {
        let l = l as usize;
        if l >= MAXLEN { return None; }
        h.count[l] += 1;
    }
    h.count[0] = 0;
    let mut offs = [0i32; MAXLEN];
    for l in 1..MAXLEN - 1 {
        offs[l + 1] = offs[l] + h.count[l];
    }
    for (i, &l) in lengths.iter().enumerate() {
        let l = l as usize;
        if l != 0 {
            let o = offs[l] as usize;
            if o >= MAXSYM { return None; }
            h.sym[o] = i as i32;
            offs[l] += 1;
        }
    }
    Some(h)
}

fn huff_decode(b: &mut BitR, h: &Huff) -> i32 {
    b.refill((MAXLEN - 1) as i32);
    let mut reg = b.bits;
    let mut avail = b.nbits;
    let (mut code, mut first, mut index) = (0i32, 0i32, 0i32);
    for len in 1..MAXLEN {
        if avail <= 0 { return -1; }
        avail -= 1;
        code |= (reg & 1) as i32;
        reg >>= 1;
        let cnt = h.count[len];
        if code - first < cnt {
            b.bits = reg;
            b.nbits = avail;
            let idx = index + (code - first);
            if idx < 0 || idx as usize >= MAXSYM { return -1; }
            return h.sym[idx as usize];
        }
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
    }
    -1
}

const LEN_BASE: [i32; 29] = [3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258];
const LEN_EXTRA: [i32; 29] = [0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0];
const DIST_BASE: [i32; 30] = [1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577];
const DIST_EXTRA: [i32; 30] = [0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13];
const CLC_ORDER: [usize; 19] = [16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15];

fn block_body(b: &mut BitR, lit: &Huff, dist: &Huff, out: &mut [u8], op: &mut usize) -> i32 {
    let outcap = out.len();
    loop {
        let s = huff_decode(b, lit);
        if s < 0 { return -1; }
        if s == 256 { return 0; }                          // end of block
        if s < 256 {
            if *op >= outcap { return -1; }
            out[*op] = s as u8;
            *op += 1;
            continue;
        }
        let s = (s - 257) as usize;
        if s >= 29 { return -1; }
        let e = b.getbits(LEN_EXTRA[s]); if e < 0 { return -1; }
        let length = (LEN_BASE[s] + e) as usize;
        let ds = huff_decode(b, dist); if ds < 0 || ds >= 30 { return -1; }
        let de = b.getbits(DIST_EXTRA[ds as usize]); if de < 0 { return -1; }
        let distance = (DIST_BASE[ds as usize] + de) as usize;
        if distance == 0 || distance > *op || *op + length > outcap { return -1; }
        for _ in 0..length {
            out[*op] = out[*op - distance];                 // both indices bounds-guarded above
            *op += 1;
        }
    }
}

fn inflate_core(b: &mut BitR, out: &mut [u8], op: &mut usize) -> i32 {
    let outcap = out.len();
    loop {
        let final_ = b.getbit(); if final_ < 0 { return -1; }
        let btype = b.getbits(2); if btype < 0 { return -1; }
        if btype == 0 {                                      // stored
            b.nbits = 0; b.bits = 0;                         // byte-align, drop partial
            if b.pos + 4 > b.p.len() { return -1; }
            let len = (b.p[b.pos] as usize) | ((b.p[b.pos + 1] as usize) << 8);
            b.pos += 4;                                      // skip LEN + NLEN
            if b.pos + len > b.p.len() || *op + len > outcap { return -1; }
            for _ in 0..len { out[*op] = b.p[b.pos]; *op += 1; b.pos += 1; }
        } else if btype == 1 || btype == 2 {
            let lit;
            let dist;
            if btype == 1 {                                  // fixed Huffman
                let mut ll = [0u8; 288];
                for v in ll[0..144].iter_mut() { *v = 8; }
                for v in ll[144..256].iter_mut() { *v = 9; }
                for v in ll[256..280].iter_mut() { *v = 7; }
                for v in ll[280..288].iter_mut() { *v = 8; }
                let dl = [5u8; 30];
                lit = match huff_build(&ll) { Some(h) => h, None => return -1 };
                dist = match huff_build(&dl) { Some(h) => h, None => return -1 };
            } else {                                         // dynamic Huffman
                let hlit = b.getbits(5) + 257;
                let hdist = b.getbits(5) + 1;
                let hclen = b.getbits(4) + 4;
                if hlit > 286 || hdist > 30 { return -1; }
                let mut cll = [0u8; 19];
                for i in 0..hclen as usize {
                    if i >= 19 { return -1; }
                    let v = b.getbits(3); if v < 0 { return -1; }
                    cll[CLC_ORDER[i]] = v as u8;
                }
                let cl = match huff_build(&cll) { Some(h) => h, None => return -1 };
                let mut lens = [0u8; 288 + 30];
                let total = (hlit + hdist) as usize;
                let mut n = 0usize;
                while n < total {
                    let s = huff_decode(b, &cl); if s < 0 { return -1; }
                    if s < 16 {
                        lens[n] = s as u8; n += 1;
                    } else if s == 16 {
                        let g = b.getbits(2); if g < 0 || n == 0 { return -1; }
                        let mut r = g + 3;
                        while r > 0 && n < total { lens[n] = lens[n - 1]; n += 1; r -= 1; }
                    } else if s == 17 {
                        let g = b.getbits(3); if g < 0 { return -1; }
                        let mut r = g + 3;
                        while r > 0 && n < total { lens[n] = 0; n += 1; r -= 1; }
                    } else {
                        let g = b.getbits(7); if g < 0 { return -1; }
                        let mut r = g + 11;
                        while r > 0 && n < total { lens[n] = 0; n += 1; r -= 1; }
                    }
                }
                lit = match huff_build(&lens[..hlit as usize]) { Some(h) => h, None => return -1 };
                dist = match huff_build(&lens[hlit as usize..(hlit + hdist) as usize]) { Some(h) => h, None => return -1 };
            }
            if block_body(b, &lit, &dist, out, op) != 0 { return -1; }
        } else {
            return -1;                                       // reserved BTYPE
        }
        if final_ != 0 { break; }
    }
    0
}

/// C ABI: `int inflate_raw(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)`.
#[no_mangle]
pub extern "C" fn inflate_raw(input: *const u8, inlen: i32, out: *mut u8, outcap: i32, outlen: *mut i32) -> i32 {
    if input.is_null() || out.is_null() || outlen.is_null() || inlen < 0 || outcap < 0 {
        return -1;
    }
    let inb = unsafe { slice::from_raw_parts(input, inlen as usize) };
    let outb = unsafe { slice::from_raw_parts_mut(out, outcap as usize) };
    let mut b = BitR::new(inb);
    let mut op: usize = 0;
    if inflate_core(&mut b, outb, &mut op) != 0 { return -1; }
    unsafe { *outlen = op as i32; }
    0
}

/// C ABI: `int zlib_decompress(const uint8_t *in, int inlen, uint8_t *out, int outcap, int *outlen)`.
#[no_mangle]
pub extern "C" fn zlib_decompress(input: *const u8, inlen: i32, out: *mut u8, outcap: i32, outlen: *mut i32) -> i32 {
    if input.is_null() || inlen < 2 { return -1; }
    let inb = unsafe { slice::from_raw_parts(input, inlen as usize) };
    if (inb[0] & 0x0f) != 8 { return -1; }                  // not DEFLATE
    let mut off = 2usize;
    if (inb[1] & 0x20) != 0 { off += 4; }                   // preset dictionary id
    if off >= inlen as usize { return -1; }
    inflate_raw(unsafe { input.add(off) }, inlen - off as i32, out, outcap, outlen)
}

/// Boot self-test: decompress a baked-in zlib vector and check length + checksum.
/// Returns 0 on success, -1 on failure. (Vector: python `zlib.compress(...,9)`.)
#[no_mangle]
pub extern "C" fn rust_inflate_selftest() -> i32 {
    const COMP: [u8; 112] = [
        0x78,0xda,0x73,0x4c,0x2d,0xc9,0x48,0x2d,0x52,0xf0,0x0f,0x56,0xc8,0xa8,0x4c,0x2a,0xca,0x4c,0x51,0x70,
        0xd6,0x0e,0x2a,0x2d,0x2e,0x51,0xc8,0xcc,0x4b,0xcb,0x49,0x2c,0x49,0x55,0x28,0x4e,0xcd,0x49,0xd3,0x2d,
        0x49,0x2d,0x2e,0xd1,0x53,0x70,0x1c,0x55,0x4a,0xbc,0xd2,0x90,0x8c,0x54,0x85,0xc2,0xd2,0xcc,0xe4,0x6c,
        0x85,0xa4,0xa2,0xfc,0xf2,0x3c,0x85,0xb4,0xfc,0x0a,0x85,0xac,0xd2,0xdc,0x82,0x62,0x85,0xfc,0x32,0xa0,
        0x11,0x40,0x73,0x14,0x72,0x12,0xab,0x2a,0x15,0x52,0xf2,0xd3,0xf5,0x14,0x0c,0x0c,0x8d,0x8c,0x4d,0x4c,
        0xcd,0xcc,0x2d,0x2c,0x87,0x94,0x46,0x00,0xc6,0xbb,0xc2,0x11,
    ];
    const EXPECT_LEN: i32 = 572;
    const EXPECT_SUM: u32 = 49680;
    let mut out = [0u8; 1024];
    let mut olen: i32 = 0;
    let rc = zlib_decompress(COMP.as_ptr(), COMP.len() as i32, out.as_mut_ptr(), out.len() as i32, &mut olen);
    if rc != 0 || olen != EXPECT_LEN { return -1; }
    let mut sum: u32 = 0;
    for &x in out[..olen as usize].iter() { sum = sum.wrapping_add(x as u32); }
    if sum != EXPECT_SUM { return -1; }
    0
}
