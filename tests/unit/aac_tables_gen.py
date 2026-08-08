#!/usr/bin/env python3
"""tests/unit/aac_tables_gen.py -- emit c/lib/audio/aac_tables.h.

Same shape, and the same reasoning, as tests/unit/mp3_tables_gen.py. An AAC
decoder needs two kinds of constant: bitstream tables that only the standard
can tell you (the eleven spectral Huffman codebooks and the scalefactor
codebook of ISO/IEC 14496-3 Table 4.A.2 ff., the scalefactor band boundaries
of Table 4.130 ff., the TNS band limits), and coefficients that are pure
formulas (the sine and Kaiser-Bessel-derived windows).  This script holds the
first kind as data and computes the second kind, so the C side contains no
transcendental call at table-build time.

PROVENANCE.  The bitstream tables are the published tables of ISO/IEC 14496-3
(MPEG-4 Audio) -- they are data, not code, and every AAC implementation
contains the same numbers.  They were transcribed from the arrays FFmpeg n6.1
publishes in libavcodec/aactab.c, which is the same route mp3_tables_gen.py
took through LAME, and then checked structurally here rather than by eye.

WHAT "CHECKED" MEANS, AND WHY IT IS NOT A FORMALITY.  There are 1401 spectral
codewords plus 121 scalefactor codewords below.  A single wrong length or
wrong code would not crash anything; it would desynchronise one codebook and
produce plausible-sounding garbage in the bands that use it.  So before this
script will emit a single byte it proves, for every codebook:

  * Kraft equality: sum of 2^-len is exactly 1.  A wrong length breaks it.
  * prefix-freeness: no codeword is a prefix of another.  A wrong code
    breaks it.
  * canonicality: sorting by (length, code) reproduces the codes by the
    canonical assignment starting at zero.  This is not required by the
    standard -- it is a property these particular tables happen to have --
    and checking it is what licenses the compact (count[], sym[]) form the C
    decoder uses instead of a 2^19-entry lookup table.

and for every scalefactor band table: offsets strictly increasing, starting at
0, ending exactly at the transform length, and a band count matching the
published num_swb.

Regenerate with:
    python3 tests/unit/aac_tables_gen.py c/lib/audio/aac_tables.h
The output is committed; a normal build never runs this script.
"""

import math
import sys

# --- published bitstream tables -------------------------------------------
# SPECTRAL[cb] = (codes, lengths), indexed by codebook number 1..11.
# The symbol index within each codebook is the tuple index in the codebook's
# own ordering: for the 4-tuple books (1..4) it is
# ((w*3+x)*3+y)*3+z with each of w,x,y,z in 0..2 (unsigned books map that to
# -1..1), and for the 2-tuple books (5..11) it is y*mod+z with mod = 9, 8, 13
# and 17 respectively.  aac.c reconstructs the tuple from the index, so the
# ordering here is load-bearing and is exactly the published one.

SPECTRAL = {
    1: (
        [0x7f8, 0x1f1, 0x7fd, 0x3f5, 0x68, 0x3f0, 0x7f7, 0x1ec, 0x7f5, 0x3f1, 0x72, 0x3f4, 0x74, 0x11, 0x76, 0x1eb, 0x6c, 0x3f6, 0x7fc, 0x1e1, 0x7f1, 0x1f0, 0x61, 0x1f6, 0x7f2, 0x1ea, 0x7fb, 0x1f2, 0x69, 0x1ed, 0x77, 0x17, 0x6f, 0x1e6, 0x64, 0x1e5, 0x67, 0x15, 0x62, 0x12, 0x0, 0x14, 0x65, 0x16, 0x6d, 0x1e9, 0x63, 0x1e4, 0x6b, 0x13, 0x71, 0x1e3, 0x70, 0x1f3, 0x7fe, 0x1e7, 0x7f3, 0x1ef, 0x60, 0x1ee, 0x7f0, 0x1e2, 0x7fa, 0x3f3, 0x6a, 0x1e8, 0x75, 0x10, 0x73, 0x1f4, 0x6e, 0x3f7, 0x7f6, 0x1e0, 0x7f9, 0x3f2, 0x66, 0x1f5, 0x7ff, 0x1f7, 0x7f4],
        [11, 9, 11, 10, 7, 10, 11, 9, 11, 10, 7, 10, 7, 5, 7, 9, 7, 10, 11, 9, 11, 9, 7, 9, 11, 9, 11, 9, 7, 9, 7, 5, 7, 9, 7, 9, 7, 5, 7, 5, 1, 5, 7, 5, 7, 9, 7, 9, 7, 5, 7, 9, 7, 9, 11, 9, 11, 9, 7, 9, 11, 9, 11, 10, 7, 9, 7, 5, 7, 9, 7, 10, 11, 9, 11, 10, 7, 9, 11, 9, 11]),
    2: (
        [0x1f3, 0x6f, 0x1fd, 0xeb, 0x23, 0xea, 0x1f7, 0xe8, 0x1fa, 0xf2, 0x2d, 0x70, 0x20, 0x6, 0x2b, 0x6e, 0x28, 0xe9, 0x1f9, 0x66, 0xf8, 0xe7, 0x1b, 0xf1, 0x1f4, 0x6b, 0x1f5, 0xec, 0x2a, 0x6c, 0x2c, 0xa, 0x27, 0x67, 0x1a, 0xf5, 0x24, 0x8, 0x1f, 0x9, 0x0, 0x7, 0x1d, 0xb, 0x30, 0xef, 0x1c, 0x64, 0x1e, 0xc, 0x29, 0xf3, 0x2f, 0xf0, 0x1fc, 0x71, 0x1f2, 0xf4, 0x21, 0xe6, 0xf7, 0x68, 0x1f8, 0xee, 0x22, 0x65, 0x31, 0x2, 0x26, 0xed, 0x25, 0x6a, 0x1fb, 0x72, 0x1fe, 0x69, 0x2e, 0xf6, 0x1ff, 0x6d, 0x1f6],
        [9, 7, 9, 8, 6, 8, 9, 8, 9, 8, 6, 7, 6, 5, 6, 7, 6, 8, 9, 7, 8, 8, 6, 8, 9, 7, 9, 8, 6, 7, 6, 5, 6, 7, 6, 8, 6, 5, 6, 5, 3, 5, 6, 5, 6, 8, 6, 7, 6, 5, 6, 8, 6, 8, 9, 7, 9, 8, 6, 8, 8, 7, 9, 8, 6, 7, 6, 4, 6, 8, 6, 7, 9, 7, 9, 7, 6, 8, 9, 7, 9]),
    3: (
        [0x0, 0x9, 0xef, 0xb, 0x19, 0xf0, 0x1eb, 0x1e6, 0x3f2, 0xa, 0x35, 0x1ef, 0x34, 0x37, 0x1e9, 0x1ed, 0x1e7, 0x3f3, 0x1ee, 0x3ed, 0x1ffa, 0x1ec, 0x1f2, 0x7f9, 0x7f8, 0x3f8, 0xff8, 0x8, 0x38, 0x3f6, 0x36, 0x75, 0x3f1, 0x3eb, 0x3ec, 0xff4, 0x18, 0x76, 0x7f4, 0x39, 0x74, 0x3ef, 0x1f3, 0x1f4, 0x7f6, 0x1e8, 0x3ea, 0x1ffc, 0xf2, 0x1f1, 0xffb, 0x3f5, 0x7f3, 0xffc, 0xee, 0x3f7, 0x7ffe, 0x1f0, 0x7f5, 0x7ffd, 0x1ffb, 0x3ffa, 0xffff, 0xf1, 0x3f0, 0x3ffc, 0x1ea, 0x3ee, 0x3ffb, 0xff6, 0xffa, 0x7ffc, 0x7f2, 0xff5, 0xfffe, 0x3f4, 0x7f7, 0x7ffb, 0xff7, 0xff9, 0x7ffa],
        [1, 4, 8, 4, 5, 8, 9, 9, 10, 4, 6, 9, 6, 6, 9, 9, 9, 10, 9, 10, 13, 9, 9, 11, 11, 10, 12, 4, 6, 10, 6, 7, 10, 10, 10, 12, 5, 7, 11, 6, 7, 10, 9, 9, 11, 9, 10, 13, 8, 9, 12, 10, 11, 12, 8, 10, 15, 9, 11, 15, 13, 14, 16, 8, 10, 14, 9, 10, 14, 12, 12, 15, 11, 12, 16, 10, 11, 15, 12, 12, 15]),
    4: (
        [0x7, 0x16, 0xf6, 0x18, 0x8, 0xef, 0x1ef, 0xf3, 0x7f8, 0x19, 0x17, 0xed, 0x15, 0x1, 0xe2, 0xf0, 0x70, 0x3f0, 0x1ee, 0xf1, 0x7fa, 0xee, 0xe4, 0x3f2, 0x7f6, 0x3ef, 0x7fd, 0x5, 0x14, 0xf2, 0x9, 0x4, 0xe5, 0xf4, 0xe8, 0x3f4, 0x6, 0x2, 0xe7, 0x3, 0x0, 0x6b, 0xe3, 0x69, 0x1f3, 0xeb, 0xe6, 0x3f6, 0x6e, 0x6a, 0x1f4, 0x3ec, 0x1f0, 0x3f9, 0xf5, 0xec, 0x7fb, 0xea, 0x6f, 0x3f7, 0x7f9, 0x3f3, 0xfff, 0xe9, 0x6d, 0x3f8, 0x6c, 0x68, 0x1f5, 0x3ee, 0x1f2, 0x7f4, 0x7f7, 0x3f1, 0xffe, 0x3ed, 0x1f1, 0x7f5, 0x7fe, 0x3f5, 0x7fc],
        [4, 5, 8, 5, 4, 8, 9, 8, 11, 5, 5, 8, 5, 4, 8, 8, 7, 10, 9, 8, 11, 8, 8, 10, 11, 10, 11, 4, 5, 8, 4, 4, 8, 8, 8, 10, 4, 4, 8, 4, 4, 7, 8, 7, 9, 8, 8, 10, 7, 7, 9, 10, 9, 10, 8, 8, 11, 8, 7, 10, 11, 10, 12, 8, 7, 10, 7, 7, 9, 10, 9, 11, 11, 10, 12, 10, 9, 11, 11, 10, 11]),
    5: (
        [0x1fff, 0xff7, 0x7f4, 0x7e8, 0x3f1, 0x7ee, 0x7f9, 0xff8, 0x1ffd, 0xffd, 0x7f1, 0x3e8, 0x1e8, 0xf0, 0x1ec, 0x3ee, 0x7f2, 0xffa, 0xff4, 0x3ef, 0x1f2, 0xe8, 0x70, 0xec, 0x1f0, 0x3ea, 0x7f3, 0x7eb, 0x1eb, 0xea, 0x1a, 0x8, 0x19, 0xee, 0x1ef, 0x7ed, 0x3f0, 0xf2, 0x73, 0xb, 0x0, 0xa, 0x71, 0xf3, 0x7e9, 0x7ef, 0x1ee, 0xef, 0x18, 0x9, 0x1b, 0xeb, 0x1e9, 0x7ec, 0x7f6, 0x3eb, 0x1f3, 0xed, 0x72, 0xe9, 0x1f1, 0x3ed, 0x7f7, 0xff6, 0x7f0, 0x3e9, 0x1ed, 0xf1, 0x1ea, 0x3ec, 0x7f8, 0xff9, 0x1ffc, 0xffc, 0xff5, 0x7ea, 0x3f3, 0x3f2, 0x7f5, 0xffb, 0x1ffe],
        [13, 12, 11, 11, 10, 11, 11, 12, 13, 12, 11, 10, 9, 8, 9, 10, 11, 12, 12, 10, 9, 8, 7, 8, 9, 10, 11, 11, 9, 8, 5, 4, 5, 8, 9, 11, 10, 8, 7, 4, 1, 4, 7, 8, 11, 11, 9, 8, 5, 4, 5, 8, 9, 11, 11, 10, 9, 8, 7, 8, 9, 10, 11, 12, 11, 10, 9, 8, 9, 10, 11, 12, 13, 12, 12, 11, 10, 10, 11, 12, 13]),
    6: (
        [0x7fe, 0x3fd, 0x1f1, 0x1eb, 0x1f4, 0x1ea, 0x1f0, 0x3fc, 0x7fd, 0x3f6, 0x1e5, 0xea, 0x6c, 0x71, 0x68, 0xf0, 0x1e6, 0x3f7, 0x1f3, 0xef, 0x32, 0x27, 0x28, 0x26, 0x31, 0xeb, 0x1f7, 0x1e8, 0x6f, 0x2e, 0x8, 0x4, 0x6, 0x29, 0x6b, 0x1ee, 0x1ef, 0x72, 0x2d, 0x2, 0x0, 0x3, 0x2f, 0x73, 0x1fa, 0x1e7, 0x6e, 0x2b, 0x7, 0x1, 0x5, 0x2c, 0x6d, 0x1ec, 0x1f9, 0xee, 0x30, 0x24, 0x2a, 0x25, 0x33, 0xec, 0x1f2, 0x3f8, 0x1e4, 0xed, 0x6a, 0x70, 0x69, 0x74, 0xf1, 0x3fa, 0x7ff, 0x3f9, 0x1f6, 0x1ed, 0x1f8, 0x1e9, 0x1f5, 0x3fb, 0x7fc],
        [11, 10, 9, 9, 9, 9, 9, 10, 11, 10, 9, 8, 7, 7, 7, 8, 9, 10, 9, 8, 6, 6, 6, 6, 6, 8, 9, 9, 7, 6, 4, 4, 4, 6, 7, 9, 9, 7, 6, 4, 4, 4, 6, 7, 9, 9, 7, 6, 4, 4, 4, 6, 7, 9, 9, 8, 6, 6, 6, 6, 6, 8, 9, 10, 9, 8, 7, 7, 7, 7, 8, 10, 11, 10, 9, 9, 9, 9, 9, 10, 11]),
    7: (
        [0x0, 0x5, 0x37, 0x74, 0xf2, 0x1eb, 0x3ed, 0x7f7, 0x4, 0xc, 0x35, 0x71, 0xec, 0xee, 0x1ee, 0x1f5, 0x36, 0x34, 0x72, 0xea, 0xf1, 0x1e9, 0x1f3, 0x3f5, 0x73, 0x70, 0xeb, 0xf0, 0x1f1, 0x1f0, 0x3ec, 0x3fa, 0xf3, 0xed, 0x1e8, 0x1ef, 0x3ef, 0x3f1, 0x3f9, 0x7fb, 0x1ed, 0xef, 0x1ea, 0x1f2, 0x3f3, 0x3f8, 0x7f9, 0x7fc, 0x3ee, 0x1ec, 0x1f4, 0x3f4, 0x3f7, 0x7f8, 0xffd, 0xffe, 0x7f6, 0x3f0, 0x3f2, 0x3f6, 0x7fa, 0x7fd, 0xffc, 0xfff],
        [1, 3, 6, 7, 8, 9, 10, 11, 3, 4, 6, 7, 8, 8, 9, 9, 6, 6, 7, 8, 8, 9, 9, 10, 7, 7, 8, 8, 9, 9, 10, 10, 8, 8, 9, 9, 10, 10, 10, 11, 9, 8, 9, 9, 10, 10, 11, 11, 10, 9, 9, 10, 10, 11, 12, 12, 11, 10, 10, 10, 11, 11, 12, 12]),
    8: (
        [0xe, 0x5, 0x10, 0x30, 0x6f, 0xf1, 0x1fa, 0x3fe, 0x3, 0x0, 0x4, 0x12, 0x2c, 0x6a, 0x75, 0xf8, 0xf, 0x2, 0x6, 0x14, 0x2e, 0x69, 0x72, 0xf5, 0x2f, 0x11, 0x13, 0x2a, 0x32, 0x6c, 0xec, 0xfa, 0x71, 0x2b, 0x2d, 0x31, 0x6d, 0x70, 0xf2, 0x1f9, 0xef, 0x68, 0x33, 0x6b, 0x6e, 0xee, 0xf9, 0x3fc, 0x1f8, 0x74, 0x73, 0xed, 0xf0, 0xf6, 0x1f6, 0x1fd, 0x3fd, 0xf3, 0xf4, 0xf7, 0x1f7, 0x1fb, 0x1fc, 0x3ff],
        [5, 4, 5, 6, 7, 8, 9, 10, 4, 3, 4, 5, 6, 7, 7, 8, 5, 4, 4, 5, 6, 7, 7, 8, 6, 5, 5, 6, 6, 7, 8, 8, 7, 6, 6, 6, 7, 7, 8, 9, 8, 7, 6, 7, 7, 8, 8, 10, 9, 7, 7, 8, 8, 8, 9, 9, 10, 8, 8, 8, 9, 9, 9, 10]),
    9: (
        [0x0, 0x5, 0x37, 0xe7, 0x1de, 0x3ce, 0x3d9, 0x7c8, 0x7cd, 0xfc8, 0xfdd, 0x1fe4, 0x1fec, 0x4, 0xc, 0x35, 0x72, 0xea, 0xed, 0x1e2, 0x3d1, 0x3d3, 0x3e0, 0x7d8, 0xfcf, 0xfd5, 0x36, 0x34, 0x71, 0xe8, 0xec, 0x1e1, 0x3cf, 0x3dd, 0x3db, 0x7d0, 0xfc7, 0xfd4, 0xfe4, 0xe6, 0x70, 0xe9, 0x1dd, 0x1e3, 0x3d2, 0x3dc, 0x7cc, 0x7ca, 0x7de, 0xfd8, 0xfea, 0x1fdb, 0x1df, 0xeb, 0x1dc, 0x1e6, 0x3d5, 0x3de, 0x7cb, 0x7dd, 0x7dc, 0xfcd, 0xfe2, 0xfe7, 0x1fe1, 0x3d0, 0x1e0, 0x1e4, 0x3d6, 0x7c5, 0x7d1, 0x7db, 0xfd2, 0x7e0, 0xfd9, 0xfeb, 0x1fe3, 0x1fe9, 0x7c4, 0x1e5, 0x3d7, 0x7c6, 0x7cf, 0x7da, 0xfcb, 0xfda, 0xfe3, 0xfe9, 0x1fe6, 0x1ff3, 0x1ff7, 0x7d3, 0x3d8, 0x3e1, 0x7d4, 0x7d9, 0xfd3, 0xfde, 0x1fdd, 0x1fd9, 0x1fe2, 0x1fea, 0x1ff1, 0x1ff6, 0x7d2, 0x3d4, 0x3da, 0x7c7, 0x7d7, 0x7e2, 0xfce, 0xfdb, 0x1fd8, 0x1fee, 0x3ff0, 0x1ff4, 0x3ff2, 0x7e1, 0x3df, 0x7c9, 0x7d6, 0xfca, 0xfd0, 0xfe5, 0xfe6, 0x1feb, 0x1fef, 0x3ff3, 0x3ff4, 0x3ff5, 0xfe0, 0x7ce, 0x7d5, 0xfc6, 0xfd1, 0xfe1, 0x1fe0, 0x1fe8, 0x1ff0, 0x3ff1, 0x3ff8, 0x3ff6, 0x7ffc, 0xfe8, 0x7df, 0xfc9, 0xfd7, 0xfdc, 0x1fdc, 0x1fdf, 0x1fed, 0x1ff5, 0x3ff9, 0x3ffb, 0x7ffd, 0x7ffe, 0x1fe7, 0xfcc, 0xfd6, 0xfdf, 0x1fde, 0x1fda, 0x1fe5, 0x1ff2, 0x3ffa, 0x3ff7, 0x3ffc, 0x3ffd, 0x7fff],
        [1, 3, 6, 8, 9, 10, 10, 11, 11, 12, 12, 13, 13, 3, 4, 6, 7, 8, 8, 9, 10, 10, 10, 11, 12, 12, 6, 6, 7, 8, 8, 9, 10, 10, 10, 11, 12, 12, 12, 8, 7, 8, 9, 9, 10, 10, 11, 11, 11, 12, 12, 13, 9, 8, 9, 9, 10, 10, 11, 11, 11, 12, 12, 12, 13, 10, 9, 9, 10, 11, 11, 11, 12, 11, 12, 12, 13, 13, 11, 9, 10, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 11, 10, 10, 11, 11, 12, 12, 13, 13, 13, 13, 13, 13, 11, 10, 10, 11, 11, 11, 12, 12, 13, 13, 14, 13, 14, 11, 10, 11, 11, 12, 12, 12, 12, 13, 13, 14, 14, 14, 12, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, 14, 15, 12, 11, 12, 12, 12, 13, 13, 13, 13, 14, 14, 15, 15, 13, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15]),
    10: (
        [0x22, 0x8, 0x1d, 0x26, 0x5f, 0xd3, 0x1cf, 0x3d0, 0x3d7, 0x3ed, 0x7f0, 0x7f6, 0xffd, 0x7, 0x0, 0x1, 0x9, 0x20, 0x54, 0x60, 0xd5, 0xdc, 0x1d4, 0x3cd, 0x3de, 0x7e7, 0x1c, 0x2, 0x6, 0xc, 0x1e, 0x28, 0x5b, 0xcd, 0xd9, 0x1ce, 0x1dc, 0x3d9, 0x3f1, 0x25, 0xb, 0xa, 0xd, 0x24, 0x57, 0x61, 0xcc, 0xdd, 0x1cc, 0x1de, 0x3d3, 0x3e7, 0x5d, 0x21, 0x1f, 0x23, 0x27, 0x59, 0x64, 0xd8, 0xdf, 0x1d2, 0x1e2, 0x3dd, 0x3ee, 0xd1, 0x55, 0x29, 0x56, 0x58, 0x62, 0xce, 0xe0, 0xe2, 0x1da, 0x3d4, 0x3e3, 0x7eb, 0x1c9, 0x5e, 0x5a, 0x5c, 0x63, 0xca, 0xda, 0x1c7, 0x1ca, 0x1e0, 0x3db, 0x3e8, 0x7ec, 0x1e3, 0xd2, 0xcb, 0xd0, 0xd7, 0xdb, 0x1c6, 0x1d5, 0x1d8, 0x3ca, 0x3da, 0x7ea, 0x7f1, 0x1e1, 0xd4, 0xcf, 0xd6, 0xde, 0xe1, 0x1d0, 0x1d6, 0x3d1, 0x3d5, 0x3f2, 0x7ee, 0x7fb, 0x3e9, 0x1cd, 0x1c8, 0x1cb, 0x1d1, 0x1d7, 0x1df, 0x3cf, 0x3e0, 0x3ef, 0x7e6, 0x7f8, 0xffa, 0x3eb, 0x1dd, 0x1d3, 0x1d9, 0x1db, 0x3d2, 0x3cc, 0x3dc, 0x3ea, 0x7ed, 0x7f3, 0x7f9, 0xff9, 0x7f2, 0x3ce, 0x1e4, 0x3cb, 0x3d8, 0x3d6, 0x3e2, 0x3e5, 0x7e8, 0x7f4, 0x7f5, 0x7f7, 0xffb, 0x7fa, 0x3ec, 0x3df, 0x3e1, 0x3e4, 0x3e6, 0x3f0, 0x7e9, 0x7ef, 0xff8, 0xffe, 0xffc, 0xfff],
        [6, 5, 6, 6, 7, 8, 9, 10, 10, 10, 11, 11, 12, 5, 4, 4, 5, 6, 7, 7, 8, 8, 9, 10, 10, 11, 6, 4, 5, 5, 6, 6, 7, 8, 8, 9, 9, 10, 10, 6, 5, 5, 5, 6, 7, 7, 8, 8, 9, 9, 10, 10, 7, 6, 6, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 8, 7, 6, 7, 7, 7, 8, 8, 8, 9, 10, 10, 11, 9, 7, 7, 7, 7, 8, 8, 9, 9, 9, 10, 10, 11, 9, 8, 8, 8, 8, 8, 9, 9, 9, 10, 10, 11, 11, 9, 8, 8, 8, 8, 8, 9, 9, 10, 10, 10, 11, 11, 10, 9, 9, 9, 9, 9, 9, 10, 10, 10, 11, 11, 12, 10, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 12, 11, 10, 9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 12, 11, 10, 10, 10, 10, 10, 10, 11, 11, 12, 12, 12, 12]),
    11: (
        [0x0, 0x6, 0x19, 0x3d, 0x9c, 0xc6, 0x1a7, 0x390, 0x3c2, 0x3df, 0x7e6, 0x7f3, 0xffb, 0x7ec, 0xffa, 0xffe, 0x38e, 0x5, 0x1, 0x8, 0x14, 0x37, 0x42, 0x92, 0xaf, 0x191, 0x1a5, 0x1b5, 0x39e, 0x3c0, 0x3a2, 0x3cd, 0x7d6, 0xae, 0x17, 0x7, 0x9, 0x18, 0x39, 0x40, 0x8e, 0xa3, 0xb8, 0x199, 0x1ac, 0x1c1, 0x3b1, 0x396, 0x3be, 0x3ca, 0x9d, 0x3c, 0x15, 0x16, 0x1a, 0x3b, 0x44, 0x91, 0xa5, 0xbe, 0x196, 0x1ae, 0x1b9, 0x3a1, 0x391, 0x3a5, 0x3d5, 0x94, 0x9a, 0x36, 0x38, 0x3a, 0x41, 0x8c, 0x9b, 0xb0, 0xc3, 0x19e, 0x1ab, 0x1bc, 0x39f, 0x38f, 0x3a9, 0x3cf, 0x93, 0xbf, 0x3e, 0x3f, 0x43, 0x45, 0x9e, 0xa7, 0xb9, 0x194, 0x1a2, 0x1ba, 0x1c3, 0x3a6, 0x3a7, 0x3bb, 0x3d4, 0x9f, 0x1a0, 0x8f, 0x8d, 0x90, 0x98, 0xa6, 0xb6, 0xc4, 0x19f, 0x1af, 0x1bf, 0x399, 0x3bf, 0x3b4, 0x3c9, 0x3e7, 0xa8, 0x1b6, 0xab, 0xa4, 0xaa, 0xb2, 0xc2, 0xc5, 0x198, 0x1a4, 0x1b8, 0x38c, 0x3a4, 0x3c4, 0x3c6, 0x3dd, 0x3e8, 0xad, 0x3af, 0x192, 0xbd, 0xbc, 0x18e, 0x197, 0x19a, 0x1a3, 0x1b1, 0x38d, 0x398, 0x3b7, 0x3d3, 0x3d1, 0x3db, 0x7dd, 0xb4, 0x3de, 0x1a9, 0x19b, 0x19c, 0x1a1, 0x1aa, 0x1ad, 0x1b3, 0x38b, 0x3b2, 0x3b8, 0x3ce, 0x3e1, 0x3e0, 0x7d2, 0x7e5, 0xb7, 0x7e3, 0x1bb, 0x1a8, 0x1a6, 0x1b0, 0x1b2, 0x1b7, 0x39b, 0x39a, 0x3ba, 0x3b5, 0x3d6, 0x7d7, 0x3e4, 0x7d8, 0x7ea, 0xba, 0x7e8, 0x3a0, 0x1bd, 0x1b4, 0x38a, 0x1c4, 0x392, 0x3aa, 0x3b0, 0x3bc, 0x3d7, 0x7d4, 0x7dc, 0x7db, 0x7d5, 0x7f0, 0xc1, 0x7fb, 0x3c8, 0x3a3, 0x395, 0x39d, 0x3ac, 0x3ae, 0x3c5, 0x3d8, 0x3e2, 0x3e6, 0x7e4, 0x7e7, 0x7e0, 0x7e9, 0x7f7, 0x190, 0x7f2, 0x393, 0x1be, 0x1c0, 0x394, 0x397, 0x3ad, 0x3c3, 0x3c1, 0x3d2, 0x7da, 0x7d9, 0x7df, 0x7eb, 0x7f4, 0x7fa, 0x195, 0x7f8, 0x3bd, 0x39c, 0x3ab, 0x3a8, 0x3b3, 0x3b9, 0x3d0, 0x3e3, 0x3e5, 0x7e2, 0x7de, 0x7ed, 0x7f1, 0x7f9, 0x7fc, 0x193, 0xffd, 0x3dc, 0x3b6, 0x3c7, 0x3cc, 0x3cb, 0x3d9, 0x3da, 0x7d3, 0x7e1, 0x7ee, 0x7ef, 0x7f5, 0x7f6, 0xffc, 0xfff, 0x19d, 0x1c2, 0xb5, 0xa1, 0x96, 0x97, 0x95, 0x99, 0xa0, 0xa2, 0xac, 0xa9, 0xb1, 0xb3, 0xbb, 0xc0, 0x18f, 0x4],
        [4, 5, 6, 7, 8, 8, 9, 10, 10, 10, 11, 11, 12, 11, 12, 12, 10, 5, 4, 5, 6, 7, 7, 8, 8, 9, 9, 9, 10, 10, 10, 10, 11, 8, 6, 5, 5, 6, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 8, 7, 6, 6, 6, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 8, 8, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 8, 8, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 8, 9, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 10, 8, 9, 8, 8, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 10, 10, 8, 10, 9, 8, 8, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 11, 8, 10, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 11, 11, 8, 11, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 10, 11, 11, 8, 11, 10, 9, 9, 10, 9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 8, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 9, 11, 10, 9, 9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 9, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 9, 12, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 9, 9, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 9, 5]),
}
SF_CODE = [0x3ffe8, 0x3ffe6, 0x3ffe7, 0x3ffe5, 0x7fff5, 0x7fff1, 0x7ffed, 0x7fff6, 0x7ffee, 0x7ffef, 0x7fff0, 0x7fffc, 0x7fffd, 0x7ffff, 0x7fffe, 0x7fff7, 0x7fff8, 0x7fffb, 0x7fff9, 0x3ffe4, 0x7fffa, 0x3ffe3, 0x1ffef, 0x1fff0, 0xfff5, 0x1ffee, 0xfff2, 0xfff3, 0xfff4, 0xfff1, 0x7ff6, 0x7ff7, 0x3ff9, 0x3ff5, 0x3ff7, 0x3ff3, 0x3ff6, 0x3ff2, 0x1ff7, 0x1ff5, 0xff9, 0xff7, 0xff6, 0x7f9, 0xff4, 0x7f8, 0x3f9, 0x3f7, 0x3f5, 0x1f8, 0x1f7, 0xfa, 0xf8, 0xf6, 0x79, 0x3a, 0x38, 0x1a, 0xb, 0x4, 0x0, 0xa, 0xc, 0x1b, 0x39, 0x3b, 0x78, 0x7a, 0xf7, 0xf9, 0x1f6, 0x1f9, 0x3f4, 0x3f6, 0x3f8, 0x7f5, 0x7f4, 0x7f6, 0x7f7, 0xff5, 0xff8, 0x1ff4, 0x1ff6, 0x1ff8, 0x3ff8, 0x3ff4, 0xfff0, 0x7ff4, 0xfff6, 0x7ff5, 0x3ffe2, 0x7ffd9, 0x7ffda, 0x7ffdb, 0x7ffdc, 0x7ffdd, 0x7ffde, 0x7ffd8, 0x7ffd2, 0x7ffd3, 0x7ffd4, 0x7ffd5, 0x7ffd6, 0x7fff2, 0x7ffdf, 0x7ffe7, 0x7ffe8, 0x7ffe9, 0x7ffea, 0x7ffeb, 0x7ffe6, 0x7ffe0, 0x7ffe1, 0x7ffe2, 0x7ffe3, 0x7ffe4, 0x7ffe5, 0x7ffd7, 0x7ffec, 0x7fff4, 0x7fff3]
SF_BITS = [18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 18, 19, 18, 17, 17, 16, 17, 16, 16, 16, 16, 15, 15, 14, 14, 14, 14, 14, 14, 13, 13, 12, 12, 12, 11, 12, 11, 10, 10, 10, 9, 9, 8, 8, 8, 7, 6, 6, 5, 4, 3, 1, 4, 4, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 10, 11, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 16, 15, 16, 15, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19]
SWB_OFFSET_1024_96 = [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 156, 172, 188, 212, 240, 276, 320, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960, 1024]
SWB_OFFSET_1024_64 = [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 100, 112, 124, 140, 156, 172, 192, 216, 240, 268, 304, 344, 384, 424, 464, 504, 544, 584, 624, 664, 704, 744, 784, 824, 864, 904, 944, 984, 1024]
SWB_OFFSET_1024_48 = [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 1024]
SWB_OFFSET_1024_32 = [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 88, 96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292, 320, 352, 384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736, 768, 800, 832, 864, 896, 928, 960, 992, 1024]
SWB_OFFSET_1024_24 = [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 52, 60, 68, 76, 84, 92, 100, 108, 116, 124, 136, 148, 160, 172, 188, 204, 220, 240, 260, 284, 308, 336, 364, 396, 432, 468, 508, 552, 600, 652, 704, 768, 832, 896, 960, 1024]
SWB_OFFSET_1024_16 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 100, 112, 124, 136, 148, 160, 172, 184, 196, 212, 228, 244, 260, 280, 300, 320, 344, 368, 396, 424, 456, 492, 532, 572, 616, 664, 716, 772, 832, 896, 960, 1024]
SWB_OFFSET_1024_8 = [0, 12, 24, 36, 48, 60, 72, 84, 96, 108, 120, 132, 144, 156, 172, 188, 204, 220, 236, 252, 268, 288, 308, 328, 348, 372, 396, 420, 448, 476, 508, 544, 580, 620, 664, 712, 764, 820, 880, 944, 1024]
SWB_OFFSET_128_96 = [0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 64, 92, 128]
SWB_OFFSET_128_48 = [0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80, 96, 112, 128]
SWB_OFFSET_128_24 = [0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 64, 76, 92, 108, 128]
SWB_OFFSET_128_16 = [0, 4, 8, 12, 16, 20, 24, 28, 32, 40, 48, 60, 72, 88, 108, 128]
SWB_OFFSET_128_8 = [0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 60, 72, 88, 108, 128]

# num_swb, indexed by sampling frequency index 0..12.
NUM_SWB_1024 = [41, 41, 47, 49, 49, 51, 47, 47, 43, 43, 43, 40, 40]
NUM_SWB_128 = [12, 12, 12, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15]

# The maximum number of scalefactor bands TNS may operate on, long and short.
TNS_MAX_BANDS_1024 = [31, 31, 34, 40, 42, 51, 46, 46, 42, 42, 42, 39, 39]
TNS_MAX_BANDS_128 = [9, 9, 10, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14]

# Which swb table each sampling frequency index uses.
SWB_1024_BY_SFI = ['96', '96', '64', '48', '48', '32', '24', '24', '16', '16',
                   '16', '8', '8']
SWB_128_BY_SFI = ['96', '96', '96', '48', '48', '48', '24', '24', '16', '16',
                  '16', '8', '8']

SAMPLE_RATES = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                16000, 12000, 11025, 8000, 7350, 0, 0, 0]


# --- validation ------------------------------------------------------------

def check_codebook(name, codes, bits):
    n = len(codes)
    if n != len(bits):
        raise SystemExit("%s: %d codes but %d lengths" % (name, n, len(bits)))

    kraft = sum(2.0 ** -b for b in bits)
    if kraft != 1.0:
        raise SystemExit("%s: Kraft sum is %.17g, not 1 -- a length is wrong"
                         % (name, kraft))

    order = sorted(range(n), key=lambda i: (bits[i], codes[i]))
    for a in range(n):
        i = order[a]
        for b in range(a + 1, n):
            j = order[b]
            if bits[i] <= bits[j] and (codes[j] >> (bits[j] - bits[i])) == codes[i]:
                raise SystemExit("%s: code %d (len %d) is a prefix of code %d "
                                 "(len %d) -- a code is wrong"
                                 % (name, i, bits[i], j, bits[j]))

    code = codes[order[0]]
    if code != 0:
        raise SystemExit("%s: shortest codeword is %d, not 0" % (name, code))
    prev = bits[order[0]]
    for idx in order[1:]:
        code = (code + 1) << (bits[idx] - prev)
        prev = bits[idx]
        if code != codes[idx]:
            raise SystemExit("%s: not canonical at symbol %d" % (name, idx))

    return order


def canonical_form(codes, bits):
    """(maxlen, count[1..maxlen], symbols in canonical order)."""
    order = sorted(range(len(codes)), key=lambda i: (bits[i], codes[i]))
    maxlen = max(bits)
    count = [0] * (maxlen + 1)
    for b in bits:
        count[b] += 1
    return maxlen, count[1:], order


def check_swb(name, offs, nswb, length):
    if len(offs) != nswb + 1:
        raise SystemExit("%s: %d offsets for %d bands" % (name, len(offs), nswb))
    if offs[0] != 0:
        raise SystemExit("%s: does not start at 0" % name)
    if offs[-1] != length:
        raise SystemExit("%s: ends at %d, not %d" % (name, offs[-1], length))
    for i in range(1, len(offs)):
        if offs[i] <= offs[i - 1]:
            raise SystemExit("%s: offsets not increasing at %d" % (name, i))


# --- computed windows ------------------------------------------------------

def bessel_i0(x):
    """Modified Bessel function of the first kind, order zero, by its series.
    Converges quickly for the arguments a Kaiser window needs (max pi*6)."""
    s = 1.0
    t = 1.0
    k = 1
    while True:
        t *= (x / (2.0 * k)) ** 2
        s += t
        if t < 1e-21 * s:
            return s
        k += 1
        if k > 200:
            return s


def kbd_window(n, alpha):
    """Kaiser-Bessel-derived window, ISO/IEC 14496-3 clause 4.6.11.2.
    Returns the first n/2 coefficients (the decoder mirrors them)."""
    half = n // 2
    w = []
    for j in range(half + 1):
        r = 2.0 * j / half - 1.0
        w.append(bessel_i0(math.pi * alpha * math.sqrt(max(0.0, 1.0 - r * r)))
                 / bessel_i0(math.pi * alpha))
    total = sum(w)
    out = []
    acc = 0.0
    for i in range(half):
        acc += w[i]
        out.append(math.sqrt(acc / total))
    return out


def sine_window(n):
    half = n // 2
    return [math.sin(math.pi / n * (i + 0.5)) for i in range(half)]


# The TNS filter coefficients are a formula, not a table: ISO/IEC 14496-3
# clause 4.6.9.3 inverse-quantises the transmitted index c (sign-extended from
# the coef_len bits actually read) as
#
#     iqfac   = ((1 << (res-1)) - 0.5) / (pi/2)
#     iqfac_m = ((1 << (res-1)) + 0.5) / (pi/2)
#     coef    = sin(c >= 0 ? c/iqfac : c/iqfac_m)
#
# with res = coef_res + 3 (3 or 4) EVEN WHEN coef_compress reduced the number
# of bits read -- that asymmetry is the easiest thing in TNS to get wrong, so
# the four resulting tables are cross-checked below against the values every
# decoder publishes.
TNS_PUBLISHED = {
    (0, 3): [0.0, 0.4338837391, 0.7818314825, 0.9749279122,
             -0.9848077530, -0.8660254038, -0.6427876097, -0.3420201433],
    (1, 3): [0.0, 0.4338837391, -0.6427876097, -0.3420201433],
    (0, 4): [0.0, 0.2079116908, 0.4067366431, 0.5877852523,
             0.7431448255, 0.8660254038, 0.9510565163, 0.9945218954,
             -0.9957341763, -0.9618256432, -0.8951632914, -0.7980172273,
             -0.6736956436, -0.5264321629, -0.3612416662, -0.1837495178],
    (1, 4): [0.0, 0.2079116908, 0.4067366431, 0.5877852523,
             -0.6736956436, -0.5264321629, -0.3612416662, -0.1837495178],
}


def tns_coefs(compress, res):
    coef_len = res - compress
    n = 1 << coef_len
    iqfac = ((1 << (res - 1)) - 0.5) / (math.pi / 2.0)
    iqfac_m = ((1 << (res - 1)) + 0.5) / (math.pi / 2.0)
    out = []
    for raw in range(n):
        c = raw - n if raw >= n // 2 else raw          # sign-extend coef_len bits
        out.append(math.sin(c / (iqfac if c >= 0 else iqfac_m)))
    want = TNS_PUBLISHED[(compress, res)]
    if len(want) != n:
        raise SystemExit("tns (%d,%d): %d values, published has %d"
                         % (compress, res, n, len(want)))
    for i in range(n):
        if abs(out[i] - want[i]) > 5e-10:
            raise SystemExit("tns (%d,%d)[%d]: formula gives %.10f, published "
                             "%.10f -- the sign extension or iqfac is wrong"
                             % (compress, res, i, out[i], want[i]))
    return out


# --- emit ------------------------------------------------------------------

def emit_u16(f, name, vals, per=12):
    f.write("static const uint16_t %s[%d] = {\n" % (name, len(vals)))
    for i in range(0, len(vals), per):
        f.write("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",\n")
    f.write("};\n")


def emit_u8(f, name, vals, per=16):
    f.write("static const uint8_t %s[%d] = {\n" % (name, len(vals)))
    for i in range(0, len(vals), per):
        f.write("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",\n")
    f.write("};\n")


def emit_i32(f, name, vals, per=12):
    f.write("static const int32_t %s[%d] = {\n" % (name, len(vals)))
    for i in range(0, len(vals), per):
        f.write("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",\n")
    f.write("};\n")


def emit_double(f, name, vals, per=4):
    f.write("static const double %s[%d] = {\n" % (name, len(vals)))
    for i in range(0, len(vals), per):
        f.write("    " + ", ".join(repr(v) for v in vals[i:i + per]) + ",\n")
    f.write("};\n")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "c/lib/audio/aac_tables.h"

    swb1024 = {
        '96': SWB_OFFSET_1024_96, '64': SWB_OFFSET_1024_64,
        '48': SWB_OFFSET_1024_48, '32': SWB_OFFSET_1024_32,
        '24': SWB_OFFSET_1024_24, '16': SWB_OFFSET_1024_16,
        '8': SWB_OFFSET_1024_8,
    }
    swb128 = {
        '96': SWB_OFFSET_128_96, '48': SWB_OFFSET_128_48,
        '24': SWB_OFFSET_128_24, '16': SWB_OFFSET_128_16,
        '8': SWB_OFFSET_128_8,
    }

    for sfi in range(13):
        check_swb("swb_1024_sfi%d" % sfi, swb1024[SWB_1024_BY_SFI[sfi]],
                  NUM_SWB_1024[sfi], 1024)
        check_swb("swb_128_sfi%d" % sfi, swb128[SWB_128_BY_SFI[sfi]],
                  NUM_SWB_128[sfi], 128)

    forms = {}
    total = 0
    for cb in range(1, 12):
        codes, bits = SPECTRAL[cb]
        check_codebook("cb%d" % cb, codes, bits)
        forms[cb] = canonical_form(codes, bits)
        total += len(codes)
    check_codebook("scalefactor", SF_CODE, SF_BITS)
    sf_form = canonical_form(SF_CODE, SF_BITS)

    f = open(out, "w")
    f.write("""/* c/lib/audio/aac_tables.h -- GENERATED by tests/unit/aac_tables_gen.py.
 * Do not edit by hand; edit the generator and re-run it.
 *
 * Bitstream tables (the eleven spectral Huffman codebooks, the scalefactor
 * codebook, the scalefactor band boundaries, the TNS band limits) are the
 * published tables of ISO/IEC 14496-3. The windows are computed from the
 * formulas in the standard, at generation time rather than at run time, for
 * the reason mp3_tables.h gives: a table built by two different libm
 * implementations would not be bit-identical between the host test and the
 * guest, and `make test-audio-codec-os` is exactly that comparison.
 *
 * The generator refuses to emit a codebook that is not an exactly complete,
 * prefix-free, CANONICAL code, which is what makes a transcription slip in
 * these %d codewords a build failure rather than a wrong sample in one band
 * of one frame. Canonicality is also what licenses the compact form below:
 * counts per length plus symbols in canonical order, decoded by the standard
 * incremental walk, instead of a 2^19-entry direct lookup.
 */
#ifndef LOGIT_AAC_TABLES_H
#define LOGIT_AAC_TABLES_H

#include <stdint.h>

""" % (total + len(SF_CODE)))

    f.write("/* --- spectral Huffman codebooks, ISO/IEC 14496-3 Table 4.A.2 ff. ---\n"
            " * For codebook c: aac_hcb_count_c[l-1] codewords have length l, and\n"
            " * aac_hcb_sym_c[] lists the symbol indices in canonical order. */\n")
    for cb in range(1, 12):
        maxlen, count, order = forms[cb]
        f.write("#define AAC_HCB%d_MAXLEN %d\n" % (cb, maxlen))
        emit_u8(f, "aac_hcb_count_%d" % cb, count)
        emit_u16(f, "aac_hcb_sym_%d" % cb, order)
        f.write("\n")

    maxlen, count, order = sf_form
    f.write("/* Scalefactor codebook, ISO/IEC 14496-3 Table 4.A.1. The symbol is\n"
            " * the scalefactor delta plus 60. */\n")
    f.write("#define AAC_SF_MAXLEN %d\n" % maxlen)
    emit_u8(f, "aac_sf_count", count)
    emit_u16(f, "aac_sf_sym", order)
    f.write("\n")

    f.write("/* --- scalefactor band offsets ------------------------------------ */\n")
    for k, v in sorted(swb1024.items(), key=lambda kv: -int(kv[0])):
        emit_u16(f, "aac_swb_1024_%s" % k, v)
    for k, v in sorted(swb128.items(), key=lambda kv: -int(kv[0])):
        emit_u16(f, "aac_swb_128_%s" % k, v)
    f.write("\nstatic const uint16_t * const aac_swb_1024[13] = {\n    %s\n};\n"
            % ", ".join("aac_swb_1024_%s" % s for s in SWB_1024_BY_SFI))
    f.write("static const uint16_t * const aac_swb_128[13] = {\n    %s\n};\n\n"
            % ", ".join("aac_swb_128_%s" % s for s in SWB_128_BY_SFI))
    emit_u8(f, "aac_num_swb_1024", NUM_SWB_1024)
    emit_u8(f, "aac_num_swb_128", NUM_SWB_128)
    emit_u8(f, "aac_tns_max_bands_1024", TNS_MAX_BANDS_1024)
    emit_u8(f, "aac_tns_max_bands_128", TNS_MAX_BANDS_128)
    f.write("\n")
    emit_i32(f, "aac_sample_rates", SAMPLE_RATES)
    f.write("\n")

    f.write("/* --- windows, ISO/IEC 14496-3 clause 4.6.11.2 ---------------------\n"
            " * Only the rising half is stored; the falling half is its mirror.\n"
            " * KBD uses alpha = 4 for the long window and 6 for the short one. */\n")
    emit_double(f, "aac_win_sine_1024", sine_window(2048))
    emit_double(f, "aac_win_sine_128", sine_window(256))
    emit_double(f, "aac_win_kbd_1024", kbd_window(2048, 4.0))
    emit_double(f, "aac_win_kbd_128", kbd_window(256, 6.0))

    f.write("\n/* --- TNS filter coefficients, ISO/IEC 14496-3 clause 4.6.9.3 -----\n"
            " * Indexed [coef_compress*2 + (coef_res_bits-3)][transmitted index].\n"
            " * Computed from the formula and cross-checked against the published\n"
            " * values by the generator. */\n")
    for compress in (0, 1):
        for res in (3, 4):
            emit_double(f, "aac_tns_coef_%d_%d" % (compress, res),
                        tns_coefs(compress, res))
    f.write("static const double * const aac_tns_coef[4] = {\n"
            "    aac_tns_coef_0_3, aac_tns_coef_0_4,\n"
            "    aac_tns_coef_1_3, aac_tns_coef_1_4,\n};\n")

    f.write("\n#endif /* LOGIT_AAC_TABLES_H */\n")
    f.close()

    sys.stderr.write("aac_tables_gen: %d spectral + %d scalefactor codewords "
                     "validated (Kraft, prefix-free, canonical), %d swb tables "
                     "checked -> %s\n"
                     % (total, len(SF_CODE), 13 * 2, out))


if __name__ == "__main__":
    main()
