//! VP8 reconstruction: intra prediction, coefficient decoding, the macroblock
//! loop, the loop filter, and YUV -> RGBA. The state and transforms live in
//! `vp8_dec.rs`; the entry point `decode_vp8_keyframe` is at the bottom here.

use crate::imgbuf::Buf;
use crate::vp8::*;
use crate::vp8_dec::*;
use crate::vp8_tables::*;

#[inline]
pub(crate) fn avg3(x: u8, y: u8, z: u8) -> u8 {
    ((x as u32 + 2 * y as u32 + z as u32 + 2) >> 2) as u8
}

#[inline]
pub(crate) fn avg2(x: u8, y: u8) -> u8 {
    ((x as u32 + y as u32 + 1) >> 1) as u8
}

// ---------------------------------------------------------------------------
// Whole-block intra prediction (16x16 luma, 8x8 chroma). RFC 6386 §12.2.
//
// Only DC_PRED asks whether its neighbours exist. V, H and TM read the border
// unconditionally, which is exactly why the border rows carry 127 and the
// border columns 129: those values ARE the prediction at the frame edge, not
// a placeholder for one.
// ---------------------------------------------------------------------------

pub(crate) fn pred_block(pl: &mut [u8], off: usize, stride: usize, size: usize, mode: usize,
              left_ok: bool, up_ok: bool) {
    match mode {
        V_PRED => {
            for y in 0..size {
                for x in 0..size {
                    pl[off + y * stride + x] = pl[off - stride + x];
                }
            }
        }
        H_PRED => {
            for y in 0..size {
                let l = pl[off + y * stride - 1];
                for x in 0..size {
                    pl[off + y * stride + x] = l;
                }
            }
        }
        TM_PRED => {
            let p = pl[off - stride - 1] as i32;
            for y in 0..size {
                let l = pl[off + y * stride - 1] as i32;
                for x in 0..size {
                    let a = pl[off - stride + x] as i32;
                    pl[off + y * stride + x] = clamp255(l + a - p);
                }
            }
        }
        _ => {
            // DC_PRED -- the ONLY mode that asks whether its neighbours exist.
            // NEGATIVE CONTROL vp8-dc-always-avail says they always do, which
            // averages the 127/129 border into the DC of every edge block.
            let (left_ok, up_ok) = if cfg!(feature = "vp8-dc-always-avail") {
                (true, true)
            } else {
                (left_ok, up_ok)
            };
            let mut sum = 0u32;
            let mut n = 0u32;
            if up_ok {
                for x in 0..size {
                    sum += pl[off - stride + x] as u32;
                }
                n += size as u32;
            }
            if left_ok {
                for y in 0..size {
                    sum += pl[off + y * stride - 1] as u32;
                }
                n += size as u32;
            }
            let dc = if n == 0 {
                128u8
            } else {
                // n is 16/32 (luma) or 8/16 (chroma): round by n/2, divide by n.
                ((sum + n / 2) / n) as u8
            };
            for y in 0..size {
                for x in 0..size {
                    pl[off + y * stride + x] = dc;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 4x4 subblock prediction, RFC 6386 §12.3, transcribed from the document's own
// `subblock_intra_predict`. Its naming is kept: A[-1] == L[-1] == P, and the
// nine-entry E array runs from L[3] up through P and along to A[3].
//
// (The RFC's listing has a typo in B_HD_PRED -- `svg2p` for `avg2p` -- which
// is why the transcription is by structure rather than by copy.)
// ---------------------------------------------------------------------------

/// `a[0]` = P, `a[1..9]` = A[0..8]; `lv[0]` = P, `lv[1..5]` = L[0..4].
pub(crate) fn pred4(b: &mut [[u8; 4]; 4], a: &[u8; 9], lv: &[u8; 5], mode: usize) {
    // avg3p(A + c) and avg2p(A + c) in the RFC's indexing.
    let a3a = |c: usize| avg3(a[c], a[c + 1], a[c + 2]);
    let a2a = |c: usize| avg2(a[c + 1], a[c + 2]);
    // E[0..9]
    let e: [u8; 9] = [lv[4], lv[3], lv[2], lv[1], a[0], a[1], a[2], a[3], a[4]];
    let e3 = |i: usize| avg3(e[i - 1], e[i], e[i + 1]);
    let e2 = |i: usize| avg2(e[i], e[i + 1]);
    let l3 = |r: usize| avg3(lv[r], lv[r + 1], lv[r + 2]); // avg3p(L + r)

    match mode {
        B_DC_PRED => {
            let mut v = 4u32;
            for i in 0..4 {
                v += a[i + 1] as u32 + lv[i + 1] as u32;
            }
            let v = (v >> 3) as u8;
            for r in 0..4 {
                for c in 0..4 {
                    b[r][c] = v;
                }
            }
        }
        B_TM_PRED => {
            let p = a[0] as i32;
            for r in 0..4 {
                for c in 0..4 {
                    b[r][c] = clamp255(lv[r + 1] as i32 + a[c + 1] as i32 - p);
                }
            }
        }
        B_VE_PRED => {
            for c in 0..4 {
                let v = a3a(c);
                for r in 0..4 {
                    b[r][c] = v;
                }
            }
        }
        B_HE_PRED => {
            // The bottom row is exceptional: L[4] does not exist.
            let bottom = avg3(lv[3], lv[4], lv[4]);
            for c in 0..4 {
                b[3][c] = bottom;
            }
            for r in (0..3).rev() {
                let v = l3(r);
                for c in 0..4 {
                    b[r][c] = v;
                }
            }
        }
        B_LD_PRED => {
            b[0][0] = a3a(1);
            b[0][1] = a3a(2);
            b[1][0] = b[0][1];
            b[0][2] = a3a(3);
            b[1][1] = b[0][2];
            b[2][0] = b[0][2];
            b[0][3] = a3a(4);
            b[1][2] = b[0][3];
            b[2][1] = b[0][3];
            b[3][0] = b[0][3];
            b[1][3] = a3a(5);
            b[2][2] = b[1][3];
            b[3][1] = b[1][3];
            b[2][3] = a3a(6);
            b[3][2] = b[2][3];
            b[3][3] = avg3(a[7], a[8], a[8]); // A[8] does not exist
        }
        B_RD_PRED => {
            b[3][0] = e3(1);
            b[3][1] = e3(2);
            b[2][0] = b[3][1];
            b[3][2] = e3(3);
            b[2][1] = b[3][2];
            b[1][0] = b[3][2];
            b[3][3] = e3(4);
            b[2][2] = b[3][3];
            b[1][1] = b[3][3];
            b[0][0] = b[3][3];
            b[2][3] = e3(5);
            b[1][2] = b[2][3];
            b[0][1] = b[2][3];
            b[1][3] = e3(6);
            b[0][2] = b[1][3];
            b[0][3] = e3(7);
        }
        B_VR_PRED => {
            b[3][0] = e3(2);
            b[2][0] = e3(3);
            b[3][1] = e3(4);
            b[1][0] = b[3][1];
            b[2][1] = e2(4);
            b[0][0] = b[2][1];
            b[3][2] = e3(5);
            b[1][1] = b[3][2];
            b[2][2] = e2(5);
            b[0][1] = b[2][2];
            b[3][3] = e3(6);
            b[1][2] = b[3][3];
            b[2][3] = e2(6);
            b[0][2] = b[2][3];
            b[1][3] = e3(7);
            b[0][3] = e2(7);
        }
        B_VL_PRED => {
            b[0][0] = a2a(0);
            b[1][0] = a3a(1);
            b[2][0] = a2a(1);
            b[0][1] = b[2][0];
            b[1][1] = a3a(2);
            b[3][0] = b[1][1];
            b[2][1] = a2a(2);
            b[0][2] = b[2][1];
            b[3][1] = a3a(3);
            b[1][2] = b[3][1];
            b[2][2] = a2a(3);
            b[0][3] = b[2][2];
            b[3][2] = a3a(4);
            b[1][3] = b[3][2];
            // The last two break the pattern, per the RFC's own note.
            b[2][3] = a3a(5);
            b[3][3] = a3a(6);
        }
        B_HD_PRED => {
            b[3][0] = e2(0);
            b[3][1] = e3(1);
            b[2][0] = e2(1);
            b[3][2] = b[2][0];
            b[2][1] = e3(2);
            b[3][3] = b[2][1];
            b[2][2] = e2(2);
            b[1][0] = b[2][2];
            b[2][3] = e3(3);
            b[1][1] = b[2][3];
            b[1][2] = e2(3);
            b[0][0] = b[1][2];
            b[1][3] = e3(4);
            b[0][1] = b[1][3];
            b[0][2] = e3(5);
            b[0][3] = e3(6);
        }
        _ => {
            // B_HU_PRED
            b[0][0] = avg2(lv[1], lv[2]);
            b[0][1] = avg3(lv[1], lv[2], lv[3]);
            b[0][2] = avg2(lv[2], lv[3]);
            b[1][0] = b[0][2];
            b[0][3] = avg3(lv[2], lv[3], lv[4]);
            b[1][1] = b[0][3];
            b[1][2] = avg2(lv[3], lv[4]);
            b[2][0] = b[1][2];
            b[1][3] = avg3(lv[3], lv[4], lv[4]);
            b[2][1] = b[1][3];
            let last = lv[4];
            b[2][2] = last;
            b[2][3] = last;
            b[3][0] = last;
            b[3][1] = last;
            b[3][2] = last;
            b[3][3] = last;
        }
    }
}

// ---------------------------------------------------------------------------
// Coefficient tokens (RFC 6386 §13).
// ---------------------------------------------------------------------------

const DCT_EOB: i32 = 11;

/// Decode one 4x4 block's coefficients into `out` (natural order, dequantised).
/// Returns the coefficient position at which the block ended; the caller's
/// non-zero context is `end > first`, which is libwebp's rule and matters:
/// a block whose tokens are sixteen explicit zeros ends at 16 and therefore
/// counts as non-zero for its neighbours even though it wrote nothing.
pub(crate) fn decode_coeffs(bd: &mut Bool, probs: &[[[u8; 11]; 3]; 8], ctx0: usize, first: usize,
                 dq: [i32; 2], out: &mut [i32; 16]) -> usize {
    let mut n = first;
    let mut c = ctx0;
    let mut skip_eob = false;
    while n < 16 {
        let band = COEFF_BANDS[n];
        let p = &probs[band][c];
        let tok = if skip_eob {
            bd.get_tree_from(&COEFF_TREE, p, 2)
        } else {
            bd.get_tree(&COEFF_TREE, p)
        };
        if tok == DCT_EOB {
            break;
        }
        if tok == 0 {
            c = 0;
            skip_eob = true; // EOB cannot directly follow a zero
            n += 1;
            continue;
        }
        skip_eob = false;
        let mut val: i32;
        if tok <= 4 {
            val = tok;
            c = if tok == 1 { 1 } else { 2 };
        } else {
            let cat = (tok - 5) as usize;
            let mut extra = 0i32;
            for j in 0..CAT_LEN[cat] {
                extra += extra + bd.get(CAT_PROBS[cat][j]) as i32;
            }
            val = CAT_BASE[cat] + extra;
            c = 2;
        }
        if bd.get_bit() != 0 {
            val = -val;
        }
        out[ZIGZAG[n]] = val * if n == 0 { dq[0] } else { dq[1] };
        n += 1;
    }
    n
}

// ---------------------------------------------------------------------------
// The loop filter (RFC 6386 §15). Signed 8-bit arithmetic throughout: the
// specification's own c()/u2s()/s2u() helpers, kept as such because the
// saturation is load-bearing, not incidental.
// ---------------------------------------------------------------------------

#[inline]
pub(crate) fn c8(v: i32) -> i32 {
    if v < -128 { -128 } else if v > 127 { 127 } else { v }
}
#[inline]
pub(crate) fn u2s(v: u8) -> i32 {
    v as i32 - 128
}
#[inline]
pub(crate) fn s2u(v: i32) -> u8 {
    (c8(v) + 128) as u8
}
#[inline]
pub(crate) fn absd(a: u8, b: u8) -> i32 {
    (a as i32 - b as i32).abs()
}

/// The shared 4-tap adjustment. Returns the value the outer taps need.
pub(crate) fn common_adjust(use_outer: bool, p: &mut [u8], i1: usize, i0: usize,
                 j0: usize, j1: usize) -> i32 {
    let p1 = u2s(p[i1]);
    let p0 = u2s(p[i0]);
    let q0 = u2s(p[j0]);
    let q1 = u2s(p[j1]);
    let a = c8((if use_outer { c8(p1 - q1) } else { 0 }) + 3 * (q0 - p0));
    let f = c8(a + 4) >> 3;
    let e = c8(a + 3) >> 3;
    p[j0] = s2u(q0 - f);
    p[i0] = s2u(p0 + e);
    f
}

pub(crate) fn filter_yes(interior: i32, edge: i32, p: &[u8], idx: [usize; 8]) -> bool {
    let v: [u8; 8] = [
        p[idx[0]], p[idx[1]], p[idx[2]], p[idx[3]],
        p[idx[4]], p[idx[5]], p[idx[6]], p[idx[7]],
    ];
    (absd(v[3], v[4]) * 2 + absd(v[2], v[5]) / 2) <= edge
        && absd(v[0], v[1]) <= interior
        && absd(v[1], v[2]) <= interior
        && absd(v[2], v[3]) <= interior
        && absd(v[7], v[6]) <= interior
        && absd(v[6], v[5]) <= interior
        && absd(v[5], v[4]) <= interior
}

pub(crate) fn hev(thresh: i32, p: &[u8], i1: usize, i0: usize, j0: usize, j1: usize) -> bool {
    absd(p[i1], p[i0]) > thresh || absd(p[j1], p[j0]) > thresh
}

/// idx = [p3 p2 p1 p0 q0 q1 q2 q3] as plane indices.
pub(crate) fn subblock_filter(hev_t: i32, interior: i32, edge: i32, p: &mut [u8], idx: [usize; 8]) {
    if !filter_yes(interior, edge, p, idx) {
        return;
    }
    let hv = hev(hev_t, p, idx[2], idx[3], idx[4], idx[5]);
    let p1 = u2s(p[idx[2]]);
    let q1 = u2s(p[idx[5]]);
    let a = (common_adjust(hv, p, idx[2], idx[3], idx[4], idx[5]) + 1) >> 1;
    if !hv {
        p[idx[5]] = s2u(q1 - a);
        p[idx[2]] = s2u(p1 + a);
    }
}

pub(crate) fn mb_filter(hev_t: i32, interior: i32, edge: i32, p: &mut [u8], idx: [usize; 8]) {
    if !filter_yes(interior, edge, p, idx) {
        return;
    }
    if hev(hev_t, p, idx[2], idx[3], idx[4], idx[5]) {
        common_adjust(true, p, idx[2], idx[3], idx[4], idx[5]);
        return;
    }
    let p2 = u2s(p[idx[1]]);
    let p1 = u2s(p[idx[2]]);
    let p0 = u2s(p[idx[3]]);
    let q0 = u2s(p[idx[4]]);
    let q1 = u2s(p[idx[5]]);
    let q2 = u2s(p[idx[6]]);
    let w = c8(c8(p1 - q1) + 3 * (q0 - p0));
    let a = c8((27 * w + 63) >> 7);
    p[idx[4]] = s2u(q0 - a);
    p[idx[3]] = s2u(p0 + a);
    let a = c8((18 * w + 63) >> 7);
    p[idx[5]] = s2u(q1 - a);
    p[idx[2]] = s2u(p1 + a);
    let a = c8((9 * w + 63) >> 7);
    p[idx[6]] = s2u(q2 - a);
    p[idx[1]] = s2u(p2 + a);
}

pub(crate) fn simple_filter(edge: i32, p: &mut [u8], i1: usize, i0: usize, j0: usize, j1: usize) {
    if (absd(p[i0], p[j0]) * 2 + absd(p[i1], p[j1]) / 2) <= edge {
        common_adjust(true, p, i1, i0, j0, j1);
    }
}

/// Eight plane indices centred on a vertical edge at column `x` (filtering
/// across it, i.e. the taps run horizontally).
#[inline]
pub(crate) fn vidx(base: usize, _stride: usize) -> [usize; 8] {
    [base - 4, base - 3, base - 2, base - 1, base, base + 1, base + 2, base + 3]
}

/// Eight plane indices centred on a horizontal edge at row `y`.
#[inline]
pub(crate) fn hidx(base: usize, s: usize) -> [usize; 8] {
    [
        base - 4 * s, base - 3 * s, base - 2 * s, base - s,
        base, base + s, base + 2 * s, base + 3 * s,
    ]
}
