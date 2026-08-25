//! VP8 INTER-frame decoder: video. Golden/altref reference slots with sign
//! bias, mode/entropy probability persistence across frames
//! (`refresh_entropy_probs`), MV decoding (short/long tree, near/nearest/new,
//! split down to 4x4), the sixtap and bilinear sub-pixel filters with VP8's
//! exact two-pass ordering, and the loop filter's per-reference/per-mode
//! deltas. `vp8_frame.rs`/`vp8_dec.rs`/`vp8_rec.rs` are the shared plumbing
//! (bool decoder, quant tables, transforms, intra predictors, coefficient
//! tokens, loop filter kernels) -- this file adds only what a frame with a
//! *previous* frame needs.
//!
//! WHY THIS IS A SEPARATE, FEATURE-GATED MODULE AND NOT MORE OF
//! `vp8_frame.rs`. `$(RUST_LIB)` -- the crate compiled by `cargo build
//! --release --target x86_64-unknown-none` with no `--features` -- is linked
//! into the KERNEL (`$(KERNEL): $(OBJ) $(RUST_LIB) linker.ld`) *and* into the
//! browser and `/bin/imgcheck` (same artifact, same invocation, see the
//! Makefile). CLAUDE.md is explicit about why a video decoder does not belong
//! in ring 0: it holds megabytes of live reference frames and runs 30x/second,
//! which is exactly the shape `c/lib/video` (H.264/H.265) was pulled out of
//! the kernel for. A VP8 key FRAME is fine in the kernel -- one shot, no
//! reference frames, which is why `vp8_frame.rs` (WebP) is unconditional --
//! but VP8 INTER frames reintroduce precisely the thing that rule exists to
//! keep out: `VideoDec` below owns three full reference-frame copies
//! (`RefBuf`) plus a per-macroblock mode/MV grid, alive for the life of a
//! decode session, not freed between frames.
//!
//! The mechanism is a Cargo feature, `vp8-interframe` (default OFF, see
//! `rust/Cargo.toml`), gating this module's declaration in `lib.rs`
//! (`#[cfg(feature = "vp8-interframe")] mod vp8_inter;`) and every inter-only
//! table in `vp8_tables.rs`. The kernel's `$(RUST_LIB)` build never passes
//! `--features`, so this module -- and the tables it alone uses
//! (`Y_MODE_TREE`, `MV_REF_TREE`, `SIXTAP_FILTERS`, ...) -- are never even
//! parsed for that build, let alone linked in. A ring-3 consumer (browser or
//! Preview, once wired to WebM/VP8 video) is meant to build its OWN
//! `RUST_LIB` variant with `--features vp8-interframe` turned on, the same
//! way `test-webp-vp8-negctl` already builds a scratch variant with its own
//! `--features` into a separate `--target-dir` (see `tests/vp8.mk`) rather
//! than touching the shared build. Proof, not assertion: see the report for
//! `nm` output showing zero `vp8_inter`/inter-table symbols in the real
//! kernel `$(RUST_LIB)`.
//!
//! GROUND TRUTH: RFC 6386's own reference decoder (Attachment One, "dixie") --
//! `modemv.c` (mode/MV parsing, `find_near_mvs`, split MV, `read_mvcomponent`)
//! and `predict.c` (`Hinterp`/`Vinterp`, the two-pass sub-pixel filter, chroma
//! MV averaging). Every function below is transcribed from that source, not
//! from the prose alone -- the prose and the reference code disagree in one
//! place noted at the NEWMV clamp below, and the code (what libvpx and every
//! real decoder actually runs) is what a `dwebp`/ffmpeg-style byte-exact gate
//! has to match.
//!
//! COORDINATE-CLAMPED READS REPLACE A PHYSICAL BORDER BUFFER, on purpose.
//! dixie extends each reference frame with a real border and only builds a
//! per-block emulated edge (`build_mc_border`) when a motion vector's reach
//! would exceed it (`need_mc_border`). That border is pure edge replication
//! (`memset(dst, ref_row[0], left)` / `memset(..., ref_row[w-1], right)`), and
//! edge replication is scale-invariant: clamping a coordinate to `[0, w)` /
//! `[0, h)` for ANY out-of-range offset reads the identical value a
//! finite-width replicated border would have supplied, because every pixel in
//! that border -- at any distance -- is the same edge pixel. So `samp()`
//! below need never special-case "how far out of bounds": one clamp handles
//! whole-MB clamped vectors, unclamped SPLITMV/NEW4x4 vectors, and everything
//! in between, with no `need_mc_border` bookkeeping and no separate emulated-
//! edge code path to keep in sync with the fast one.
//!
//! PER-4x4-SUBBLOCK PREDICTION EVEN WHEN ONE MV COVERS THE WHOLE MACROBLOCK.
//! dixie's `recon_1_block` already predicts one 4x4 block at a time
//! regardless of mode; the sixtap filter's per-pixel result depends only on
//! that pixel's OWN fractional offset and its 6 taps, never on neighbouring
//! output pixels, so tiling 4x4 vs. doing a whole 16x16 span in one pass is
//! the same arithmetic either way. One code path serves NEARESTMV/NEARMV/
//! ZEROMV/NEWMV (16 identical MVs) and SPLITMV (up to 16 distinct ones)
//! without a separate "fast" 16x16 predictor to keep byte-identical to the
//! 4x4 one.

use crate::imgbuf::Buf;
use crate::vp8::{Bool, FilterHeader, MAX_PARTITIONS, NUM_MB_SEGMENTS, Quant, QuantDeltas,
                  Segmentation, build_quant};
use crate::vp8_dec::{B_PRED, Planes, bmode_of, clamp255, idct4x4_add, iwht4x4, NZ};
use crate::vp8_frame::loop_filter;
use crate::vp8_rec::{decode_coeffs, pred4, pred_block};
use crate::vp8_tables::*;

// Inter y_mode values (RFC 6386's mv_ref enum: mv_nearest = num_ymodes = 5).
const NEARESTMV: u8 = 5;
const NEARMV: u8 = 6;
const ZEROMV: u8 = 7;
const NEWMV: u8 = 8;
const SPLITMV: u8 = 9;

fn le24(p: &[u8], i: usize) -> Option<usize> {
    Some(*p.get(i)? as usize | (*p.get(i + 1)? as usize) << 8 | (*p.get(i + 2)? as usize) << 16)
}

// ---------------------------------------------------------------------------
// Per-macroblock mode/MV info, persisted across the WHOLE frame (raster
// order lets a later macroblock always see an already-decoded neighbour) but
// NOT across frames -- every frame's own row loop overwrites every interior
// cell, matching dixie's `mb_info_rows` (reused, not re-zeroed, each frame).
// ---------------------------------------------------------------------------

#[derive(Clone, Copy)]
struct MbInfo {
    ref_frame: u8, // 0 = intra (dixie's CURRENT_FRAME) / 1 last / 2 golden / 3 altref
    y_mode: u8,    // 0..4 intra (DC/V/H/TM/B_PRED) or 5..9 inter (NEAREST..SPLIT)
    segment_id: u8,
    mv: (i32, i32),           // (row, col), 1/8-pel; for SPLITMV == submv[15]
    submv: [(i32, i32); 16],  // valid only when y_mode == SPLITMV
}

const MBI_ZERO: MbInfo = MbInfo { ref_frame: 0, y_mode: 0, segment_id: 0, mv: (0, 0), submv: [(0, 0); 16] };
const MBI_SZ: usize = core::mem::size_of::<MbInfo>();

/// A (mb_w+1) x (mb_h+1) grid with a permanent zeroed border at row -1/col -1
/// (dixie's "1 MB border of 0,0 motion vectors", implicitly intra since
/// `ref_frame == 0` there) -- so a neighbour lookup at the frame edge never
/// needs a bounds special case.
struct MbGrid {
    buf: Buf,
    stride: usize,
}

impl MbGrid {
    fn new(mb_w: usize, mb_h: usize) -> Option<MbGrid> {
        let stride = mb_w.checked_add(1)?;
        let rows = mb_h.checked_add(1)?;
        let bytes = stride.checked_mul(rows)?.checked_mul(MBI_SZ)?;
        let buf = Buf::zeroed(bytes)?; // all-zero bytes == MBI_ZERO (plain-int fields)
        Some(MbGrid { buf, stride })
    }

    #[inline]
    fn get(&self, row: isize, col: isize) -> MbInfo {
        let idx = ((row + 1) as usize) * self.stride + (col + 1) as usize;
        let off = idx * MBI_SZ;
        let src = &self.buf.as_ref()[off..off + MBI_SZ];
        unsafe { core::ptr::read_unaligned(src.as_ptr() as *const MbInfo) }
    }

    #[inline]
    fn set(&mut self, row: isize, col: isize, v: MbInfo) {
        let idx = ((row + 1) as usize) * self.stride + (col + 1) as usize;
        let off = idx * MBI_SZ;
        let dst = &mut self.buf.as_mut()[off..off + MBI_SZ];
        unsafe { core::ptr::write_unaligned(dst.as_mut_ptr() as *mut MbInfo, v) };
    }
}

// ---------------------------------------------------------------------------
// A reference frame: tightly packed Y/U/V (no borders -- motion compensation
// reads through `samp()`'s coordinate clamp instead, see the module doc).
// ---------------------------------------------------------------------------

struct RefBuf {
    y: Buf,
    u: Buf,
    v: Buf,
    yw: usize,
    yh: usize,
    cw: usize,
    ch: usize,
}

impl RefBuf {
    fn from_planes(pl: &Planes) -> Option<RefBuf> {
        let mut y = Buf::new(pl.yw * pl.yh)?;
        let mut u = Buf::new(pl.cw * pl.ch)?;
        let mut v = Buf::new(pl.cw * pl.ch)?;
        {
            let src = pl.buf.as_ref();
            let dy = y.as_mut();
            for r in 0..pl.yh {
                let so = pl.yb + r * pl.ys;
                dy[r * pl.yw..r * pl.yw + pl.yw].copy_from_slice(&src[so..so + pl.yw]);
            }
            let du = u.as_mut();
            for r in 0..pl.ch {
                let so = pl.ub + r * pl.cs;
                du[r * pl.cw..r * pl.cw + pl.cw].copy_from_slice(&src[so..so + pl.cw]);
            }
            let dv = v.as_mut();
            for r in 0..pl.ch {
                let so = pl.vb + r * pl.cs;
                dv[r * pl.cw..r * pl.cw + pl.cw].copy_from_slice(&src[so..so + pl.cw]);
            }
        }
        Some(RefBuf { y, u, v, yw: pl.yw, yh: pl.yh, cw: pl.cw, ch: pl.ch })
    }

    fn dup(&self) -> Option<RefBuf> {
        let mut y = Buf::new(self.y.len())?;
        y.as_mut().copy_from_slice(self.y.as_ref());
        let mut u = Buf::new(self.u.len())?;
        u.as_mut().copy_from_slice(self.u.as_ref());
        let mut v = Buf::new(self.v.len())?;
        v.as_mut().copy_from_slice(self.v.as_ref());
        Some(RefBuf { y, u, v, yw: self.yw, yh: self.yh, cw: self.cw, ch: self.ch })
    }
}

/// Edge-clamped sample read -- see the module doc for why this is exactly
/// equivalent to dixie's finite emulated border, at any offset.
#[inline]
fn samp(plane: &[u8], w: usize, h: usize, x: i32, y: i32) -> u8 {
    let cx = if x < 0 { 0 } else if (x as usize) >= w { w - 1 } else { x as usize };
    let cy = if y < 0 { 0 } else if (y as usize) >= h { h - 1 } else { y as usize };
    plane[cy * w + cx]
}

/// Sub-pixel-interpolate one 4x4 block from `plane` (dims `w`x`h`) at pixel
/// origin (`x0`,`y0`) plus motion vector (`mv_row`,`mv_col`) in 1/8-pel.
/// RFC 6386 18.3 / dixie's `Hinterp`+`Vinterp`: horizontal 6-tap pass first
/// into a 9-row intermediate (2 rows above + 4 + 3 below), then vertical.
/// `vp8-sixtap-swap` is the negative control -- vertical first, which the
/// spec's own ordering note ("must calculate two extra rows above and three
/// extra below... for the vertical interpolation to proceed") says is wrong,
/// and which only differs from the correct order when a tap is non-trivial
/// (fx!=0 or fy!=0) because the u8 round-and-clamp between passes is where
/// the two orders stop being algebraically interchangeable.
fn mc_block4(plane: &[u8], w: usize, h: usize, x0: i32, y0: i32, mv_row: i32, mv_col: i32,
             filters: &[[i32; 6]; 8], out: &mut [[u8; 4]; 4], had_frac: &mut bool) {
    let ix = x0 + (mv_col >> 3);
    let iy = y0 + (mv_row >> 3);
    let fx = (mv_col & 7) as usize;
    let fy = (mv_row & 7) as usize;
    if fx != 0 || fy != 0 {
        *had_frac = true;
    }
    let hf = &filters[fx];
    let vf = &filters[fy];

    if !cfg!(feature = "vp8-sixtap-swap") {
        let mut temp = [[0u8; 4]; 9];
        for r in 0..9 {
            let sy = iy - 2 + r as i32;
            for c in 0..4 {
                let sx = ix + c as i32;
                let mut acc = 0i32;
                for t in 0..6 {
                    acc += samp(plane, w, h, sx - 2 + t as i32, sy) as i32 * hf[t];
                }
                temp[r][c] = clamp255((acc + 64) >> 7);
            }
        }
        for r in 0..4 {
            for c in 0..4 {
                let mut acc = 0i32;
                for t in 0..6 {
                    acc += temp[r + t][c] as i32 * vf[t];
                }
                out[r][c] = clamp255((acc + 64) >> 7);
            }
        }
    } else {
        // NEGATIVE CONTROL: vertical pass first (wrong order -- see doc above).
        let mut temp = [[0u8; 9]; 4];
        for r in 0..4 {
            let sy = iy + r as i32;
            for c in 0..9 {
                let sx = ix - 2 + c as i32;
                let mut acc = 0i32;
                for t in 0..6 {
                    acc += samp(plane, w, h, sx, sy - 2 + t as i32) as i32 * vf[t];
                }
                temp[r][c] = clamp255((acc + 64) >> 7);
            }
        }
        for r in 0..4 {
            for c in 0..4 {
                let mut acc = 0i32;
                for t in 0..6 {
                    acc += temp[r][c + t] as i32 * hf[t];
                }
                out[r][c] = clamp255((acc + 64) >> 7);
            }
        }
    }
}

/// RFC 6386 18.1's `avg()` / dixie's `calculate_chroma_splitmv`, generalised:
/// both the SPLITMV case (4 distinct luma subblock MVs) and the whole-MB case
/// (the same MV counted 4 times, which collapses this exact formula to
/// dixie's separate `(v+1+(v>>31)*2)/2` halving shortcut -- verified
/// algebraically equal for every v, so one function serves both).
fn chroma_mv_from4(mvs: [(i32, i32); 4], full_pixel: bool) -> (i32, i32) {
    let sr: i32 = mvs[0].0 + mvs[1].0 + mvs[2].0 + mvs[3].0;
    let sc: i32 = mvs[0].1 + mvs[1].1 + mvs[2].1 + mvs[3].1;
    let round = |s: i32| if s < 0 { (s - 4) / 8 } else { (s + 4) / 8 };
    let mut row = round(sr);
    let mut col = round(sc);
    if full_pixel {
        row &= !7;
        col &= !7;
    }
    (row, col)
}

#[inline]
fn clamp_mv(mv: (i32, i32), b: (i32, i32, i32, i32)) -> (i32, i32) {
    // b = (to_left, to_right, to_top, to_bottom)
    (mv.0.clamp(b.2, b.3), mv.1.clamp(b.0, b.1))
}

// ---------------------------------------------------------------------------
// Motion vector decoding (RFC 6386 17 / dixie's `read_mv_component`).
// ---------------------------------------------------------------------------

fn read_mv_component(bd: &mut Bool, p: &[u8; 19]) -> i32 {
    // p[0]=is_short, p[1]=sign, p[2..9]=short tree (7), p[9..19]=long bits (10)
    let mut x = 0i32;
    if bd.get(p[0]) != 0 {
        for i in 0..3 {
            x += (bd.get(p[9 + i]) as i32) << i;
        }
        for i in (4..10).rev() {
            x += (bd.get(p[9 + i]) as i32) << i;
        }
        if (x & 0xFFF0) == 0 || bd.get(p[9 + 3]) != 0 {
            x += 8;
        }
    } else {
        x = bd.get_tree(&SMALL_MV_TREE, &p[2..9]);
    }
    if x != 0 && bd.get(p[1]) != 0 {
        x = -x;
    }
    x << 1 // quarter-pel decoded value -> 1/8-pel storage (RFC 6386 18.1)
}

fn read_mv(bd: &mut Bool, mvc: &[[u8; 19]; 2]) -> (i32, i32) {
    let row = read_mv_component(bd, &mvc[0]);
    let col = read_mv_component(bd, &mvc[1]);
    (row, col)
}

/// RFC 6386 16.4's `vp8_mvCont` / dixie's `submv_ref`: which of the four
/// context rows applies, from whether the left/above subblock MVs are zero
/// and/or equal to each other.
fn submv_context(l: (i32, i32), a: (i32, i32)) -> usize {
    let lez = l == (0, 0);
    let aez = a == (0, 0);
    let lea = l == a;
    if lea && lez { 4 } else if lea { 3 } else if aez { 2 } else if lez { 1 } else { 0 }
}

/// RFC 6386 16.3 / dixie's `find_near_mvs`: a weighted census of up to 3
/// already-decoded neighbours (above, left, above-left), each either
/// contributing to an existing distinct MV's weight or starting a new one.
/// Returns (near_mvs, cnt) with near_mvs[0]=best, [1]=nearest, [2]=near, and
/// cnt[i] the weight that selects `MODE_CONTEXTS[cnt[i]][i]`.
fn find_near_mvs(this_ref: u8, left: MbInfo, above: MbInfo, aboveleft: MbInfo,
                  sign_bias: [bool; 4]) -> ([(i32, i32); 4], [i32; 4]) {
    let mut near_mvs = [(0i32, 0i32); 4];
    let mut cnt = [0i32; 4];
    let mut mvi: usize = 0;
    let mut cnti: usize = 0;

    if above.ref_frame != 0 {
        if above.mv != (0, 0) {
            mvi += 1;
            let mut m = above.mv;
            if sign_bias[above.ref_frame as usize] != sign_bias[this_ref as usize] {
                m = (-m.0, -m.1);
            }
            near_mvs[mvi] = m;
            cnti += 1;
        }
        cnt[cnti] += 2;
    }

    if left.ref_frame != 0 {
        if left.mv != (0, 0) {
            let mut m = left.mv;
            if sign_bias[left.ref_frame as usize] != sign_bias[this_ref as usize] {
                m = (-m.0, -m.1);
            }
            if m != near_mvs[mvi] {
                mvi += 1;
                near_mvs[mvi] = m;
                cnti += 1;
            }
            cnt[cnti] += 2;
        } else {
            cnt[0] += 2;
        }
    }

    if aboveleft.ref_frame != 0 {
        if aboveleft.mv != (0, 0) {
            let mut m = aboveleft.mv;
            if sign_bias[aboveleft.ref_frame as usize] != sign_bias[this_ref as usize] {
                m = (-m.0, -m.1);
            }
            if m != near_mvs[mvi] {
                mvi += 1;
                near_mvs[mvi] = m;
                cnti += 1;
            }
            cnt[cnti] += 1;
        } else {
            cnt[0] += 1;
        }
    }

    // cnt[3] is reused here as scratch (matches the C: cntx only reaches
    // index 3 when all three neighbours contributed distinct nonzero MVs) --
    // if it did, see whether the above-left MV can be merged into "nearest".
    if cnt[3] != 0 && near_mvs[mvi] == near_mvs[1] {
        cnt[1] += 1;
    }

    cnt[3] = ((above.y_mode == SPLITMV) as i32 + (left.y_mode == SPLITMV) as i32) * 2
        + (aboveleft.y_mode == SPLITMV) as i32;

    if cnt[2] > cnt[1] {
        cnt.swap(1, 2);
        near_mvs.swap(1, 2);
    }
    if cnt[1] >= cnt[0] {
        near_mvs[0] = near_mvs[1];
    }

    (near_mvs, cnt)
}

/// RFC 6386 16.4 / dixie's `decode_split_mv`: partition selection then, per
/// partition, LEFT4x4/ABOVE4x4/ZERO4x4/NEW4x4 -- filling `mbi.submv[]`.
fn decode_split_mv(bd: &mut Bool, mbi: &mut MbInfo, left: MbInfo, above: MbInfo,
                    mv_probs: &[[u8; 19]; 2], best: (i32, i32)) {
    let part_id = bd.get_tree(&SPLIT_MV_TREE, &SPLIT_MV_PROBS) as usize & 3;
    let partition = &MV_PARTITIONS[part_id];
    let mut mask: u32 = 0;
    let mut j: i32 = 0;
    while mask < 65535 && j < 16 {
        let mut k: usize = 0;
        while k < 16 && partition[k] != j {
            k += 1;
        }
        if k >= 16 {
            break;
        }
        let left_mv = if k % 4 == 0 {
            if left.y_mode == SPLITMV { left.submv[k + 3] } else { left.mv }
        } else {
            mbi.submv[k - 1]
        };
        let above_mv = if k < 4 {
            if above.y_mode == SPLITMV { above.submv[k + 12] } else { above.mv }
        } else {
            mbi.submv[k - 4]
        };
        let ctx = submv_context(left_mv, above_mv);
        let sub = bd.get_tree(&SUBMV_REF_TREE, &SUBMV_REF_PROBS[ctx]) as usize;
        let mv = match sub {
            10 => left_mv,
            11 => above_mv,
            12 => (0, 0),
            _ => {
                let d = read_mv(bd, mv_probs);
                (d.0 + best.0, d.1 + best.1)
            }
        };
        for kk in k..16 {
            if partition[kk] == j {
                mbi.submv[kk] = mv;
                mask |= 1 << kk;
            }
        }
        j += 1;
    }
    mbi.y_mode = SPLITMV;
    mbi.mv = mbi.submv[15];
}

/// RFC 6386 16.2 / dixie's `decode_mvs`: reference-frame selection, the
/// near/nearest/best census, the mv_ref mode tree, then each mode's MV rule.
///
/// THE ONE PLACE THE RFC'S PROSE AND ITS OWN REFERENCE CODE DISAGREE, and
/// this follows the prose (6386 18.1: "the final motion vector is clamped
/// again after combining... for NEWMV... the secondary clamping is not
/// performed for SPLITMV" -- a contrast that only makes sense if NEWMV gets
/// it). dixie's own `decode_mvs` does not call `clamp_mv` a second time after
/// adding the NEWMV delta; the RFC text is unambiguous that it should. For
/// any real encoder's output the difference is unobservable (the delta plus
/// an already-clamped `best_mv` essentially never leaves the +-16px-past-
/// frame-edge clamp region on ordinary content), so this is a belt-and-braces
/// choice, not one either the WebP-derived key-frame path or this test
/// corpus can distinguish.
fn decode_mvs_mb(bd: &mut Bool, mbi: &mut MbInfo, left: MbInfo, above: MbInfo, aboveleft: MbInfo,
                  prob_last: u8, prob_gf: u8, mv_probs: &[[u8; 19]; 2], sign_bias: [bool; 4],
                  bounds: (i32, i32, i32, i32)) {
    let ref_frame: u8 = if bd.get(prob_last) != 0 { 2 + bd.get(prob_gf) as u8 } else { 1 };
    mbi.ref_frame = ref_frame;

    let (near_mvs, cnt) = find_near_mvs(ref_frame, left, above, aboveleft, sign_bias);
    let probs = [
        MODE_CONTEXTS[cnt[0].clamp(0, 5) as usize][0],
        MODE_CONTEXTS[cnt[1].clamp(0, 5) as usize][1],
        MODE_CONTEXTS[cnt[2].clamp(0, 5) as usize][2],
        MODE_CONTEXTS[cnt[3].clamp(0, 5) as usize][3],
    ];
    let y_mode = bd.get_tree(&MV_REF_TREE, &probs) as u8;
    mbi.y_mode = y_mode;

    match y_mode {
        NEARESTMV => mbi.mv = clamp_mv(near_mvs[1], bounds),
        NEARMV => mbi.mv = clamp_mv(near_mvs[2], bounds),
        ZEROMV => mbi.mv = (0, 0),
        NEWMV => {
            let best = clamp_mv(near_mvs[0], bounds);
            let d = read_mv(bd, mv_probs);
            mbi.mv = clamp_mv((d.0 + best.0, d.1 + best.1), bounds);
        }
        SPLITMV => {
            let best = clamp_mv(near_mvs[0], bounds);
            decode_split_mv(bd, mbi, left, above, mv_probs, best);
        }
        _ => {}
    }
}

// ---------------------------------------------------------------------------
// The persistent decoder. One instance per video stream: reference frames and
// entropy/segmentation/loop-filter state all outlive a single frame.
// ---------------------------------------------------------------------------

pub struct VideoDec {
    mb_w: usize,
    mb_h: usize,
    width: i32,
    height: i32,
    inited: bool,

    coeff_probs: [[[[u8; 11]; 3]; 8]; 4],
    mv_probs: [[u8; 19]; 2],
    y_mode_probs: [u8; 4],
    uv_mode_probs: [u8; 3],

    seg: Segmentation,
    filt: FilterHeader,
    sign_bias: [bool; 4],

    quants: [Quant; NUM_MB_SEGMENTS],

    last: Option<RefBuf>,
    golden: Option<RefBuf>,
    altref: Option<RefBuf>,

    mbg: Option<MbGrid>,

    out: Option<Buf>, // tightly packed I420, cropped to width x height
    had_frac_mv: bool,
}

impl VideoDec {
    fn new() -> VideoDec {
        VideoDec {
            mb_w: 0, mb_h: 0, width: 0, height: 0, inited: false,
            coeff_probs: DEFAULT_COEFF_PROBS,
            mv_probs: DEFAULT_MV_PROBS,
            y_mode_probs: Y_MODE_PROB,
            uv_mode_probs: UV_MODE_PROB,
            seg: Segmentation::default(),
            filt: FilterHeader::default(),
            sign_bias: [false; 4],
            quants: [Quant::default(); NUM_MB_SEGMENTS],
            last: None, golden: None, altref: None,
            mbg: None,
            out: None,
            had_frac_mv: false,
        }
    }
}

/// Decode one VP8 frame payload (the bytes after any container framing --
/// IVF's own 12-byte frame header is the caller's job, see `vp8_video_test.c`).
/// `Some(true)` = decoded and shown, `Some(false)` = decoded but not shown
/// (an invisible altref frame -- reference buffers still update; no output),
/// `None` = malformed/unsupported.
fn decode_frame(dec: &mut VideoDec, p: &[u8]) -> Option<bool> {
    if p.len() < 3 {
        return None;
    }
    let tag = p[0] as u32 | (p[1] as u32) << 8 | (p[2] as u32) << 16;
    let keyframe = (tag & 1) == 0;
    let version = (tag >> 1) & 3; // 2 bits (RFC 6386 9.1's BITS_GET(raw,1,2))
    let experimental = (tag >> 3) & 1;
    let shown = ((tag >> 4) & 1) != 0;
    let part0_sz = ((tag >> 5) & 0x7FFFF) as usize;
    if experimental != 0 {
        return None; // dixie refuses these by name; so do we
    }

    let data_start;
    let mb_w;
    let mb_h;
    let width;
    let height;
    let mut need_realloc = false;

    if keyframe {
        if p.len() < 10 {
            return None;
        }
        if p[3] != 0x9d || p[4] != 0x01 || p[5] != 0x2a {
            return None;
        }
        let w = (p[6] as usize | (p[7] as usize) << 8) & 0x3FFF;
        let h = (p[8] as usize | (p[9] as usize) << 8) & 0x3FFF;
        if w == 0 || h == 0 || w > 16383 || h > 16383 || w * h > 64 * 1024 * 1024 {
            return None;
        }
        data_start = 10;
        mb_w = (w + 15) / 16;
        mb_h = (h + 15) / 16;
        width = w as i32;
        height = h as i32;
        if !dec.inited || dec.mb_w != mb_w || dec.mb_h != mb_h {
            need_realloc = true;
        }
    } else {
        if !dec.inited {
            return None; // a stream must start with a keyframe
        }
        data_start = 3;
        mb_w = dec.mb_w;
        mb_h = dec.mb_h;
        width = dec.width;
        height = dec.height;
    }
    if data_start + part0_sz > p.len() || mb_w == 0 || mb_h == 0 {
        return None;
    }

    let part0 = &p[data_start..data_start + part0_sz];
    let mut bd0 = Bool::new(part0);

    if keyframe {
        if bd0.get_uint(2) != 0 {
            return None; // reserved colour-space/clamp bits
        }
        dec.seg = Segmentation::default();
        dec.filt = FilterHeader::default();
    }

    // --- segmentation header (RFC 6386 9.3, same layout key or inter) ---
    dec.seg.enabled = bd0.get_bit() != 0;
    if dec.seg.enabled {
        dec.seg.update_map = bd0.get_bit() != 0;
        let update_data = bd0.get_bit() != 0;
        if update_data {
            dec.seg.absolute = bd0.get_bit() != 0;
            for i in 0..NUM_MB_SEGMENTS {
                dec.seg.quant[i] = bd0.get_opt_signed(7);
            }
            for i in 0..NUM_MB_SEGMENTS {
                dec.seg.filter[i] = bd0.get_opt_signed(6);
            }
        }
        if dec.seg.update_map {
            for i in 0..3 {
                dec.seg.tree_probs[i] = if bd0.get_bit() != 0 { bd0.get_uint(8) as u8 } else { 255 };
            }
        }
    } else {
        dec.seg.update_map = false;
    }

    // --- loop filter header (RFC 6386 9.4) ---
    dec.filt.simple = bd0.get_bit() != 0;
    dec.filt.level = bd0.get_uint(6) as i32;
    dec.filt.sharpness = bd0.get_uint(3) as i32;
    dec.filt.delta_enabled = bd0.get_bit() != 0;
    if dec.filt.delta_enabled && bd0.get_bit() != 0 {
        for i in 0..4 {
            dec.filt.ref_delta[i] = bd0.get_opt_signed(6);
        }
        for i in 0..4 {
            dec.filt.mode_delta[i] = bd0.get_opt_signed(6);
        }
    }

    // --- token partitions (RFC 6386 9.5) ---
    let log2_parts = bd0.get_uint(2);
    let nparts = 1usize << log2_parts;
    let after0 = data_start + part0_sz;
    let table = 3 * (nparts - 1);
    if after0 + table > p.len() {
        return None;
    }
    let mut ranges = [(0usize, 0usize); MAX_PARTITIONS];
    let mut at = after0 + table;
    for i in 0..nparts {
        let sz = if i + 1 < nparts { le24(p, after0 + 3 * i)? } else { p.len().checked_sub(at)? };
        let end = at.checked_add(sz)?;
        if end > p.len() {
            return None;
        }
        ranges[i] = (at, end);
        at = end;
    }
    let mut toks: [Bool; MAX_PARTITIONS] =
        core::array::from_fn(|i| Bool::new(&p[ranges[i].0..ranges[i].1]));

    // --- quantiser header (RFC 6386 9.6) ---
    let base_q = bd0.get_uint(7) as i32;
    let mut qd = QuantDeltas::default();
    qd.y1_dc = bd0.get_opt_signed(4);
    qd.y2_dc = bd0.get_opt_signed(4);
    qd.y2_ac = bd0.get_opt_signed(4);
    qd.uv_dc = bd0.get_opt_signed(4);
    qd.uv_ac = bd0.get_opt_signed(4);
    for s in 0..NUM_MB_SEGMENTS {
        let base = if dec.seg.enabled {
            if dec.seg.absolute { dec.seg.quant[s] } else { base_q + dec.seg.quant[s] }
        } else {
            base_q
        };
        dec.quants[s] = build_quant(base.clamp(0, 127), &qd);
    }

    // --- reference header (RFC 6386 9.7/9.8) ---
    let refresh_gf;
    let refresh_arf;
    let copy_gf;
    let copy_arf;
    let refresh_entropy;
    let refresh_last;
    if keyframe {
        refresh_gf = true;
        refresh_arf = true;
        copy_gf = 0u32;
        copy_arf = 0u32;
        dec.sign_bias[2] = false;
        dec.sign_bias[3] = false;
        refresh_entropy = bd0.get_bit() != 0;
        refresh_last = true;
    } else {
        refresh_gf = bd0.get_bit() != 0;
        refresh_arf = bd0.get_bit() != 0;
        copy_gf = if !refresh_gf { bd0.get_uint(2) } else { 0 };
        copy_arf = if !refresh_arf { bd0.get_uint(2) } else { 0 };
        dec.sign_bias[2] = bd0.get_bit() != 0;
        dec.sign_bias[3] = bd0.get_bit() != 0;
        refresh_entropy = bd0.get_bit() != 0;
        refresh_last = bd0.get_bit() != 0;
    }

    if keyframe {
        dec.coeff_probs = DEFAULT_COEFF_PROBS;
        dec.mv_probs = DEFAULT_MV_PROBS;
        dec.y_mode_probs = Y_MODE_PROB;
        dec.uv_mode_probs = UV_MODE_PROB;
    }
    let saved = if !refresh_entropy {
        Some((dec.coeff_probs, dec.mv_probs, dec.y_mode_probs, dec.uv_mode_probs))
    } else {
        None
    };

    // --- entropy header (RFC 6386 9.9/9.10/13/17.2) ---
    for i in 0..4 {
        for j in 0..8 {
            for k in 0..3 {
                for l in 0..11 {
                    if bd0.get(COEFF_UPDATE_PROBS[i][j][k][l]) != 0 {
                        dec.coeff_probs[i][j][k][l] = bd0.get_uint(8) as u8;
                    }
                }
            }
        }
    }
    let coeff_skip_enabled = bd0.get_bit() != 0;
    let coeff_skip_prob = if coeff_skip_enabled { bd0.get_uint(8) as u8 } else { 0 };

    let mut prob_intra: u8 = 0;
    let mut prob_last: u8 = 0;
    let mut prob_gf: u8 = 0;
    if !keyframe {
        prob_intra = bd0.get_uint(8) as u8;
        prob_last = bd0.get_uint(8) as u8;
        prob_gf = bd0.get_uint(8) as u8;
        if bd0.get_bit() != 0 {
            for i in 0..4 {
                dec.y_mode_probs[i] = bd0.get_uint(8) as u8;
            }
        }
        if bd0.get_bit() != 0 {
            for i in 0..3 {
                dec.uv_mode_probs[i] = bd0.get_uint(8) as u8;
            }
        }
        for i in 0..2 {
            for j in 0..19 {
                if bd0.get(MV_UPDATE_PROBS[i][j]) != 0 {
                    let x = bd0.get_uint(7) as u8;
                    dec.mv_probs[i][j] = if x != 0 { x << 1 } else { 1 };
                }
            }
        }
    }

    if need_realloc {
        dec.mbg = Some(MbGrid::new(mb_w, mb_h)?);
    }
    dec.mb_w = mb_w;
    dec.mb_h = mb_h;
    dec.width = width;
    dec.height = height;

    let mut pl = Planes::new(mb_w, mb_h)?;
    let mut above_nz = Buf::zeroed(mb_w * NZ)?;
    let mut above_bmode = Buf::zeroed(mb_w * 4)?;
    let mut finfo = Buf::zeroed(mb_w * mb_h * 2)?;
    let mut had_frac = false;

    let filters: &[[i32; 6]; 8] = if version == 0 { &SIXTAP_FILTERS } else { &BILINEAR_FILTERS };
    let full_pixel = version == 3;

    for mb_y in 0..mb_h {
        let my = mb_y as isize;
        let mut left_nz = [0u8; NZ];
        let mut left_bmode = [0u8; 4];
        let mut to_left: i32 = -(1i32 << 7);
        let mut to_right: i32 = (mb_w as i32) << 7;
        let to_top: i32 = -(((mb_y as i32) + 1) << 7);
        let to_bottom: i32 = ((mb_h as i32) - (mb_y as i32)) << 7;

        for mb_x in 0..mb_w {
            let mx = mb_x as isize;
            let grid = dec.mbg.as_ref().unwrap();
            let above = grid.get(my - 1, mx);
            let left = grid.get(my, mx - 1);
            let aboveleft = grid.get(my - 1, mx - 1);
            let prevhere = grid.get(my, mx);

            let mut segid = prevhere.segment_id;
            if dec.seg.update_map {
                segid = bd0.get_tree(&MB_SEGMENT_TREE, &dec.seg.tree_probs) as u8 & 3;
            } else if keyframe {
                segid = 0;
            }

            let skip = if coeff_skip_enabled { bd0.get(coeff_skip_prob) != 0 } else { false };

            let mut mbi = MBI_ZERO;
            mbi.segment_id = segid;
            let mut is_intra_recon = true;
            let mut ymode_intra: u8 = 0;
            let mut uvmode_intra: u8 = 0;
            let mut bmodes = [0u8; 16];

            if keyframe {
                ymode_intra = bd0.get_tree(&KF_YMODE_TREE, &KF_YMODE_PROB) as u8;
                if ymode_intra as usize == B_PRED {
                    for by in 0..4 {
                        for bx in 0..4 {
                            let a = if by == 0 {
                                above_bmode.as_ref()[mb_x * 4 + bx]
                            } else {
                                bmodes[(by - 1) * 4 + bx]
                            } as usize;
                            let l = if bx == 0 { left_bmode[by] } else { bmodes[by * 4 + bx - 1] } as usize;
                            let probs = &KF_BMODE_PROBS[a.min(9)][l.min(9)];
                            bmodes[by * 4 + bx] = bd0.get_tree(&BMODE_TREE, probs) as u8;
                        }
                    }
                } else {
                    bmodes = [bmode_of(ymode_intra as usize) as u8; 16];
                }
                uvmode_intra = bd0.get_tree(&UV_MODE_TREE, &KF_UV_MODE_PROB) as u8;
                for i in 0..4 {
                    above_bmode.as_mut()[mb_x * 4 + i] = bmodes[12 + i];
                    left_bmode[i] = bmodes[i * 4 + 3];
                }
                mbi.ref_frame = 0;
                mbi.y_mode = ymode_intra;
            } else {
                let is_inter = bd0.get(prob_intra) != 0;
                if !is_inter {
                    ymode_intra = bd0.get_tree(&Y_MODE_TREE, &dec.y_mode_probs) as u8;
                    if ymode_intra as usize == B_PRED {
                        for i in 0..16 {
                            bmodes[i] = bd0.get_tree(&BMODE_TREE, &INTER_B_MODE_PROB) as u8;
                        }
                    } else {
                        bmodes = [bmode_of(ymode_intra as usize) as u8; 16];
                    }
                    uvmode_intra = bd0.get_tree(&UV_MODE_TREE, &dec.uv_mode_probs) as u8;
                    mbi.ref_frame = 0;
                    mbi.y_mode = ymode_intra;
                } else {
                    is_intra_recon = false;
                    let bounds = (to_left, to_right, to_top, to_bottom);
                    decode_mvs_mb(&mut bd0, &mut mbi, left, above, aboveleft, prob_last, prob_gf,
                                  &dec.mv_probs, dec.sign_bias, bounds);
                }
                to_left -= 16 << 3;
                to_right -= 16 << 3;
            }

            // ---- coefficients (RFC 6386 13) ----
            let has_y2 = !((mbi.ref_frame == 0 && ymode_intra as usize == B_PRED)
                || (mbi.ref_frame != 0 && mbi.y_mode == SPLITMV));
            let q = dec.quants[segid as usize & 3];
            let mut coeffs = [[0i32; 16]; 25];
            let mut nonzero = false;
            let tok = &mut toks[mb_y & (nparts - 1)];
            let anz = above_nz.as_mut();
            if skip {
                for i in 0..8 {
                    anz[mb_x * NZ + i] = 0;
                    left_nz[i] = 0;
                }
                if has_y2 {
                    anz[mb_x * NZ + 8] = 0;
                    left_nz[8] = 0;
                }
            } else {
                let first = if has_y2 {
                    let ctx = (anz[mb_x * NZ + 8] + left_nz[8]) as usize;
                    let end = decode_coeffs(tok, &dec.coeff_probs[1], ctx, 0, q.y2, &mut coeffs[24]);
                    let nz = (end > 0) as u8;
                    anz[mb_x * NZ + 8] = nz;
                    left_nz[8] = nz;
                    nonzero |= nz != 0;
                    1
                } else {
                    0
                };
                let ytype = if has_y2 { 0 } else { 3 };
                for by in 0..4 {
                    for bx in 0..4 {
                        let ctx = (anz[mb_x * NZ + bx] + left_nz[by]) as usize;
                        let end = decode_coeffs(tok, &dec.coeff_probs[ytype], ctx, first, q.y1,
                                                &mut coeffs[by * 4 + bx]);
                        let nz = (end > first) as u8;
                        anz[mb_x * NZ + bx] = nz;
                        left_nz[by] = nz;
                        nonzero |= nz != 0;
                    }
                }
                for pi in 0..2 {
                    for by in 0..2 {
                        for bx in 0..2 {
                            let ai = mb_x * NZ + 4 + pi * 2 + bx;
                            let li = 4 + pi * 2 + by;
                            let ctx = (anz[ai] + left_nz[li]) as usize;
                            let end = decode_coeffs(tok, &dec.coeff_probs[2], ctx, 0, q.uv,
                                                    &mut coeffs[16 + pi * 4 + by * 2 + bx]);
                            let nz = (end > 0) as u8;
                            anz[ai] = nz;
                            left_nz[li] = nz;
                            nonzero |= nz != 0;
                        }
                    }
                }
                if has_y2 {
                    let mut dc = [0i32; 16];
                    iwht4x4(&coeffs[24], &mut dc);
                    for i in 0..16 {
                        coeffs[i][0] = dc[i];
                    }
                }
            }

            // ---- reconstruct ----
            let ys = pl.ys;
            let cs = pl.cs;
            let yoff = pl.yb + mb_y * 16 * ys + mb_x * 16;
            let uoff = pl.ub + mb_y * 8 * cs + mb_x * 8;
            let voff = pl.vb + mb_y * 8 * cs + mb_x * 8;

            if is_intra_recon {
                let buf = pl.buf.as_mut();
                if ymode_intra as usize == B_PRED {
                    let tr: [u8; 4] = if mb_y == 0 {
                        [127; 4]
                    } else if mb_x + 1 == mb_w {
                        [buf[yoff - ys + 15]; 4]
                    } else {
                        [buf[yoff - ys + 16], buf[yoff - ys + 17], buf[yoff - ys + 18], buf[yoff - ys + 19]]
                    };
                    for by in 0..4 {
                        for bx in 0..4 {
                            let o = yoff + by * 4 * ys + bx * 4;
                            let mut a = [0u8; 9];
                            a[0] = buf[o - ys - 1];
                            for i in 0..4 {
                                a[1 + i] = buf[o - ys + i];
                            }
                            if bx == 3 {
                                a[5..9].copy_from_slice(&tr);
                            } else {
                                for i in 0..4 {
                                    a[5 + i] = buf[o - ys + 4 + i];
                                }
                            }
                            let mut lv = [0u8; 5];
                            lv[0] = a[0];
                            for i in 0..4 {
                                lv[1 + i] = buf[o + i * ys - 1];
                            }
                            let mut b = [[0u8; 4]; 4];
                            pred4(&mut b, &a, &lv, bmodes[by * 4 + bx] as usize);
                            for r in 0..4 {
                                for c in 0..4 {
                                    buf[o + r * ys + c] = b[r][c];
                                }
                            }
                            idct4x4_add(&coeffs[by * 4 + bx], buf, o, ys);
                        }
                    }
                } else {
                    pred_block(buf, yoff, ys, 16, ymode_intra as usize, mb_x > 0, mb_y > 0);
                    for by in 0..4 {
                        for bx in 0..4 {
                            idct4x4_add(&coeffs[by * 4 + bx], buf, yoff + by * 4 * ys + bx * 4, ys);
                        }
                    }
                }
                pred_block(buf, uoff, cs, 8, uvmode_intra as usize, mb_x > 0, mb_y > 0);
                pred_block(buf, voff, cs, 8, uvmode_intra as usize, mb_x > 0, mb_y > 0);
                for by in 0..2 {
                    for bx in 0..2 {
                        idct4x4_add(&coeffs[16 + by * 2 + bx], buf, uoff + by * 4 * cs + bx * 4, cs);
                        idct4x4_add(&coeffs[20 + by * 2 + bx], buf, voff + by * 4 * cs + bx * 4, cs);
                    }
                }
            } else {
                let refplane: &RefBuf = match mbi.ref_frame {
                    1 => dec.last.as_ref()?,
                    2 => dec.golden.as_ref()?,
                    _ => dec.altref.as_ref()?,
                };
                let mvs16: [(i32, i32); 16] = if mbi.y_mode == SPLITMV { mbi.submv } else { [mbi.mv; 16] };
                let mb_ox = (mb_x as i32) * 16;
                let mb_oy = (mb_y as i32) * 16;

                for b in 0..16 {
                    let bx = (b % 4) * 4;
                    let by = (b / 4) * 4;
                    let (mv_row, mv_col) = mvs16[b];
                    let mut out = [[0u8; 4]; 4];
                    mc_block4(refplane.y.as_ref(), refplane.yw, refplane.yh,
                             mb_ox + bx as i32, mb_oy + by as i32, mv_row, mv_col, filters,
                             &mut out, &mut had_frac);
                    let o = yoff + by * ys + bx;
                    let buf = pl.buf.as_mut();
                    for r in 0..4 {
                        for c in 0..4 {
                            buf[o + r * ys + c] = out[r][c];
                        }
                    }
                    idct4x4_add(&coeffs[b], buf, o, ys);
                }

                let qgroups = [[0, 1, 4, 5], [2, 3, 6, 7], [8, 9, 12, 13], [10, 11, 14, 15]];
                let mut chroma_mv = [(0i32, 0i32); 4];
                for qi in 0..4 {
                    let g = qgroups[qi];
                    chroma_mv[qi] = chroma_mv_from4([mvs16[g[0]], mvs16[g[1]], mvs16[g[2]], mvs16[g[3]]],
                                                    full_pixel);
                }
                let mb_cox = (mb_x as i32) * 8;
                let mb_coy = (mb_y as i32) * 8;
                for qi in 0..4 {
                    let bx = (qi % 2) * 4;
                    let by = (qi / 2) * 4;
                    let (mv_row, mv_col) = chroma_mv[qi];
                    let mut ou = [[0u8; 4]; 4];
                    let mut ov = [[0u8; 4]; 4];
                    mc_block4(refplane.u.as_ref(), refplane.cw, refplane.ch,
                             mb_cox + bx as i32, mb_coy + by as i32, mv_row, mv_col, filters,
                             &mut ou, &mut had_frac);
                    mc_block4(refplane.v.as_ref(), refplane.cw, refplane.ch,
                             mb_cox + bx as i32, mb_coy + by as i32, mv_row, mv_col, filters,
                             &mut ov, &mut had_frac);
                    let uo = uoff + by * cs + bx;
                    let vo = voff + by * cs + bx;
                    let buf = pl.buf.as_mut();
                    for r in 0..4 {
                        for c in 0..4 {
                            buf[uo + r * cs + c] = ou[r][c];
                            buf[vo + r * cs + c] = ov[r][c];
                        }
                    }
                    idct4x4_add(&coeffs[16 + qi], buf, uo, cs);
                    idct4x4_add(&coeffs[20 + qi], buf, vo, cs);
                }
            }

            // ---- loop-filter parameters for this MB (RFC 6386 15.2) ----
            let mut lvl = dec.filt.level;
            if dec.seg.enabled {
                lvl = if dec.seg.absolute {
                    dec.seg.filter[segid as usize & 3]
                } else {
                    lvl + dec.seg.filter[segid as usize & 3]
                };
            }
            lvl = lvl.clamp(0, 63);
            if dec.filt.delta_enabled {
                lvl += dec.filt.ref_delta[mbi.ref_frame as usize & 3];
                if mbi.ref_frame == 0 {
                    if ymode_intra as usize == B_PRED {
                        lvl += dec.filt.mode_delta[0];
                    }
                } else if mbi.y_mode == ZEROMV {
                    lvl += dec.filt.mode_delta[1];
                } else if mbi.y_mode == SPLITMV {
                    lvl += dec.filt.mode_delta[3];
                } else {
                    lvl += dec.filt.mode_delta[2];
                }
                lvl = lvl.clamp(0, 63);
            }
            let inner = nonzero || (mbi.ref_frame == 0 && ymode_intra as usize == B_PRED)
                || (mbi.ref_frame != 0 && mbi.y_mode == SPLITMV);
            let fi = finfo.as_mut();
            fi[(mb_y * mb_w + mb_x) * 2] = lvl as u8;
            fi[(mb_y * mb_w + mb_x) * 2 + 1] = inner as u8;

            dec.mbg.as_mut().unwrap().set(my, mx, mbi);
        }
    }

    if dec.filt.level > 0 {
        loop_filter(&mut pl, &finfo, mb_w, mb_h, dec.filt.sharpness, dec.filt.simple, keyframe);
    }

    // --- reference-frame bookkeeping (RFC 6386 9.7/9.8, order matters: copy
    // before refresh, exactly as dixie's decode_frame does it) ---
    let cur = RefBuf::from_planes(&pl)?;
    if copy_arf == 1 {
        dec.altref = dec.last.as_ref().and_then(|r| r.dup());
    } else if copy_arf == 2 {
        dec.altref = dec.golden.as_ref().and_then(|r| r.dup());
    }
    if copy_gf == 1 {
        dec.golden = dec.last.as_ref().and_then(|r| r.dup());
    } else if copy_gf == 2 {
        dec.golden = dec.altref.as_ref().and_then(|r| r.dup());
    }
    if refresh_gf {
        dec.golden = Some(cur.dup()?);
    }
    if refresh_arf {
        dec.altref = Some(cur.dup()?);
    }
    if refresh_last {
        dec.last = Some(cur);
    }

    if let Some((cp, mvp, ymp, uvmp)) = saved {
        dec.coeff_probs = cp;
        dec.mv_probs = mvp;
        dec.y_mode_probs = ymp;
        dec.uv_mode_probs = uvmp;
    }

    dec.inited = true;
    dec.had_frac_mv |= had_frac;

    if shown {
        let cw = (width as usize + 1) / 2;
        let ch = (height as usize + 1) / 2;
        let total = (width as usize).checked_mul(height as usize)?.checked_add(2 * cw * ch)?;
        let mut out = Buf::new(total)?;
        {
            let src = pl.buf.as_ref();
            let dst = out.as_mut();
            let mut o = 0usize;
            for r in 0..height as usize {
                let so = pl.yb + r * pl.ys;
                dst[o..o + width as usize].copy_from_slice(&src[so..so + width as usize]);
                o += width as usize;
            }
            for r in 0..ch {
                let so = pl.ub + r * pl.cs;
                dst[o..o + cw].copy_from_slice(&src[so..so + cw]);
                o += cw;
            }
            for r in 0..ch {
                let so = pl.vb + r * pl.cs;
                dst[o..o + cw].copy_from_slice(&src[so..so + cw]);
                o += cw;
            }
        }
        dec.out = Some(out);
    }

    Some(shown)
}

// ---------------------------------------------------------------------------
// C FFI. No global allocator exists for an opaque `Box<VideoDec>` in this
// `#![no_std]` crate, so the caller owns the storage (sized by
// `vp8_video_state_size()`) and we placement-new/drop into it -- the same
// shape `FrameArray` in imgbuf.rs uses for a typed kmalloc'd array.
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn vp8_video_state_size() -> usize {
    core::mem::size_of::<VideoDec>()
}

/// # Safety
/// `state` must point to at least `vp8_video_state_size()` writable, suitably
/// aligned bytes (align_of::<VideoDec>(), which is at most 8) that are not
/// already holding a live `VideoDec`.
#[no_mangle]
pub unsafe extern "C" fn vp8_video_init(state: *mut u8) {
    core::ptr::write(state as *mut VideoDec, VideoDec::new());
}

/// # Safety
/// `state` must be a pointer previously passed to `vp8_video_init` and not
/// yet freed.
#[no_mangle]
pub unsafe extern "C" fn vp8_video_free(state: *mut u8) {
    core::ptr::drop_in_place(state as *mut VideoDec);
}

/// Returns -1 on error/malformed input, 0 if decoded but not shown (an
/// invisible altref frame), 1 if decoded and shown (a new frame is available
/// via `vp8_video_yuv`).
///
/// # Safety
/// `state` must be a live, initialised decoder; `data` must point to `len`
/// readable bytes.
#[no_mangle]
pub unsafe extern "C" fn vp8_video_decode(state: *mut u8, data: *const u8, len: i32) -> i32 {
    if data.is_null() || len <= 0 {
        return -1;
    }
    let dec = &mut *(state as *mut VideoDec);
    let p = core::slice::from_raw_parts(data, len as usize);
    match decode_frame(dec, p) {
        Some(true) => 1,
        Some(false) => 0,
        None => -1,
    }
}

/// # Safety
/// `state` must be a live, initialised decoder.
#[no_mangle]
pub unsafe extern "C" fn vp8_video_width(state: *const u8) -> i32 {
    (*(state as *const VideoDec)).width
}

/// # Safety
/// `state` must be a live, initialised decoder.
#[no_mangle]
pub unsafe extern "C" fn vp8_video_height(state: *const u8) -> i32 {
    (*(state as *const VideoDec)).height
}

/// Pointer to the last SHOWN frame's tightly packed I420 (Y w*h, then U/V at
/// ceil(w/2) x ceil(h/2) each), valid until the next `vp8_video_decode` call.
/// Null if no shown frame has been decoded yet.
///
/// # Safety
/// `state` must be a live, initialised decoder.
#[no_mangle]
pub unsafe extern "C" fn vp8_video_yuv(state: *const u8) -> *const u8 {
    match &(*(state as *const VideoDec)).out {
        Some(b) => b.as_ref().as_ptr(),
        None => core::ptr::null(),
    }
}

/// # Safety
/// `state` must be a live, initialised decoder.
#[no_mangle]
pub unsafe extern "C" fn vp8_video_yuv_len(state: *const u8) -> i32 {
    match &(*(state as *const VideoDec)).out {
        Some(b) => b.len() as i32,
        None => 0,
    }
}

/// Whether ANY motion-compensated block decoded so far (across every
/// `vp8_video_decode` call on this state) used a non-full-pixel offset --
/// the negative-control gate's split between "must redden" and "must stay
/// green" cases (see tests/vp8.mk).
///
/// # Safety
/// `state` must be a live, initialised decoder.
#[no_mangle]
pub unsafe extern "C" fn vp8_video_had_frac_mv(state: *const u8) -> i32 {
    (*(state as *const VideoDec)).had_frac_mv as i32
}
