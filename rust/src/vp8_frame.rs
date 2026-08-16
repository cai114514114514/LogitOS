//! The VP8 key-frame driver: partitions, the macroblock loop, the loop-filter
//! pass, and YUV -> RGBA.

use crate::imgbuf::Buf;
use crate::vp8::*;
use crate::vp8_dec::*;
use crate::vp8_rec::*;
use crate::vp8_tables::*;

/// Per-macroblock information the loop filter needs, saved during decode
/// because by filter time the modes and coefficients are gone.
#[derive(Clone, Copy, Default)]
struct FInfo {
    level: u8,
    inner: bool,
}

fn le24(p: &[u8], i: usize) -> Option<usize> {
    Some(*p.get(i)? as usize | (*p.get(i + 1)? as usize) << 8 | (*p.get(i + 2)? as usize) << 16)
}

/// libwebp's exact YUV -> RGB, so our RGBA is comparable to `dwebp` byte for
/// byte. The 19077/26149/... constants and the two-stage shift are not "a"
/// BT.601 conversion, they are THE one every WebP on the web was decoded with.
#[inline]
fn clip8(v: i32) -> u8 {
    if (v & !((256 << 6) - 1)) == 0 {
        (v >> 6) as u8
    } else if v < 0 {
        0
    } else {
        255
    }
}

#[inline]
fn yuv_to_rgb(y: i32, u: i32, v: i32, out: &mut [u8]) {
    let y1 = (y * 19077) >> 8;
    out[0] = clip8(y1 + ((v * 26149) >> 8) - 14234);
    out[1] = clip8(y1 - ((u * 6419) >> 8) - ((v * 13320) >> 8) + 8708);
    out[2] = clip8(y1 + ((u * 33050) >> 8) - 17685);
    out[3] = 255;
}

pub fn decode_vp8_keyframe(p: &[u8]) -> Option<(i32, i32, Buf)> {
    let fh = parse_uncompressed(p)?;
    let mb_w = (fh.width + 15) / 16;
    let mb_h = (fh.height + 15) / 16;
    if mb_w == 0 || mb_h == 0 {
        return None;
    }

    let part0 = p.get(fh.data_start..fh.data_start + fh.first_part_size)?;
    let mut bd0 = Bool::new(part0);

    let mut dec = Dec::new();
    parse_compressed_header(&mut bd0, &mut dec)?;

    // Partition count sits between the filter header and the quantiser
    // indices; the partitions themselves start after a table of 3-byte sizes.
    let log2_parts = bd0.get_uint(2);
    let nparts = 1usize << log2_parts;
    let after0 = fh.data_start + fh.first_part_size;
    let table = 3 * (nparts - 1);
    if after0 + table > p.len() {
        return None;
    }
    let mut ranges = [(0usize, 0usize); MAX_PARTITIONS];
    let mut at = after0 + table;
    for i in 0..nparts {
        let sz = if i + 1 < nparts {
            le24(p, after0 + 3 * i)?
        } else {
            p.len().checked_sub(at)?
        };
        let end = at.checked_add(sz)?;
        if end > p.len() {
            return None;
        }
        ranges[i] = (at, end);
        at = end;
    }
    let mut toks: [Bool; MAX_PARTITIONS] =
        core::array::from_fn(|i| Bool::new(&p[ranges[i].0..ranges[i].1]));

    parse_quant_and_probs(&mut bd0, &mut dec);

    let mut pl = Planes::new(mb_w, mb_h)?;

    // Per-column state carried down the frame.
    let mut above_nz = Buf::zeroed(mb_w * NZ)?;
    let mut above_bmode = Buf::zeroed(mb_w * 4)?;
    let mut finfo = Buf::zeroed(mb_w * mb_h * 2)?;

    let seg_enabled = dec.seg.enabled;
    let update_map = dec.seg.update_map;
    let seg_probs = dec.seg.tree_probs;
    let use_skip = dec.use_skip_prob;
    let skip_p = dec.prob_skip_false;

    for mb_y in 0..mb_h {
        let mut left_nz = [0u8; NZ];
        let mut left_bmode = [B_DC_PRED as u8; 4];
        for mb_x in 0..mb_w {
            // ---- modes (always from partition 0) ----
            let seg = if seg_enabled && update_map {
                bd0.get_tree(&MB_SEGMENT_TREE, &seg_probs) as usize & 3
            } else {
                0
            };
            let skip = use_skip && bd0.get(skip_p) != 0;
            let ymode = bd0.get_tree(&KF_YMODE_TREE, &KF_YMODE_PROB) as usize;
            let mut bmodes = [0u8; 16];
            if ymode == B_PRED {
                for by in 0..4 {
                    for bx in 0..4 {
                        let a = if by == 0 {
                            above_bmode.as_ref()[mb_x * 4 + bx]
                        } else {
                            bmodes[(by - 1) * 4 + bx]
                        } as usize;
                        let l = if bx == 0 { left_bmode[by] } else { bmodes[by * 4 + bx - 1] }
                            as usize;
                        let probs = &KF_BMODE_PROBS[a.min(9)][l.min(9)];
                        bmodes[by * 4 + bx] = bd0.get_tree(&BMODE_TREE, probs) as u8;
                    }
                }
            } else {
                let b = bmode_of(ymode) as u8;
                bmodes = [b; 16];
            }
            let uvmode = bd0.get_tree(&UV_MODE_TREE, &KF_UV_MODE_PROB) as usize;
            for i in 0..4 {
                above_bmode.as_mut()[mb_x * 4 + i] = bmodes[12 + i];
                left_bmode[i] = bmodes[i * 4 + 3];
            }

            // ---- coefficients (from this row's token partition) ----
            let has_y2 = ymode != B_PRED;
            let q = dec.quant_for(seg);
            let mut coeffs = [[0i32; 16]; 25];
            let mut nonzero = false;
            let tok = &mut toks[mb_y & (nparts - 1)];
            let anz = above_nz.as_mut();

            if skip {
                for i in 0..8 {
                    anz[mb_x * NZ + i] = 0;
                    left_nz[i] = 0;
                }
                // Only a macroblock that HAS a Y2 block clears the Y2 context;
                // a B_PRED macroblock leaves it as the previous one set it.
                if has_y2 {
                    anz[mb_x * NZ + 8] = 0;
                    left_nz[8] = 0;
                }
            } else {
                let first = if has_y2 {
                    let ctx = (anz[mb_x * NZ + 8] + left_nz[8]) as usize;
                    let end = decode_coeffs(tok, &dec.coeff_probs[1], ctx, 0, q.y2,
                                            &mut coeffs[24]);
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
                        let end = decode_coeffs(tok, &dec.coeff_probs[ytype], ctx, first,
                                                q.y1, &mut coeffs[by * 4 + bx]);
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
                    let y2 = coeffs[24];
                    iwht4x4(&y2, &mut dc);
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
            let buf = pl.buf.as_mut();

            if ymode == B_PRED {
                // Captured ONCE for the whole macroblock: every right-edge
                // subblock, in all four rows, predicts from the row above the
                // MACROBLOCK. See the note at the top of vp8_dec.rs.
                let tr: [u8; 4] = if mb_y == 0 {
                    [127; 4]
                } else if cfg!(feature = "vp8-tr-from-subblock") {
                    // NEGATIVE CONTROL: see Cargo.toml [features].
                    [0; 4]
                } else if mb_x + 1 == mb_w {
                    let v = buf[yoff - ys + 15];
                    [v; 4]
                } else {
                    [
                        buf[yoff - ys + 16], buf[yoff - ys + 17],
                        buf[yoff - ys + 18], buf[yoff - ys + 19],
                    ]
                };
                for by in 0..4 {
                    for bx in 0..4 {
                        let o = yoff + by * 4 * ys + bx * 4;
                        let mut a = [0u8; 9];
                        a[0] = buf[o - ys - 1];
                        for i in 0..4 {
                            a[1 + i] = buf[o - ys + i];
                        }
                        if bx == 3 && !cfg!(feature = "vp8-tr-from-subblock") {
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
                pred_block(buf, yoff, ys, 16, ymode, mb_x > 0, mb_y > 0);
                for by in 0..4 {
                    for bx in 0..4 {
                        idct4x4_add(&coeffs[by * 4 + bx], buf,
                                    yoff + by * 4 * ys + bx * 4, ys);
                    }
                }
            }
            pred_block(buf, uoff, cs, 8, uvmode, mb_x > 0, mb_y > 0);
            pred_block(buf, voff, cs, 8, uvmode, mb_x > 0, mb_y > 0);
            for by in 0..2 {
                for bx in 0..2 {
                    idct4x4_add(&coeffs[16 + by * 2 + bx], buf,
                                uoff + by * 4 * cs + bx * 4, cs);
                    idct4x4_add(&coeffs[20 + by * 2 + bx], buf,
                                voff + by * 4 * cs + bx * 4, cs);
                }
            }

            // ---- what the filter will need ----
            let mut lvl = dec.filt.level;
            if seg_enabled {
                lvl = if dec.seg.absolute {
                    dec.seg.filter[seg]
                } else {
                    lvl + dec.seg.filter[seg]
                };
            }
            lvl = lvl.clamp(0, 63);
            if dec.filt.delta_enabled {
                lvl += dec.filt.ref_delta[0]; // intra
                if ymode == B_PRED {
                    lvl += dec.filt.mode_delta[0];
                }
                lvl = lvl.clamp(0, 63);
            }
            let fi = finfo.as_mut();
            fi[(mb_y * mb_w + mb_x) * 2] = lvl as u8;
            fi[(mb_y * mb_w + mb_x) * 2 + 1] = (nonzero || ymode == B_PRED) as u8;
        }
    }

    if dec.filt.level > 0 {
        loop_filter(&mut pl, &finfo, mb_w, mb_h, dec.filt.sharpness, dec.filt.simple);
    }

    // ---- YUV -> RGBA, cropped to the coded dimensions ----
    let w = fh.width;
    let h = fh.height;
    let mut out = Buf::new(w.checked_mul(h)?.checked_mul(4)?)?;
    {
        let src = pl.buf.as_ref();
        let dst = out.as_mut();
        for y in 0..h {
            let yrow = pl.yb + y * pl.ys;
            let crow = (y >> 1) * pl.cs;
            for x in 0..w {
                let yv = src[yrow + x] as i32;
                let uu = src[pl.ub + crow + (x >> 1)] as i32;
                let vv = src[pl.vb + crow + (x >> 1)] as i32;
                let o = (y * w + x) * 4;
                yuv_to_rgb(yv, uu, vv, &mut dst[o..o + 4]);
            }
        }
    }
    Some((w as i32, h as i32, out))
}

// ---------------------------------------------------------------------------
// The loop-filter pass. Runs over the whole reconstructed frame AFTER every
// macroblock is in place, because intra prediction reads unfiltered
// neighbours. Within a macroblock the order is fixed: left edge, interior
// vertical edges, top edge, interior horizontal edges -- each filter reads
// what the previous one wrote.
// ---------------------------------------------------------------------------

fn loop_filter(pl: &mut Planes, finfo: &Buf, mb_w: usize, mb_h: usize,
               sharpness: i32, simple: bool) {
    let ys = pl.ys;
    let cs = pl.cs;
    let yb = pl.yb;
    let ub = pl.ub;
    let vb = pl.vb;
    let fi = finfo.as_ref();
    let buf = pl.buf.as_mut();

    for mb_y in 0..mb_h {
        for mb_x in 0..mb_w {
            let lvl = fi[(mb_y * mb_w + mb_x) * 2] as i32;
            if lvl == 0 {
                continue;
            }
            let inner = fi[(mb_y * mb_w + mb_x) * 2 + 1] != 0;

            let mut il = lvl;
            if sharpness > 0 {
                il >>= if sharpness > 4 { 2 } else { 1 };
                if il > 9 - sharpness {
                    il = 9 - sharpness;
                }
            }
            if il < 1 {
                il = 1;
            }
            // Key frame thresholds (RFC 6386 §15.2).
            let hev_t = if lvl >= 40 { 2 } else if lvl >= 15 { 1 } else { 0 };
            let mbedge = (lvl + 2) * 2 + il;
            let subedge = lvl * 2 + il;

            let yo = yb + mb_y * 16 * ys + mb_x * 16;
            let uo = ub + mb_y * 8 * cs + mb_x * 8;
            let vo = vb + mb_y * 8 * cs + mb_x * 8;

            if simple {
                // Luma only, and only the 4-tap adjustment.
                if mb_x > 0 {
                    for r in 0..16 {
                        let b = yo + r * ys;
                        simple_filter(mbedge, buf, b - 2, b - 1, b, b + 1);
                    }
                }
                if inner {
                    for &dx in &[4usize, 8, 12] {
                        for r in 0..16 {
                            let b = yo + r * ys + dx;
                            simple_filter(subedge, buf, b - 2, b - 1, b, b + 1);
                        }
                    }
                }
                if mb_y > 0 {
                    for c in 0..16 {
                        let b = yo + c;
                        simple_filter(mbedge, buf, b - 2 * ys, b - ys, b, b + ys);
                    }
                }
                if inner {
                    for &dy in &[4usize, 8, 12] {
                        for c in 0..16 {
                            let b = yo + dy * ys + c;
                            simple_filter(subedge, buf, b - 2 * ys, b - ys, b, b + ys);
                        }
                    }
                }
                continue;
            }

            // --- normal filter: luma and chroma ---
            if mb_x > 0 {
                for r in 0..16 {
                    mb_filter(hev_t, il, mbedge, buf, vidx(yo + r * ys, ys));
                }
                for r in 0..8 {
                    mb_filter(hev_t, il, mbedge, buf, vidx(uo + r * cs, cs));
                    mb_filter(hev_t, il, mbedge, buf, vidx(vo + r * cs, cs));
                }
            }
            if inner {
                for &dx in &[4usize, 8, 12] {
                    for r in 0..16 {
                        subblock_filter(hev_t, il, subedge, buf, vidx(yo + r * ys + dx, ys));
                    }
                }
                for r in 0..8 {
                    subblock_filter(hev_t, il, subedge, buf, vidx(uo + r * cs + 4, cs));
                    subblock_filter(hev_t, il, subedge, buf, vidx(vo + r * cs + 4, cs));
                }
            }
            if mb_y > 0 {
                for c in 0..16 {
                    mb_filter(hev_t, il, mbedge, buf, hidx(yo + c, ys));
                }
                for c in 0..8 {
                    mb_filter(hev_t, il, mbedge, buf, hidx(uo + c, cs));
                    mb_filter(hev_t, il, mbedge, buf, hidx(vo + c, cs));
                }
            }
            if inner {
                for &dy in &[4usize, 8, 12] {
                    for c in 0..16 {
                        subblock_filter(hev_t, il, subedge, buf, hidx(yo + dy * ys + c, ys));
                    }
                }
                for c in 0..8 {
                    subblock_filter(hev_t, il, subedge, buf, hidx(uo + 4 * cs + c, cs));
                    subblock_filter(hev_t, il, subedge, buf, hidx(vo + 4 * cs + c, cs));
                }
            }
        }
    }
}
