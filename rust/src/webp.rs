//! WebP decoder in safe Rust: the RIFF container plus **VP8L**, the lossless
//! codec. Output is straight RGBA8.
//!
//! What is here: the RIFF/`VP8X` container walk, and a complete VP8L decoder --
//! prefix (Huffman) coding with the simple and code-length-coded forms, meta
//! Huffman (per-block code selection via an entropy image), the colour cache,
//! LZ77 with the 120-entry short-distance plane mapping, and all four inverse
//! transforms (predictor, cross-colour, subtract-green, colour indexing with
//! pixel bundling).
//!
//! What is NOT here, and fails cleanly with -1 rather than half-decoding:
//! `VP8 ` (the lossy VP8 key-frame codec) and animated WebP (`ANIM`/`ANMF`).
//! See the report in the commit that added this file.
//!
//! Reference for correctness: `cwebp`/`dwebp` from libwebp. Lossless is
//! lossless, so the bar is byte-exact, and `make test-webp` enforces exactly
//! that against PIL's libwebp-backed decode of the identical file.

use crate::imgbuf::*;

// ---------------------------------------------------------------------------
// Bit reader: VP8L packs bits LSB-first within each byte, bytes in order.
// ---------------------------------------------------------------------------
struct Br<'a> {
    d: &'a [u8],
    pos: usize,
    val: u64,
    bits: u32,
    eof: bool,
}

impl<'a> Br<'a> {
    fn new(d: &'a [u8]) -> Br<'a> {
        Br { d, pos: 0, val: 0, bits: 0, eof: false }
    }

    /// Read `n` (<= 24) bits. Past the end returns zeros and latches `eof`;
    /// callers check `eof` rather than every call site having to.
    fn read(&mut self, n: u32) -> u32 {
        if n == 0 {
            return 0;
        }
        while self.bits < n {
            let b = if self.pos < self.d.len() {
                let v = self.d[self.pos];
                self.pos += 1;
                v
            } else {
                self.eof = true;
                0
            };
            self.val |= (b as u64) << self.bits;
            self.bits += 8;
        }
        let v = (self.val & ((1u64 << n) - 1)) as u32;
        self.val >>= n;
        self.bits -= n;
        v
    }
}

// ---------------------------------------------------------------------------
// Canonical Huffman, decoded one bit at a time (the "puff" formulation): the
// tables are counts-per-length plus the symbols in canonical order, which is
// both small and impossible to index out of range.
// ---------------------------------------------------------------------------
const MAX_LEN: usize = 15;

/// A pool of `ncodes` canonical Huffman codes. Per code: 16 counts, a base into
/// the shared symbol array, and a symbol count.
struct Huffs {
    counts: U16Buf, // ncodes * (MAX_LEN+1)
    syms: U16Buf,
    base: U32Buf,  // ncodes
    nsym: U32Buf,  // ncodes
}

impl Huffs {
    fn new(ncodes: usize, total_syms: usize) -> Option<Huffs> {
        Some(Huffs {
            counts: U16Buf::zeroed(ncodes.checked_mul(MAX_LEN + 1)?)?,
            syms: U16Buf::zeroed(total_syms.max(1))?,
            base: U32Buf::zeroed(ncodes.max(1))?,
            nsym: U32Buf::zeroed(ncodes.max(1))?,
        })
    }

    /// Build code `ci` from `lengths` (0 = symbol absent). Symbols land in the
    /// shared array starting at `base`. Returns false on an over-subscribed or
    /// empty code.
    fn build(&mut self, ci: usize, lengths: &[u8], base: usize) -> bool {
        let co = ci * (MAX_LEN + 1);
        let mut counts = [0u16; MAX_LEN + 1];
        for &l in lengths.iter() {
            if l as usize > MAX_LEN {
                return false;
            }
            counts[l as usize] += 1;
        }
        let nsym: usize = lengths.len() - counts[0] as usize;
        if nsym == 0 {
            return false;
        }
        // Kraft check. A single symbol of any length is the legal degenerate
        // case VP8L's "simple code" produces; it decodes with zero bits.
        if nsym > 1 {
            let mut left = 1i32;
            for l in 1..=MAX_LEN {
                left <<= 1;
                left -= counts[l] as i32;
                if left < 0 {
                    return false;
                }
            }
        }
        // Place symbols in canonical order: by length, then by symbol value.
        let mut next = [0usize; MAX_LEN + 1];
        let mut acc = 0usize;
        for l in 1..=MAX_LEN {
            next[l] = acc;
            acc += counts[l] as usize;
        }
        for (sym, &l) in lengths.iter().enumerate() {
            if l != 0 {
                let idx = base + next[l as usize];
                next[l as usize] += 1;
                if idx >= self.syms.len() {
                    return false;
                }
                self.syms.set(idx, sym as u16);
            }
        }
        for l in 0..=MAX_LEN {
            self.counts.set(co + l, counts[l]);
        }
        self.base.set(ci, base as u32);
        self.nsym.set(ci, nsym as u32);
        true
    }

    /// Decode one symbol of code `ci`. `None` on a code that runs off the end
    /// of the table (only reachable on corrupt input).
    fn decode(&self, ci: usize, br: &mut Br) -> Option<u32> {
        let base = self.base.get(ci) as usize;
        if self.nsym.get(ci) == 1 {
            return Some(self.syms.get(base) as u32); // zero-bit code
        }
        let co = ci * (MAX_LEN + 1);
        let (mut code, mut first, mut index) = (0i32, 0i32, 0i32);
        for l in 1..=MAX_LEN {
            code |= br.read(1) as i32;
            let count = self.counts.get(co + l) as i32;
            if code - first < count {
                return Some(self.syms.get(base + (index + (code - first)) as usize) as u32);
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        None
    }
}

// ---------------------------------------------------------------------------
// VP8L constants
// ---------------------------------------------------------------------------
const NUM_LITERAL: u32 = 256;
const NUM_LENGTH: u32 = 24;
const NUM_DISTANCE: usize = 40;
const CODE_LENGTH_CODES: usize = 19;

const CL_ORDER: [usize; CODE_LENGTH_CODES] =
    [17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15];

/// The 120 short LZ77 distances, as (x, y) offsets. Distance = y*width + x,
/// clamped to at least 1. Codes above 120 are plain `code - 120` distances.
#[rustfmt::skip]
const CODE_TO_PLANE: [(i32, i32); 120] = [
    (0,1),(1,0),(1,1),(-1,1),(0,2),(2,0),(1,2),(-1,2),
    (2,1),(-2,1),(2,2),(-2,2),(0,3),(3,0),(1,3),(-1,3),
    (3,1),(-3,1),(2,3),(-2,3),(3,2),(-3,2),(0,4),(4,0),
    (1,4),(-1,4),(4,1),(-4,1),(3,3),(-3,3),(2,4),(-2,4),
    (4,2),(-4,2),(0,5),(3,4),(-3,4),(4,3),(-4,3),(5,0),
    (1,5),(-1,5),(5,1),(-5,1),(2,5),(-2,5),(5,2),(-5,2),
    (4,4),(-4,4),(3,5),(-3,5),(5,3),(-5,3),(0,6),(6,0),
    (1,6),(-1,6),(6,1),(-6,1),(2,6),(-2,6),(6,2),(-6,2),
    (4,5),(-4,5),(5,4),(-5,4),(3,6),(-3,6),(6,3),(-6,3),
    (0,7),(7,0),(1,7),(-1,7),(5,5),(-5,5),(7,1),(-7,1),
    (4,6),(-4,6),(6,4),(-6,4),(2,7),(-2,7),(7,2),(-7,2),
    (3,7),(-3,7),(7,3),(-7,3),(5,6),(-5,6),(6,5),(-6,5),
    (8,0),(4,7),(-4,7),(7,4),(-7,4),(8,1),(8,2),(6,6),
    (-6,6),(8,3),(5,7),(-5,7),(7,5),(-7,5),(8,4),(6,7),
    (-6,7),(7,6),(-7,6),(8,5),(7,7),(-7,7),(8,6),(8,7),
];

fn div_round_up(a: u32, b: u32) -> u32 {
    (a + b - 1) / b
}

/// Prefix coding shared by the length and distance alphabets: codes 0..3 are
/// literal, above that the code splits into a base and `extra_bits` extras.
fn prefix_value(code: u32, br: &mut Br) -> u32 {
    if code < 4 {
        return code + 1;
    }
    let extra = (code - 2) >> 1;
    let offset = (2 + (code & 1)) << extra;
    offset + br.read(extra) + 1
}

fn plane_to_distance(xsize: u32, plane: u32) -> u32 {
    if plane > 120 {
        return plane - 120;
    }
    let (xoff, yoff) = CODE_TO_PLANE[(plane - 1) as usize];
    let d = yoff * xsize as i32 + xoff;
    if d >= 1 {
        d as u32
    } else {
        1
    }
}

// ---------------------------------------------------------------------------
// Huffman code-group reading
// ---------------------------------------------------------------------------

/// Read one prefix code into `hf[ci]`, writing symbols at `base`.
/// Returns the number of symbol slots consumed, or None on malformed input.
fn read_code(
    br: &mut Br,
    hf: &mut Huffs,
    ci: usize,
    base: usize,
    alphabet: usize,
    lens: &mut [u8],
) -> Option<usize> {
    let lens = lens.get_mut(..alphabet)?;
    lens.fill(0);
    if br.read(1) == 1 {
        // Simple code: one or two symbols, each one bit long.
        let nsym = br.read(1) + 1;
        let first_is_8bit = br.read(1) == 1;
        let s0 = br.read(if first_is_8bit { 8 } else { 1 }) as usize;
        if s0 >= alphabet {
            return None;
        }
        lens[s0] = 1;
        if nsym == 2 {
            let s1 = br.read(8) as usize;
            if s1 >= alphabet {
                return None;
            }
            lens[s1] = 1;
        }
    } else {
        // The code lengths are themselves prefix-coded, by a 19-symbol code
        // whose own lengths are 3 bits each in a fixed permutation.
        let mut cl = [0u8; CODE_LENGTH_CODES];
        let ncl = br.read(4) as usize + 4;
        if ncl > CODE_LENGTH_CODES {
            return None;
        }
        for i in 0..ncl {
            cl[CL_ORDER[i]] = br.read(3) as u8;
        }
        // A private one-code table; ci == usize::MAX would be nicer but the
        // pool is indexed, so borrow slot `ci` first and rebuild it after.
        let mut clh = Huffs::new(1, CODE_LENGTH_CODES)?;
        if !clh.build(0, &cl, 0) {
            return None;
        }
        let mut max_symbol = alphabet;
        if br.read(1) == 1 {
            let nbits = 2 + 2 * br.read(3);
            max_symbol = 2 + br.read(nbits) as usize;
            if max_symbol > alphabet {
                return None;
            }
        }
        let mut sym = 0usize;
        let mut prev_len = 8u8;
        while sym < alphabet {
            if max_symbol == 0 {
                break;
            }
            max_symbol -= 1;
            if br.eof {
                return None;
            }
            let code_len = clh.decode(0, br)?;
            if code_len < 16 {
                lens[sym] = code_len as u8;
                sym += 1;
                if code_len != 0 {
                    prev_len = code_len as u8;
                }
            } else {
                let slot = (code_len - 16) as usize;
                const EXTRA: [u32; 3] = [2, 3, 7];
                const OFFSET: [u32; 3] = [3, 3, 11];
                if slot > 2 {
                    return None;
                }
                let repeat = (br.read(EXTRA[slot]) + OFFSET[slot]) as usize;
                if sym + repeat > alphabet {
                    return None;
                }
                let v = if slot == 0 { prev_len } else { 0 };
                for _ in 0..repeat {
                    lens[sym] = v;
                    sym += 1;
                }
            }
        }
    }
    if br.eof {
        return None;
    }
    if !hf.build(ci, lens, base) {
        return None;
    }
    Some(alphabet)
}

/// The five alphabets of one Huffman group, in stream order.
fn alphabet_sizes(cache_bits: u32) -> [usize; 5] {
    let cache = if cache_bits > 0 { 1usize << cache_bits } else { 0 };
    [
        (NUM_LITERAL + NUM_LENGTH) as usize + cache,
        256,
        256,
        256,
        NUM_DISTANCE,
    ]
}

// ---------------------------------------------------------------------------
// Image stream
// ---------------------------------------------------------------------------

struct Transform {
    kind: u32,
    bits: u32,
    data: U32Buf,
    xsize: u32,
}

/// Decode one VP8L image stream of `xsize` x `ysize` ARGB pixels.
/// `level0` streams may carry transforms and meta Huffman; sub-streams (the
/// transform images and the colour table) may not, which bounds recursion at
/// exactly two levels.
fn decode_stream(
    br: &mut Br,
    xsize: u32,
    ysize: u32,
    level0: bool,
    transforms: Option<&mut [Option<Transform>; 4]>,
    out_xsize: &mut u32,
) -> Option<U32Buf> {
    let mut xsize = xsize;
    // --- transforms (level 0 only) ---
    if level0 {
        let tf = transforms?;
        let mut seen = [false; 4];
        let mut order = 0usize;
        while br.read(1) == 1 {
            if br.eof {
                return None;
            }
            let kind = br.read(2);
            if seen[kind as usize] {
                return None; // each transform may appear at most once
            }
            seen[kind as usize] = true;
            let t = match kind {
                0 | 1 => {
                    let bits = br.read(3) + 2;
                    let tw = div_round_up(xsize, 1 << bits);
                    let th = div_round_up(ysize, 1 << bits);
                    let mut sub = 0u32;
                    let data = decode_stream(br, tw, th, false, None, &mut sub)?;
                    Transform { kind, bits, data, xsize: tw }
                }
                2 => Transform { kind, bits: 0, data: U32Buf::zeroed(1)?, xsize: 0 },
                _ => {
                    let ncolors = br.read(8) + 1;
                    let mut sub = 0u32;
                    let data = decode_stream(br, ncolors, 1, false, None, &mut sub)?;
                    // The colour table is delta-coded per byte against the
                    // previous entry; undo that once, here.
                    let mut table = U32Buf::zeroed(256)?;
                    let mut prev = 0u32;
                    for i in 0..ncolors as usize {
                        let v = data.get(i);
                        let e = ((v & 0xff).wrapping_add(prev & 0xff) & 0xff)
                            | ((((v >> 8) & 0xff).wrapping_add((prev >> 8) & 0xff) & 0xff) << 8)
                            | ((((v >> 16) & 0xff).wrapping_add((prev >> 16) & 0xff) & 0xff) << 16)
                            | ((((v >> 24) & 0xff).wrapping_add((prev >> 24) & 0xff) & 0xff) << 24);
                        table.set(i, e);
                        prev = e;
                    }
                    let bits = if ncolors <= 2 {
                        3
                    } else if ncolors <= 4 {
                        2
                    } else if ncolors <= 16 {
                        1
                    } else {
                        0
                    };
                    // Pixel bundling narrows the image the entropy coder sees.
                    xsize = div_round_up(xsize, 1 << bits);
                    Transform { kind, bits, data: table, xsize: ncolors }
                }
            };
            tf[order] = Some(t);
            order += 1;
            if order == 4 {
                break;
            }
        }
        if br.eof {
            return None;
        }
    }
    *out_xsize = xsize;

    // --- colour cache ---
    let mut cache_bits = 0u32;
    if br.read(1) == 1 {
        cache_bits = br.read(4);
        if !(1..=11).contains(&cache_bits) {
            return None;
        }
    }

    // --- meta Huffman ---
    let mut huff_bits = 0u32;
    let mut huff_xsize = 0u32;
    let mut entropy: Option<U32Buf> = None;
    let mut ngroups = 1usize;
    if level0 && br.read(1) == 1 {
        huff_bits = br.read(3) + 2;
        huff_xsize = div_round_up(xsize, 1 << huff_bits);
        let hy = div_round_up(ysize, 1 << huff_bits);
        let mut sub = 0u32;
        let mut e = decode_stream(br, huff_xsize, hy, false, None, &mut sub)?;
        let mut maxg = 0u32;
        for i in 0..e.len() {
            // The group index lives in the red and green bytes.
            let g = (e.get(i) >> 8) & 0xffff;
            e.set(i, g);
            if g + 1 > maxg {
                maxg = g + 1;
            }
        }
        ngroups = maxg.max(1) as usize;
        entropy = Some(e);
    }
    if br.eof {
        return None;
    }

    // --- the five prefix codes per group ---
    let sizes = alphabet_sizes(cache_bits);
    let per_group: usize = sizes.iter().sum();
    // Ceiling on table memory: a header can claim a great many groups, but each
    // one has to be spelled out in the bitstream, so this only ever rejects the
    // pathological file.
    let total_syms = ngroups.checked_mul(per_group)?;
    if total_syms > (16 << 20) {
        return None;
    }
    let mut hf = Huffs::new(ngroups * 5, total_syms)?;
    let mut lens = [0u8; 2400];
    let mut base = 0usize;
    for g in 0..ngroups {
        for k in 0..5 {
            let used = read_code(br, &mut hf, g * 5 + k, base, sizes[k], &mut lens)?;
            base += used;
        }
    }

    // --- pixels ---
    let npix = (xsize as usize).checked_mul(ysize as usize)?;
    if npix == 0 || npix > MAX_PIXELS {
        return None;
    }
    let mut px = U32Buf::zeroed(npix)?;
    let cache_size = if cache_bits > 0 { 1usize << cache_bits } else { 0 };
    let mut cache = U32Buf::zeroed(cache_size.max(1))?;
    let cache_limit = NUM_LITERAL + NUM_LENGTH + cache_size as u32;

    let mut pos = 0usize;
    let (mut col, mut row) = (0u32, 0u32);
    let mut group = 0usize;
    let hmask: u32 = if huff_bits == 0 { u32::MAX } else { (1 << huff_bits) - 1 };
    while pos < npix {
        if br.eof {
            break; // truncated: keep what decoded, like the other codecs
        }
        if let Some(e) = &entropy {
            if col & hmask == 0 {
                let gi = (row >> huff_bits) as usize * huff_xsize as usize
                    + (col >> huff_bits) as usize;
                group = (e.get(gi) as usize).min(ngroups - 1);
            }
        }
        let g5 = group * 5;
        let green = hf.decode(g5, br)?;
        if green < NUM_LITERAL {
            let red = hf.decode(g5 + 1, br)?;
            let blue = hf.decode(g5 + 2, br)?;
            let alpha = hf.decode(g5 + 3, br)?;
            let v = (alpha << 24) | (red << 16) | (green << 8) | blue;
            px.set(pos, v);
            if cache_size > 0 {
                cache.set(hash_pix(v, cache_bits), v);
            }
            pos += 1;
            col += 1;
            if col >= xsize {
                col = 0;
                row += 1;
            }
        } else if green < NUM_LITERAL + NUM_LENGTH {
            let length = prefix_value(green - NUM_LITERAL, br) as usize;
            let dsym = hf.decode(g5 + 4, br)?;
            let dcode = prefix_value(dsym, br);
            let dist = plane_to_distance(xsize, dcode) as usize;
            if dist > pos || length > npix - pos || length == 0 {
                return None;
            }
            for i in 0..length {
                let v = px.get(pos + i - dist);
                px.set(pos + i, v);
                if cache_size > 0 {
                    cache.set(hash_pix(v, cache_bits), v);
                }
            }
            pos += length;
            col += length as u32;
            while col >= xsize {
                col -= xsize;
                row += 1;
            }
        } else if green < cache_limit {
            let key = (green - NUM_LITERAL - NUM_LENGTH) as usize;
            let v = cache.get(key);
            px.set(pos, v);
            cache.set(hash_pix(v, cache_bits), v);
            pos += 1;
            col += 1;
            if col >= xsize {
                col = 0;
                row += 1;
            }
        } else {
            return None;
        }
    }
    Some(px)
}

fn hash_pix(argb: u32, bits: u32) -> usize {
    (argb.wrapping_mul(0x1e35_a7bd) >> (32 - bits)) as usize
}

// ---------------------------------------------------------------------------
// Inverse transforms
// ---------------------------------------------------------------------------

fn avg2(a: u32, b: u32) -> u32 {
    let mut r = 0u32;
    for s in [0u32, 8, 16, 24] {
        let v = (((a >> s) & 0xff) + ((b >> s) & 0xff)) / 2;
        r |= v << s;
    }
    r
}

fn clip255(v: i32) -> u32 {
    if v < 0 {
        0
    } else if v > 255 {
        255
    } else {
        v as u32
    }
}

fn add_sub_full(c0: u32, c1: u32, c2: u32) -> u32 {
    let mut r = 0u32;
    for s in [0u32, 8, 16, 24] {
        let v = ((c0 >> s) & 0xff) as i32 + ((c1 >> s) & 0xff) as i32 - ((c2 >> s) & 0xff) as i32;
        r |= clip255(v) << s;
    }
    r
}

fn add_sub_half(c0: u32, c1: u32, c2: u32) -> u32 {
    let ave = avg2(c0, c1);
    let mut r = 0u32;
    for s in [0u32, 8, 16, 24] {
        let a = ((ave >> s) & 0xff) as i32;
        let b = ((c2 >> s) & 0xff) as i32;
        r |= clip255(a + (a - b) / 2) << s;
    }
    r
}

/// Predictor 11. Picks whichever of `a` (top) / `b` (left) the gradient favours.
fn select(a: u32, b: u32, c: u32) -> u32 {
    let sub3 = |x: i32, y: i32, z: i32| (y - z).abs() - (x - z).abs();
    let d = sub3((a >> 24) as i32, (b >> 24) as i32, (c >> 24) as i32)
        + sub3(((a >> 16) & 0xff) as i32, ((b >> 16) & 0xff) as i32, ((c >> 16) & 0xff) as i32)
        + sub3(((a >> 8) & 0xff) as i32, ((b >> 8) & 0xff) as i32, ((c >> 8) & 0xff) as i32)
        + sub3((a & 0xff) as i32, (b & 0xff) as i32, (c & 0xff) as i32);
    if d <= 0 {
        a
    } else {
        b
    }
}

fn add_pixels(a: u32, b: u32) -> u32 {
    // Per-channel wrapping add of two packed ARGB values.
    let lo = (a & 0x00ff_00ff).wrapping_add(b & 0x00ff_00ff) & 0x00ff_00ff;
    let hi = ((a >> 8) & 0x00ff_00ff).wrapping_add((b >> 8) & 0x00ff_00ff) & 0x00ff_00ff;
    (hi << 8) | lo
}

fn inverse_predictor(px: &mut U32Buf, w: u32, h: u32, t: &Transform) {
    let bits = t.bits;
    let tw = t.xsize;
    for y in 0..h {
        for x in 0..w {
            let i = (y * w + x) as usize;
            let pred = if x == 0 && y == 0 {
                0xff00_0000
            } else if y == 0 {
                px.get(i - 1)
            } else if x == 0 {
                px.get(i - w as usize)
            } else {
                let mode = (t.data.get(((y >> bits) * tw + (x >> bits)) as usize) >> 8) & 0xff;
                let l = px.get(i - 1);
                let tp = px.get(i - w as usize);
                let tl = px.get(i - w as usize - 1);
                // TR is literally `top[+1]` in the flat buffer. At the last
                // column that is the FIRST pixel of the current row (already
                // written this pass), not a clamp of the last column -- the
                // reference decoder reads the contiguous buffer and an encoder
                // predicted against exactly that, so clamping here would be a
                // mismatch on every image whose last column uses predictor 3/5/9.
                let tr = px.get(i - w as usize + 1);
                match mode {
                    0 => 0xff00_0000,
                    1 => l,
                    2 => tp,
                    3 => tr,
                    4 => tl,
                    5 => avg2(avg2(l, tr), tp),
                    6 => avg2(l, tl),
                    7 => avg2(l, tp),
                    8 => avg2(tl, tp),
                    9 => avg2(tp, tr),
                    10 => avg2(avg2(l, tl), avg2(tp, tr)),
                    11 => select(tp, l, tl),
                    12 => add_sub_full(l, tp, tl),
                    13 => add_sub_half(l, tp, tl),
                    _ => l,
                }
            };
            px.set(i, add_pixels(px.get(i), pred));
        }
    }
}

fn inverse_cross_color(px: &mut U32Buf, w: u32, h: u32, t: &Transform) {
    let bits = t.bits;
    let tw = t.xsize;
    for y in 0..h {
        for x in 0..w {
            let i = (y * w + x) as usize;
            let m = t.data.get(((y >> bits) * tw + (x >> bits)) as usize);
            let g2r = (m & 0xff) as u8 as i8 as i32;
            let g2b = ((m >> 8) & 0xff) as u8 as i8 as i32;
            let r2b = ((m >> 16) & 0xff) as u8 as i8 as i32;
            let argb = px.get(i);
            let green = ((argb >> 8) & 0xff) as u8 as i8 as i32;
            let mut nr = ((argb >> 16) & 0xff) as i32;
            let mut nb = (argb & 0xff) as i32;
            nr += (g2r * green) >> 5;
            nr &= 0xff;
            nb += (g2b * green) >> 5;
            nb += (r2b * (nr as u8 as i8 as i32)) >> 5;
            nb &= 0xff;
            px.set(i, (argb & 0xff00_ff00) | ((nr as u32) << 16) | nb as u32);
        }
    }
}

fn inverse_subtract_green(px: &mut U32Buf) {
    for i in 0..px.len() {
        let v = px.get(i);
        let g = (v >> 8) & 0xff;
        let r = ((v >> 16) & 0xff).wrapping_add(g) & 0xff;
        let b = (v & 0xff).wrapping_add(g) & 0xff;
        px.set(i, (v & 0xff00_ff00) | (r << 16) | b);
    }
}

/// Colour indexing: `src` is the narrow bundled image, output is `w` wide.
fn inverse_color_index(src: &U32Buf, sw: u32, w: u32, h: u32, t: &Transform) -> Option<U32Buf> {
    let mut out = U32Buf::zeroed((w as usize).checked_mul(h as usize)?)?;
    let bits = t.bits;
    let bpp = 8 >> bits; // bits per index within the green byte
    let per_byte = 1u32 << bits;
    let mask = (1u32 << bpp) - 1;
    let ncolors = t.xsize;
    for y in 0..h {
        let mut packed = 0u32;
        for x in 0..w {
            if x % per_byte == 0 {
                packed = (src.get((y * sw + x / per_byte) as usize) >> 8) & 0xff;
            }
            let idx = packed & mask;
            packed >>= bpp;
            // An index past the table is transparent black, not an error --
            // that is what the reference decoder's expanded table produces.
            let v = if idx < ncolors { t.data.get(idx as usize) } else { 0 };
            out.set((y * w + x) as usize, v);
        }
    }
    Some(out)
}

// ---------------------------------------------------------------------------
// VP8L entry point
// ---------------------------------------------------------------------------

/// Decode a VP8L image STREAM (no 5-byte header) of `w` x `h` into ARGB, undoing
/// its transforms. Shared by the lossless image path and by the alpha plane of
/// a lossy image, whose compressed form is exactly this with the header taken
/// off and the answer carried in the green channel.
fn vp8l_pixels(br: &mut Br, w: u32, h: u32) -> Option<U32Buf> {
    let mut tf: [Option<Transform>; 4] = [None, None, None, None];
    let mut sub_w = w;
    let mut px = decode_stream(br, w, h, true, Some(&mut tf), &mut sub_w)?;

    // Transforms undo in the reverse of the order they were read.
    let mut cur_w = sub_w;
    for slot in (0..4).rev() {
        let t = match &tf[slot] {
            None => continue,
            Some(t) => t,
        };
        match t.kind {
            0 => inverse_predictor(&mut px, cur_w, h, t),
            1 => inverse_cross_color(&mut px, cur_w, h, t),
            2 => inverse_subtract_green(&mut px),
            _ => {
                px = inverse_color_index(&px, cur_w, w, h, t)?;
                cur_w = w;
            }
        }
    }
    if cur_w != w {
        return None;
    }
    Some(px)
}

// ---------------------------------------------------------------------------
// ALPH: the alpha plane of a LOSSY WebP.
//
// A lossy WebP carries no alpha of its own -- VP8 has no alpha channel -- so a
// transparent one is a VP8 frame plus a separate ALPH chunk holding an 8-bit
// plane. Without this, such a file decodes with correct colour and alpha 255
// everywhere: a logo drawn as an opaque rectangle, which is worse than a
// broken-image box because nothing looks broken.
//
// Header byte: bits 1-0 compression, 3-2 filter, 5-4 pre-processing, 7-6
// reserved. Pre-processing describes what the ENCODER did to the plane before
// compressing it (level reduction); the decoder has nothing to undo and
// libwebp ignores it, so the field is read and dropped rather than refused.
// ---------------------------------------------------------------------------

fn gradient_pred(a: u8, b: u8, c: u8) -> u8 {
    let g = a as i32 + b as i32 - c as i32;
    if g < 0 { 0 } else if g > 255 { 255 } else { g as u8 }
}

/// Undo the per-row spatial filter, in place. `prev` is the already-unfiltered
/// row above; for the first row there is none and every method degenerates to
/// horizontal with a zero seed, which is libwebp's behaviour and not an
/// approximation of it.
fn alpha_unfilter(method: u8, plane: &mut [u8], w: usize, h: usize) {
    if method == 0 {
        return;
    }
    for y in 0..h {
        let row = y * w;
        if y == 0 || method == 1 {
            // horizontal (and every method's first row)
            let mut pred = if y == 0 { 0u8 } else { plane[row - w] };
            for x in 0..w {
                let v = plane[row + x].wrapping_add(pred);
                plane[row + x] = v;
                pred = v;
            }
        } else if method == 2 {
            for x in 0..w {
                plane[row + x] = plane[row + x].wrapping_add(plane[row - w + x]);
            }
        } else {
            let mut left = plane[row - w];
            let mut top_left = left;
            for x in 0..w {
                let top = plane[row - w + x];
                left = plane[row + x].wrapping_add(gradient_pred(left, top, top_left));
                top_left = top;
                plane[row + x] = left;
            }
        }
    }
}

fn decode_alpha_plane(alph: &[u8], w: usize, h: usize) -> Option<Buf> {
    let hdr = *alph.first()?;
    if (hdr >> 6) != 0 {
        return None; // reserved bits set
    }
    let compression = hdr & 3;
    let filter = (hdr >> 2) & 3;
    let data = alph.get(1..)?;
    let n = w.checked_mul(h)?;
    let mut plane = Buf::zeroed(n)?;
    match compression {
        0 => {
            let src = data.get(..n)?;
            plane.as_mut().copy_from_slice(src);
        }
        1 => {
            let mut br = Br::new(data);
            let px = vp8l_pixels(&mut br, w as u32, h as u32)?;
            let m = plane.as_mut();
            for i in 0..n {
                m[i] = (px.get(i) >> 8) as u8; // alpha rides in green
            }
        }
        _ => return None,
    }
    alpha_unfilter(filter, plane.as_mut(), w, h);
    Some(plane)
}

/// The ALPH chunk's payload, if the container has one.
fn find_alph(p: &[u8]) -> Option<(usize, usize)> {
    let mut i = 12usize;
    while i + 8 <= p.len() {
        let sz = le32(p, i + 4)? as usize;
        let body = i + 8;
        if &p[i..i + 4] == b"ALPH" {
            let end = body.checked_add(sz)?.min(p.len());
            return Some((body, end));
        }
        i = body.checked_add(sz)?.checked_add(sz & 1)?;
    }
    None
}

fn decode_vp8l(d: &[u8]) -> Option<(i32, i32, Buf)> {
    if d.first() != Some(&0x2f) {
        return None;
    }
    let mut br = Br::new(&d[1..]);
    let w = br.read(14) + 1;
    let h = br.read(14) + 1;
    let _alpha_used = br.read(1);
    if br.read(3) != 0 || br.eof {
        return None;
    }
    if rgba_size(w as i32, h as i32).is_none() {
        return None;
    }

    let px = vp8l_pixels(&mut br, w, h)?;

    let mut buf = Buf::new(rgba_size(w as i32, h as i32)?)?;
    {
        let o = buf.as_mut();
        for i in 0..(w as usize * h as usize) {
            let v = px.get(i);
            o[i * 4] = (v >> 16) as u8;
            o[i * 4 + 1] = (v >> 8) as u8;
            o[i * 4 + 2] = v as u8;
            o[i * 4 + 3] = (v >> 24) as u8;
        }
    }
    Some((w as i32, h as i32, buf))
}

// ---------------------------------------------------------------------------
// RIFF container
// ---------------------------------------------------------------------------

/// Walk the WebP chunk list and hand back the first codec chunk found.
/// Returns (fourcc, payload range).
fn find_codec_chunk(p: &[u8]) -> Option<(&[u8; 4], usize, usize)> {
    let mut i = 12usize; // "RIFF" + size + "WEBP"
    while i + 8 <= p.len() {
        let mut fcc = [0u8; 4];
        fcc.copy_from_slice(&p[i..i + 4]);
        let sz = le32(p, i + 4)? as usize;
        let body = i + 8;
        if sz > p.len() - body.min(p.len()) {
            // Truncated final chunk: hand over what is actually present so a
            // partially received image can still decode its header.
            if &fcc == b"VP8L" || &fcc == b"VP8 " {
                return Some((leak(fcc), body, p.len()));
            }
            return None;
        }
        if &fcc == b"VP8L" || &fcc == b"VP8 " {
            return Some((leak(fcc), body, body + sz));
        }
        i = body + sz + (sz & 1);
    }
    None
}

/// The fourcc is compared, never stored; this hands back a 'static tag so the
/// caller can match on it without borrowing the input.
fn leak(f: [u8; 4]) -> &'static [u8; 4] {
    if &f == b"VP8L" {
        b"VP8L"
    } else {
        b"VP8 "
    }
}

fn decode_webp(p: &[u8]) -> Option<(i32, i32, Buf)> {
    let (fcc, a, b) = find_codec_chunk(p)?;
    let body = p.get(a..b)?;
    if fcc == b"VP8L" {
        return decode_vp8l(body);
    }
    let (w, h, mut rgba) = crate::vp8_frame::decode_vp8_keyframe(body)?;
    if let Some((a0, a1)) = find_alph(p) {
        // A failed alpha plane is NOT a failed image: the colour is already
        // decoded and correct, and dropping it would replace a wrong alpha
        // channel with no picture at all. It stays opaque and says nothing,
        // which is the same thing every other decoder in this tree does with a
        // trailing chunk it cannot read.
        if let Some(plane) = decode_alpha_plane(p.get(a0..a1)?, w as usize, h as usize) {
            let src = plane.as_ref();
            let dst = rgba.as_mut();
            for i in 0..(w as usize * h as usize) {
                dst[i * 4 + 3] = src[i];
            }
        }
    }
    Some((w, h, rgba))
}

// ---- C ABI ----

#[no_mangle]
pub extern "C" fn webp_detect(p: *const u8, n: i32) -> i32 {
    if p.is_null() || n < 16 {
        return 0;
    }
    let s = unsafe { core::slice::from_raw_parts(p, 16) };
    i32::from(&s[0..4] == b"RIFF" && &s[8..12] == b"WEBP")
}

#[no_mangle]
pub extern "C" fn webp_decode(p: *const u8, n: i32, out: *mut Image) -> i32 {
    if p.is_null() || out.is_null() || n < 16 {
        return -1;
    }
    let input = unsafe { core::slice::from_raw_parts(p, n as usize) };
    match decode_webp(input) {
        Some((w, h, buf)) => {
            unsafe {
                (*out).w = w;
                (*out).h = h;
                (*out).rgba = buf.into_raw();
            }
            0
        }
        None => -1,
    }
}

#[no_mangle]
pub extern "C" fn webp_register() {
    unsafe { img_register(webp_detect, webp_decode) };
}
