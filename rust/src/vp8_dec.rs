//! VP8 key-frame reconstruction: headers, modes, coefficients, prediction,
//! transforms and the loop filter. `vp8.rs` holds the bool decoder, the
//! quantiser tables and the uncompressed frame header; `vp8_tables.rs` holds
//! the probability tables, generated from RFC 6386.
//!
//! ORDER OF OPERATIONS, and the two places it is easy to get wrong:
//!
//!  1. The whole frame is reconstructed UNFILTERED, then the loop filter runs
//!     as a second pass. That is not an optimisation, it is required: intra
//!     prediction reads its neighbours' *unfiltered* samples. A decoder that
//!     filters each macroblock as it finishes feeds filtered pixels into the
//!     next row's predictor and drifts a little more with every row.
//!  2. A B_PRED subblock on the right edge of its macroblock takes its
//!     above-right samples from the row above the MACROBLOCK, for all four
//!     subblock rows -- not from the reconstructed subblock diagonally above
//!     it. This is VP8's most-reimplemented bug; `top_right` below is captured
//!     once per macroblock precisely so the wrong pixels are not reachable.

use crate::imgbuf::Buf;
use crate::vp8::*;
use crate::vp8_tables::*;

// Intra 16x16 / chroma modes.
pub(crate) const DC_PRED: usize = 0;
pub(crate) const V_PRED: usize = 1;
pub(crate) const H_PRED: usize = 2;
pub(crate) const TM_PRED: usize = 3;
pub(crate) const B_PRED: usize = 4;

// 4x4 submodes.
pub(crate) const B_DC_PRED: usize = 0;
pub(crate) const B_TM_PRED: usize = 1;
pub(crate) const B_VE_PRED: usize = 2;
pub(crate) const B_HE_PRED: usize = 3;
pub(crate) const B_LD_PRED: usize = 4;
pub(crate) const B_RD_PRED: usize = 5;
pub(crate) const B_VR_PRED: usize = 6;
pub(crate) const B_VL_PRED: usize = 7;
pub(crate) const B_HD_PRED: usize = 8;
pub(crate) const B_HU_PRED: usize = 9;

/// The 4x4 submode a whole-macroblock mode implies, for the purpose of a
/// neighbouring B_PRED block's context. A 16x16 V_PRED macroblock looks like
/// sixteen B_VE_PRED blocks to its neighbour.
pub(crate) fn bmode_of(ymode: usize) -> usize {
    match ymode {
        V_PRED => B_VE_PRED,
        H_PRED => B_HE_PRED,
        TM_PRED => B_TM_PRED,
        _ => B_DC_PRED,
    }
}

#[inline]
pub(crate) fn clamp255(v: i32) -> u8 {
    if v < 0 { 0 } else if v > 255 { 255 } else { v as u8 }
}

// ---------------------------------------------------------------------------
// Planes. One allocation holding Y, U and V, each with a one-pixel top border
// (filled 127) and a one-pixel left border (filled 129) -- VP8's rule for the
// samples a predictor reads outside the frame -- plus four bytes of right
// spill on every row so an above-right read is always in bounds.
// ---------------------------------------------------------------------------

pub(crate) struct Planes {
    pub(crate) buf: Buf,
    pub(crate) ys: usize,
    pub(crate) cs: usize,
    pub(crate) yb: usize, // index of luma pixel (0,0)
    pub(crate) ub: usize,
    pub(crate) vb: usize,
    pub(crate) yw: usize,
    pub(crate) yh: usize,
    pub(crate) cw: usize,
    pub(crate) ch: usize,
}

impl Planes {
    pub(crate) fn new(mb_w: usize, mb_h: usize) -> Option<Planes> {
        let yw = mb_w * 16;
        let yh = mb_h * 16;
        let cw = mb_w * 8;
        let ch = mb_h * 8;
        let ys = yw + 1 + 4;
        let cs = cw + 1 + 4;
        let ysz = ys.checked_mul(yh + 1)?;
        let csz = cs.checked_mul(ch + 1)?;
        let total = ysz.checked_add(csz.checked_mul(2)?)?;
        let mut buf = Buf::zeroed(total)?;
        {
            let m = buf.as_mut();
            // Top border row = 127 across the whole stride (this also covers
            // the above-left corner and the four above-right spill bytes).
            for i in 0..ys {
                m[i] = 127;
            }
            for i in 0..cs {
                m[ysz + i] = 127;
                m[ysz + csz + i] = 127;
            }
            // Left border column = 129 on every image row.
            for r in 1..=yh {
                m[r * ys] = 129;
            }
            for r in 1..=ch {
                m[ysz + r * cs] = 129;
                m[ysz + csz + r * cs] = 129;
            }
        }
        Some(Planes {
            buf,
            ys,
            cs,
            yb: ys + 1,
            ub: ysz + cs + 1,
            vb: ysz + csz + cs + 1,
            yw,
            yh,
            cw,
            ch,
        })
    }
}

// ---------------------------------------------------------------------------
// Transforms (RFC 6386 §14.3). Exact integer arithmetic; the constants are the
// specification's, and the output is ADDED to the prediction and clamped.
// ---------------------------------------------------------------------------

const COSPI8SQRT2MINUS1: i32 = 20091;
const SINPI8SQRT2: i32 = 35468;

/// Inverse WHT: spreads the second-order block's 16 values into the DC slots
/// of the sixteen luma subblocks.
pub(crate) fn iwht4x4(input: &[i32; 16], dc_out: &mut [i32; 16]) {
    let mut tmp = [0i32; 16];
    for i in 0..4 {
        let a1 = input[i] + input[12 + i];
        let b1 = input[4 + i] + input[8 + i];
        let c1 = input[4 + i] - input[8 + i];
        let d1 = input[i] - input[12 + i];
        tmp[i] = a1 + b1;
        tmp[4 + i] = c1 + d1;
        tmp[8 + i] = a1 - b1;
        tmp[12 + i] = d1 - c1;
    }
    for i in 0..4 {
        let r = i * 4;
        let a1 = tmp[r] + tmp[r + 3];
        let b1 = tmp[r + 1] + tmp[r + 2];
        let c1 = tmp[r + 1] - tmp[r + 2];
        let d1 = tmp[r] - tmp[r + 3];
        dc_out[r] = (a1 + b1 + 3) >> 3;
        dc_out[r + 1] = (c1 + d1 + 3) >> 3;
        dc_out[r + 2] = (a1 - b1 + 3) >> 3;
        dc_out[r + 3] = (d1 - c1 + 3) >> 3;
    }
}

/// Inverse DCT of one 4x4 block, added in place to the plane at `off`.
pub(crate) fn idct4x4_add(coeff: &[i32; 16], plane: &mut [u8], off: usize, stride: usize) {
    let mut tmp = [0i32; 16];
    for i in 0..4 {
        let a1 = coeff[i] + coeff[8 + i];
        let b1 = coeff[i] - coeff[8 + i];
        let t1 = (coeff[4 + i] * SINPI8SQRT2) >> 16;
        let t2 = coeff[12 + i] + ((coeff[12 + i] * COSPI8SQRT2MINUS1) >> 16);
        let c1 = t1 - t2;
        let t1 = coeff[4 + i] + ((coeff[4 + i] * COSPI8SQRT2MINUS1) >> 16);
        let t2 = (coeff[12 + i] * SINPI8SQRT2) >> 16;
        let d1 = t1 + t2;
        tmp[i] = a1 + d1;
        tmp[12 + i] = a1 - d1;
        tmp[4 + i] = b1 + c1;
        tmp[8 + i] = b1 - c1;
    }
    for i in 0..4 {
        let r = i * 4;
        let a1 = tmp[r] + tmp[r + 2];
        let b1 = tmp[r] - tmp[r + 2];
        let t1 = (tmp[r + 1] * SINPI8SQRT2) >> 16;
        let t2 = tmp[r + 3] + ((tmp[r + 3] * COSPI8SQRT2MINUS1) >> 16);
        let c1 = t1 - t2;
        let t1 = tmp[r + 1] + ((tmp[r + 1] * COSPI8SQRT2MINUS1) >> 16);
        let t2 = (tmp[r + 3] * SINPI8SQRT2) >> 16;
        let d1 = t1 + t2;
        let o = off + i * stride;
        let vals = [
            (a1 + d1 + 4) >> 3,
            (b1 + c1 + 4) >> 3,
            (b1 - c1 + 4) >> 3,
            (a1 - d1 + 4) >> 3,
        ];
        for (k, v) in vals.iter().enumerate() {
            if let Some(p) = plane.get_mut(o + k) {
                *p = clamp255(*p as i32 + *v);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The decoder.
// ---------------------------------------------------------------------------

pub(crate) struct Mb {
    pub(crate) ymode: u8,
    pub(crate) uvmode: u8,
    pub(crate) bmodes: [u8; 16],
    pub(crate) seg: u8,
    pub(crate) skip_coeff: bool, // the coded skip flag
    pub(crate) nonzero: bool,    // any non-zero coefficient actually decoded
}

pub struct Dec {
    pub(crate) coeff_probs: [[[[u8; 11]; 3]; 8]; 4],
    pub(crate) seg: Segmentation,
    pub(crate) filt: FilterHeader,
    pub(crate) base_q: i32,
    pub(crate) qd: QuantDeltas,
    pub(crate) quants: [Quant; NUM_MB_SEGMENTS],
    pub(crate) prob_skip_false: u8,
    pub(crate) use_skip_prob: bool,
}

// Non-zero context layout, per macroblock, 9 entries:
//   0..3  the four luma columns / rows
//   4..5  U
//   6..7  V
//   8     Y2
// "above" keeps one of these per macroblock column; "left" is a single set
// reset at the start of each macroblock row.
pub(crate) const NZ: usize = 9;

impl Dec {
    pub(crate) fn new() -> Dec {
        Dec {
            coeff_probs: DEFAULT_COEFF_PROBS,
            seg: Segmentation::default(),
            filt: FilterHeader::default(),
            base_q: 0,
            qd: QuantDeltas::default(),
            quants: [Quant::default(); NUM_MB_SEGMENTS],
            prob_skip_false: 0,
            use_skip_prob: false,
        }
    }

    pub(crate) fn quant_for(&self, seg: usize) -> Quant {
        if self.seg.enabled {
            self.quants[seg & 3]
        } else {
            self.quants[0]
        }
    }
}

/// Parse the part of the frame header that lives in the first bool-coded
/// partition, up to (not including) the per-macroblock data.
pub(crate) fn parse_compressed_header(bd: &mut Bool, dec: &mut Dec) -> Option<()> {
    // Key frame: colour space and clamping type. Only 0 is defined for colour
    // space; a 1 here means a bitstream this decoder has no definition for.
    if bd.get_bit() != 0 {
        return None;
    }
    let _clamping = bd.get_bit();

    dec.seg.enabled = bd.get_bit() != 0;
    if dec.seg.enabled {
        dec.seg.update_map = bd.get_bit() != 0;
        let update_data = bd.get_bit() != 0;
        if update_data {
            dec.seg.absolute = bd.get_bit() != 0;
            for i in 0..NUM_MB_SEGMENTS {
                dec.seg.quant[i] = bd.get_opt_signed(7);
            }
            for i in 0..NUM_MB_SEGMENTS {
                dec.seg.filter[i] = bd.get_opt_signed(6);
            }
        }
        if dec.seg.update_map {
            for i in 0..3 {
                dec.seg.tree_probs[i] = if bd.get_bit() != 0 { bd.get_uint(8) as u8 } else { 255 };
            }
        }
    } else {
        dec.seg.update_map = false;
    }

    dec.filt.simple = bd.get_bit() != 0;
    dec.filt.level = bd.get_uint(6) as i32;
    dec.filt.sharpness = bd.get_uint(3) as i32;
    dec.filt.delta_enabled = bd.get_bit() != 0;
    if dec.filt.delta_enabled && bd.get_bit() != 0 {
        for i in 0..4 {
            dec.filt.ref_delta[i] = bd.get_opt_signed(6);
        }
        for i in 0..4 {
            dec.filt.mode_delta[i] = bd.get_opt_signed(6);
        }
    }
    Some(())
}

pub(crate) fn parse_quant_and_probs(bd: &mut Bool, dec: &mut Dec) {
    dec.base_q = bd.get_uint(7) as i32;
    dec.qd.y1_dc = bd.get_opt_signed(4);
    dec.qd.y2_dc = bd.get_opt_signed(4);
    dec.qd.y2_ac = bd.get_opt_signed(4);
    dec.qd.uv_dc = bd.get_opt_signed(4);
    dec.qd.uv_ac = bd.get_opt_signed(4);

    // Key frame: refresh_entropy_probs. Nothing persists between frames here
    // (a WebP still is one frame), so the bit is consumed and discarded.
    let _refresh = bd.get_bit();

    for i in 0..4 {
        for j in 0..8 {
            for k in 0..3 {
                for l in 0..11 {
                    if bd.get(COEFF_UPDATE_PROBS[i][j][k][l]) != 0 {
                        dec.coeff_probs[i][j][k][l] = bd.get_uint(8) as u8;
                    }
                }
            }
        }
    }

    dec.use_skip_prob = bd.get_bit() != 0;
    dec.prob_skip_false = if dec.use_skip_prob { bd.get_uint(8) as u8 } else { 0 };

    for s in 0..NUM_MB_SEGMENTS {
        let base = if dec.seg.enabled {
            if dec.seg.absolute { dec.seg.quant[s] } else { dec.base_q + dec.seg.quant[s] }
        } else {
            dec.base_q
        };
        dec.quants[s] = build_quant(if base < 0 { 0 } else if base > 127 { 127 } else { base }, &dec.qd);
    }
}
