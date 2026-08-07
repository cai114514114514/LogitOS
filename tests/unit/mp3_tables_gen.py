#!/usr/bin/env python3
"""tests/unit/mp3_tables_gen.py -- emit c/lib/audio/mp3_tables.h.

WHAT THIS IS.  A Layer III decoder needs two kinds of constant: bitstream
tables that only the standard can tell you (the Huffman codebooks of ISO/IEC
11172-3 Table 3-B.7, the scalefactor band boundaries of Table 3-B.8, the
synthesis window of Table 3-B.3), and coefficients that are pure formulas (the
IMDCT cosines, the block windows, the alias-reduction butterflies, the
intensity-stereo ratios).  This script holds the first kind as data and
computes the second kind, so the C side contains no transcendental function
call at all.  That matters twice over: the target build links mini-libc, which
has no double-precision libm, and a table computed at run time by two
different libm implementations would not be bit-identical between the host
test and the guest -- which is exactly the comparison `make test-audio-os`
performs.

PROVENANCE.  The bitstream tables are the published tables of ISO/IEC 11172-3.
They are data, not code, and every Layer III implementation contains the same
numbers; they were transcribed from the arrays LAME 3.100 publishes in
libmp3lame/tables.c and mpglib/{layer3,tabinit}.c, then checked independently.
Every codebook below satisfies the Kraft equality exactly (sum 2^-len == 1)
and is verified prefix-free, complete, and duplicate-free before this script
will emit anything.  A transcription slip in a length breaks Kraft; a slip in a
code breaks prefix-freeness.  Both abort the generator, which is what makes
these ~1400 entries a build failure rather than a wrong sample in one frame of
one file.  The synthesis window is kept as exact multiples of 2^-16 rather
than the 9-decimal rounding the table is usually printed with, because the
standard's values are exactly that.

Regenerate with:
    python3 tests/unit/mp3_tables_gen.py c/lib/audio/mp3_tables.h
The output is committed; a normal build never runs this script.
"""

import math
import sys

HUFF = {
    1: (2, [
        1, 3, 2, 3],
        [
        1, 1, 1, 0]),
    2: (3, [
        1, 3, 6, 3, 3, 5, 5, 5, 6],
        [
        1, 2, 1, 3, 1, 1, 3, 2, 0]),
    3: (3, [
        2, 2, 6, 3, 2, 5, 5, 5, 6],
        [
        3, 2, 1, 1, 1, 1, 3, 2, 0]),
    5: (4, [
        1, 3, 6, 7, 3, 3, 6, 7, 6, 6, 7, 8, 7, 6, 7, 8],
        [
        1, 2, 6, 5, 3, 1, 4, 4, 7, 5, 7, 1, 6, 1, 1, 0]),
    6: (4, [
        3, 3, 5, 7, 3, 2, 4, 5, 4, 4, 5, 6, 6, 5, 6, 7],
        [
        7, 3, 5, 1, 6, 2, 3, 2, 5, 4, 4, 1, 3, 3, 2, 0]),
    7: (6, [
        1, 3, 6, 8, 8, 9, 3, 4, 6, 7, 7, 8, 6, 5, 7, 8, 8, 9, 7, 7, 8, 9, 9, 9,
        7, 7, 8, 9, 9, 10, 8, 8, 9, 10, 10, 10],
        [
        1, 2, 10, 19, 16, 10, 3, 3, 7, 10, 5, 3, 11, 4, 13, 17, 8, 4, 12, 11, 18, 15, 11, 2,
        7, 6, 9, 14, 3, 1, 6, 4, 5, 3, 2, 0]),
    8: (6, [
        2, 3, 6, 8, 8, 9, 3, 2, 4, 8, 8, 8, 6, 4, 6, 8, 8, 9, 8, 8, 8, 9, 9, 10,
        8, 7, 8, 9, 10, 10, 9, 8, 9, 9, 11, 11],
        [
        3, 4, 6, 18, 12, 5, 5, 1, 2, 16, 9, 3, 7, 3, 5, 14, 7, 3, 19, 17, 15, 13, 10, 4,
        13, 5, 8, 11, 5, 1, 12, 4, 4, 1, 1, 0]),
    9: (6, [
        3, 3, 5, 6, 8, 9, 3, 3, 4, 5, 6, 8, 4, 4, 5, 6, 7, 8, 6, 5, 6, 7, 7, 8,
        7, 6, 7, 7, 8, 9, 8, 7, 8, 8, 9, 9],
        [
        7, 5, 9, 14, 15, 7, 6, 4, 5, 5, 6, 7, 7, 6, 8, 8, 8, 5, 15, 6, 9, 10, 5, 1,
        11, 7, 9, 6, 4, 1, 14, 4, 6, 2, 6, 0]),
    10: (8, [
        1, 3, 6, 8, 9, 9, 9, 10, 3, 4, 6, 7, 8, 9, 8, 8, 6, 6, 7, 8, 9, 10, 9, 9,
        7, 7, 8, 9, 10, 10, 9, 10, 8, 8, 9, 10, 10, 10, 10, 10, 9, 9, 10, 10, 11, 11, 10, 11,
        8, 8, 9, 10, 10, 10, 11, 11, 9, 8, 9, 10, 10, 11, 11, 11],
        [
        1, 2, 10, 23, 35, 30, 12, 17, 3, 3, 8, 12, 18, 21, 12, 7, 11, 9, 15, 21, 32, 40, 19, 6,
        14, 13, 22, 34, 46, 23, 18, 7, 20, 19, 33, 47, 27, 22, 9, 3, 31, 22, 41, 26, 21, 20, 5, 3,
        14, 13, 10, 11, 16, 6, 5, 1, 9, 8, 7, 8, 4, 4, 2, 0]),
    11: (8, [
        2, 3, 5, 7, 8, 9, 8, 9, 3, 3, 4, 6, 8, 8, 7, 8, 5, 5, 6, 7, 8, 9, 8, 8,
        7, 6, 7, 9, 8, 10, 8, 9, 8, 8, 8, 9, 9, 10, 9, 10, 8, 8, 9, 10, 10, 11, 10, 11,
        8, 7, 7, 8, 9, 10, 10, 10, 8, 7, 8, 9, 10, 10, 10, 10],
        [
        3, 4, 10, 24, 34, 33, 21, 15, 5, 3, 4, 10, 32, 17, 11, 10, 11, 7, 13, 18, 30, 31, 20, 5,
        25, 11, 19, 59, 27, 18, 12, 5, 35, 33, 31, 58, 30, 16, 7, 5, 28, 26, 32, 19, 17, 15, 8, 14,
        14, 12, 9, 13, 14, 9, 4, 1, 11, 4, 6, 6, 6, 3, 2, 0]),
    12: (8, [
        4, 3, 5, 7, 8, 9, 9, 9, 3, 3, 4, 5, 7, 7, 8, 8, 5, 4, 5, 6, 7, 8, 7, 8,
        6, 5, 6, 6, 7, 8, 8, 8, 7, 6, 7, 7, 8, 8, 8, 9, 8, 7, 8, 8, 8, 9, 8, 9,
        8, 7, 7, 8, 8, 9, 9, 10, 9, 8, 8, 9, 9, 9, 9, 10],
        [
        9, 6, 16, 33, 41, 39, 38, 26, 7, 5, 6, 9, 23, 16, 26, 11, 17, 7, 11, 14, 21, 30, 10, 7,
        17, 10, 15, 12, 18, 28, 14, 5, 32, 13, 22, 19, 18, 16, 9, 5, 40, 17, 31, 29, 17, 13, 4, 2,
        27, 12, 11, 15, 10, 7, 4, 1, 27, 12, 8, 12, 6, 3, 1, 0]),
    13: (16, [
        1, 4, 6, 7, 8, 9, 9, 10, 9, 10, 11, 11, 12, 12, 13, 13, 3, 4, 6, 7, 8, 8, 9, 9,
        9, 9, 10, 10, 11, 12, 12, 12, 6, 6, 7, 8, 9, 9, 10, 10, 9, 10, 10, 11, 11, 12, 13, 13,
        7, 7, 8, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 13, 13, 8, 7, 9, 9, 10, 10, 11, 11,
        10, 11, 11, 12, 12, 13, 13, 14, 9, 8, 9, 10, 10, 10, 11, 11, 11, 11, 12, 11, 13, 13, 14, 14,
        9, 9, 10, 10, 11, 11, 11, 11, 11, 12, 12, 12, 13, 13, 14, 14, 10, 9, 10, 11, 11, 11, 12, 12,
        12, 12, 13, 13, 13, 14, 16, 16, 9, 8, 9, 10, 10, 11, 11, 12, 12, 12, 12, 13, 13, 14, 15, 15,
        10, 9, 10, 10, 11, 11, 11, 13, 12, 13, 13, 14, 14, 14, 16, 15, 10, 10, 10, 11, 11, 12, 12, 13,
        12, 13, 14, 13, 14, 15, 16, 17, 11, 10, 10, 11, 12, 12, 12, 12, 13, 13, 13, 14, 15, 15, 15, 16,
        11, 11, 11, 12, 12, 13, 12, 13, 14, 14, 15, 15, 15, 16, 16, 16, 12, 11, 12, 13, 13, 13, 14, 14,
        14, 14, 14, 15, 16, 15, 16, 16, 13, 12, 12, 13, 13, 13, 15, 14, 14, 17, 15, 15, 15, 17, 16, 16,
        12, 12, 13, 14, 14, 14, 15, 14, 15, 15, 16, 16, 19, 18, 19, 16],
        [
        1, 5, 14, 21, 34, 51, 46, 71, 42, 52, 68, 52, 67, 44, 43, 19, 3, 4, 12, 19, 31, 26, 44, 33,
        31, 24, 32, 24, 31, 35, 22, 14, 15, 13, 23, 36, 59, 49, 77, 65, 29, 40, 30, 40, 27, 33, 42, 16,
        22, 20, 37, 61, 56, 79, 73, 64, 43, 76, 56, 37, 26, 31, 25, 14, 35, 16, 60, 57, 97, 75, 114, 91,
        54, 73, 55, 41, 48, 53, 23, 24, 58, 27, 50, 96, 76, 70, 93, 84, 77, 58, 79, 29, 74, 49, 41, 17,
        47, 45, 78, 74, 115, 94, 90, 79, 69, 83, 71, 50, 59, 38, 36, 15, 72, 34, 56, 95, 92, 85, 91, 90,
        86, 73, 77, 65, 51, 44, 43, 42, 43, 20, 30, 44, 55, 78, 72, 87, 78, 61, 46, 54, 37, 30, 20, 16,
        53, 25, 41, 37, 44, 59, 54, 81, 66, 76, 57, 54, 37, 18, 39, 11, 35, 33, 31, 57, 42, 82, 72, 80,
        47, 58, 55, 21, 22, 26, 38, 22, 53, 25, 23, 38, 70, 60, 51, 36, 55, 26, 34, 23, 27, 14, 9, 7,
        34, 32, 28, 39, 49, 75, 30, 52, 48, 40, 52, 28, 18, 17, 9, 5, 45, 21, 34, 64, 56, 50, 49, 45,
        31, 19, 12, 15, 10, 7, 6, 3, 48, 23, 20, 39, 36, 35, 53, 21, 16, 23, 13, 10, 6, 1, 4, 2,
        16, 15, 17, 27, 25, 20, 29, 11, 17, 12, 16, 8, 1, 1, 0, 1]),
    15: (16, [
        3, 4, 5, 7, 7, 8, 9, 9, 9, 10, 10, 11, 11, 11, 12, 13, 4, 3, 5, 6, 7, 7, 8, 8,
        8, 9, 9, 10, 10, 10, 11, 11, 5, 5, 5, 6, 7, 7, 8, 8, 8, 9, 9, 10, 10, 11, 11, 11,
        6, 6, 6, 7, 7, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 11, 7, 6, 7, 7, 8, 8, 9, 9,
        9, 9, 10, 10, 10, 11, 11, 11, 8, 7, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 11, 11, 11, 12,
        9, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 11, 11, 12, 12, 9, 8, 8, 9, 9, 9, 9, 10,
        10, 10, 10, 10, 11, 11, 11, 12, 9, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 12, 12, 12,
        9, 8, 9, 9, 9, 9, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 10, 9, 9, 9, 10, 10, 10, 10,
        10, 11, 11, 11, 11, 12, 13, 12, 10, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 13,
        11, 10, 9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 13, 13, 11, 10, 10, 10, 10, 11, 11, 11,
        11, 12, 12, 12, 12, 12, 13, 13, 12, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 12, 13,
        12, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13, 13, 13],
        [
        7, 12, 18, 53, 47, 76, 124, 108, 89, 123, 108, 119, 107, 81, 122, 63, 13, 5, 16, 27, 46, 36, 61, 51,
        42, 70, 52, 83, 65, 41, 59, 36, 19, 17, 15, 24, 41, 34, 59, 48, 40, 64, 50, 78, 62, 80, 56, 33,
        29, 28, 25, 43, 39, 63, 55, 93, 76, 59, 93, 72, 54, 75, 50, 29, 52, 22, 42, 40, 67, 57, 95, 79,
        72, 57, 89, 69, 49, 66, 46, 27, 77, 37, 35, 66, 58, 52, 91, 74, 62, 48, 79, 63, 90, 62, 40, 38,
        125, 32, 60, 56, 50, 92, 78, 65, 55, 87, 71, 51, 73, 51, 70, 30, 109, 53, 49, 94, 88, 75, 66, 122,
        91, 73, 56, 42, 64, 44, 21, 25, 90, 43, 41, 77, 73, 63, 56, 92, 77, 66, 47, 67, 48, 53, 36, 20,
        71, 34, 67, 60, 58, 49, 88, 76, 67, 106, 71, 54, 38, 39, 23, 15, 109, 53, 51, 47, 90, 82, 58, 57,
        48, 72, 57, 41, 23, 27, 62, 9, 86, 42, 40, 37, 70, 64, 52, 43, 70, 55, 42, 25, 29, 18, 11, 11,
        118, 68, 30, 55, 50, 46, 74, 65, 49, 39, 24, 16, 22, 13, 14, 7, 91, 44, 39, 38, 34, 63, 52, 45,
        31, 52, 28, 19, 14, 8, 9, 3, 123, 60, 58, 53, 47, 43, 32, 22, 37, 24, 17, 12, 15, 10, 2, 1,
        71, 37, 34, 30, 28, 20, 17, 26, 21, 16, 10, 6, 8, 6, 2, 0]),
    16: (16, [
        1, 4, 6, 8, 9, 9, 10, 10, 11, 11, 11, 12, 12, 12, 13, 9, 3, 4, 6, 7, 8, 9, 9, 9,
        10, 10, 10, 11, 12, 11, 12, 8, 6, 6, 7, 8, 9, 9, 10, 10, 11, 10, 11, 11, 11, 12, 12, 9,
        8, 7, 8, 9, 9, 10, 10, 10, 11, 11, 12, 12, 12, 13, 13, 10, 9, 8, 9, 9, 10, 10, 11, 11,
        11, 12, 12, 12, 13, 13, 13, 9, 9, 8, 9, 9, 10, 11, 11, 12, 11, 12, 12, 13, 13, 13, 14, 10,
        10, 9, 9, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 14, 10, 10, 9, 10, 10, 11, 11, 11, 12,
        12, 13, 13, 13, 13, 15, 15, 10, 10, 10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 13, 14, 14, 14, 10,
        11, 10, 10, 11, 11, 12, 12, 13, 13, 13, 13, 14, 13, 14, 13, 11, 11, 11, 10, 11, 12, 12, 12, 12,
        13, 14, 14, 14, 15, 15, 14, 10, 12, 11, 11, 11, 12, 12, 13, 14, 14, 14, 14, 14, 14, 13, 14, 11,
        12, 12, 12, 12, 12, 13, 13, 13, 13, 15, 14, 14, 14, 14, 16, 11, 14, 12, 12, 12, 13, 13, 14, 14,
        14, 16, 15, 15, 15, 17, 15, 11, 13, 13, 11, 12, 14, 14, 13, 14, 14, 15, 16, 15, 17, 15, 14, 11,
        9, 8, 8, 9, 9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 8],
        [
        1, 5, 14, 44, 74, 63, 110, 93, 172, 149, 138, 242, 225, 195, 376, 17, 3, 4, 12, 20, 35, 62, 53, 47,
        83, 75, 68, 119, 201, 107, 207, 9, 15, 13, 23, 38, 67, 58, 103, 90, 161, 72, 127, 117, 110, 209, 206, 16,
        45, 21, 39, 69, 64, 114, 99, 87, 158, 140, 252, 212, 199, 387, 365, 26, 75, 36, 68, 65, 115, 101, 179, 164,
        155, 264, 246, 226, 395, 382, 362, 9, 66, 30, 59, 56, 102, 185, 173, 265, 142, 253, 232, 400, 388, 378, 445, 16,
        111, 54, 52, 100, 184, 178, 160, 133, 257, 244, 228, 217, 385, 366, 715, 10, 98, 48, 91, 88, 165, 157, 148, 261,
        248, 407, 397, 372, 380, 889, 884, 8, 85, 84, 81, 159, 156, 143, 260, 249, 427, 401, 392, 383, 727, 713, 708, 7,
        154, 76, 73, 141, 131, 256, 245, 426, 406, 394, 384, 735, 359, 710, 352, 11, 139, 129, 67, 125, 247, 233, 229, 219,
        393, 743, 737, 720, 885, 882, 439, 4, 243, 120, 118, 115, 227, 223, 396, 746, 742, 736, 721, 712, 706, 223, 436, 6,
        202, 224, 222, 218, 216, 389, 386, 381, 364, 888, 443, 707, 440, 437, 1728, 4, 747, 211, 210, 208, 370, 379, 734, 723,
        714, 1735, 883, 877, 876, 3459, 865, 2, 377, 369, 102, 187, 726, 722, 358, 711, 709, 866, 1734, 871, 3458, 870, 434, 0,
        12, 10, 7, 11, 10, 17, 11, 9, 13, 12, 10, 7, 5, 3, 1, 3]),
    24: (16, [
        4, 4, 6, 7, 8, 9, 9, 10, 10, 11, 11, 11, 11, 11, 12, 9, 4, 4, 5, 6, 7, 8, 8, 9,
        9, 9, 10, 10, 10, 10, 10, 8, 6, 5, 6, 7, 7, 8, 8, 9, 9, 9, 9, 10, 10, 10, 11, 7,
        7, 6, 7, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 7, 8, 7, 7, 8, 8, 8, 8, 9,
        9, 9, 10, 10, 10, 10, 11, 7, 9, 7, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 7,
        9, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 7, 10, 8, 8, 8, 9, 9, 9, 9,
        10, 10, 10, 10, 10, 11, 11, 8, 10, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 8,
        10, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 11, 11, 8, 11, 9, 9, 9, 9, 10, 10, 10,
        10, 10, 10, 11, 11, 11, 11, 8, 11, 10, 9, 9, 9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 8,
        11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 8, 11, 10, 10, 10, 10, 10, 10, 10,
        11, 11, 11, 11, 11, 11, 11, 8, 12, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 8,
        8, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 4],
        [
        15, 13, 46, 80, 146, 262, 248, 434, 426, 669, 653, 649, 621, 517, 1032, 88, 14, 12, 21, 38, 71, 130, 122, 216,
        209, 198, 327, 345, 319, 297, 279, 42, 47, 22, 41, 74, 68, 128, 120, 221, 207, 194, 182, 340, 315, 295, 541, 18,
        81, 39, 75, 70, 134, 125, 116, 220, 204, 190, 178, 325, 311, 293, 271, 16, 147, 72, 69, 135, 127, 118, 112, 210,
        200, 188, 352, 323, 306, 285, 540, 14, 263, 66, 129, 126, 119, 114, 214, 202, 192, 180, 341, 317, 301, 281, 262, 12,
        249, 123, 121, 117, 113, 215, 206, 195, 185, 347, 330, 308, 291, 272, 520, 10, 435, 115, 111, 109, 211, 203, 196, 187,
        353, 332, 313, 298, 283, 531, 381, 17, 427, 212, 208, 205, 201, 193, 186, 177, 169, 320, 303, 286, 268, 514, 377, 16,
        335, 199, 197, 191, 189, 181, 174, 333, 321, 305, 289, 275, 521, 379, 371, 11, 668, 184, 183, 179, 175, 344, 331, 314,
        304, 290, 277, 530, 383, 373, 366, 10, 652, 346, 171, 168, 164, 318, 309, 299, 287, 276, 263, 513, 375, 368, 362, 6,
        648, 322, 316, 312, 307, 302, 292, 284, 269, 261, 512, 376, 370, 364, 359, 4, 620, 300, 296, 294, 288, 282, 273, 266,
        515, 380, 374, 369, 365, 361, 357, 2, 1033, 280, 278, 274, 267, 264, 259, 382, 378, 372, 367, 363, 360, 358, 356, 0,
        43, 20, 19, 17, 15, 13, 11, 9, 7, 6, 4, 7, 5, 3, 1, 3]),
    32: (0, [
        1, 4, 4, 5, 4, 6, 5, 6, 4, 5, 5, 6, 5, 6, 6, 6],
        [
        1, 5, 4, 5, 6, 5, 4, 4, 7, 3, 6, 0, 7, 2, 3, 1]),
    33: (0, [
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4],
        [
        15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0]),
}

HT = [
    (0, 0), (1, 0), (2, 0), (3, 0), (0, 0), (5, 0), (6, 0), (7, 0),
    (8, 0), (9, 0), (10, 0), (11, 0), (12, 0), (13, 0), (0, 0), (15, 0),
    (16, 1), (16, 2), (16, 3), (16, 4), (16, 6), (16, 8), (16, 10), (16, 13),
    (24, 4), (24, 5), (24, 6), (24, 7), (24, 8), (24, 9), (24, 11), (24, 13)
]

SFB_LONG = [
    [0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576],
    [0, 4, 8, 12, 16, 20, 24, 30, 36, 42, 50, 60, 72, 88, 106, 128, 156, 190, 230, 276, 330, 384, 576],
    [0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 54, 66, 82, 102, 126, 156, 194, 240, 296, 364, 448, 550, 576],
    [0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576],
    [0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 114, 136, 162, 194, 232, 278, 332, 394, 464, 540, 576],
    [0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576],
    [0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576],
    [0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576],
    [0, 12, 24, 36, 48, 60, 72, 88, 108, 132, 160, 192, 232, 280, 336, 400, 476, 566, 568, 570, 572, 574, 576],
]
SFB_SHORT = [
    [0, 12, 24, 36, 48, 66, 90, 120, 156, 198, 252, 318, 408, 576],
    [0, 12, 24, 36, 48, 66, 84, 114, 150, 192, 240, 300, 378, 576],
    [0, 12, 24, 36, 48, 66, 90, 126, 174, 234, 312, 414, 540, 576],
    [0, 12, 24, 36, 54, 72, 96, 126, 168, 222, 300, 396, 522, 576],
    [0, 12, 24, 36, 54, 78, 108, 144, 186, 240, 312, 408, 540, 576],
    [0, 12, 24, 36, 54, 78, 108, 144, 186, 240, 312, 402, 522, 576],
    [0, 12, 24, 36, 54, 78, 108, 144, 186, 240, 312, 402, 522, 576],
    [0, 12, 24, 36, 54, 78, 108, 144, 186, 240, 312, 402, 522, 576],
    [0, 24, 48, 72, 108, 156, 216, 288, 372, 480, 486, 492, 498, 576],
]

DEWIN_65536 = [
    0, -1, -1, -1, -1, -1, -1, -2, -2, -2, -2, -3,
    -3, -4, -4, -5, -5, -6, -7, -7, -8, -9, -10, -11,
    -13, -14, -16, -17, -19, -21, -24, -26, -29, -31, -35, -38,
    -41, -45, -49, -53, -58, -63, -68, -73, -79, -85, -91, -97,
    -104, -111, -117, -125, -132, -139, -147, -154, -161, -169, -176, -183,
    -190, -196, -202, -208, -213, -218, -222, -225, -227, -228, -228, -227,
    -224, -221, -215, -208, -200, -189, -177, -163, -146, -127, -106, -83,
    -57, -29, 2, 36, 72, 111, 153, 197, 244, 294, 347, 401,
    459, 519, 581, 645, 711, 779, 848, 919, 991, 1064, 1137, 1210,
    1283, 1356, 1428, 1498, 1567, 1634, 1698, 1759, 1817, 1870, 1919, 1962,
    2001, 2032, 2057, 2075, 2085, 2087, 2080, 2063, 2037, 2000, 1952, 1893,
    1822, 1739, 1644, 1535, 1414, 1280, 1131, 970, 794, 605, 402, 185,
    -45, -288, -545, -814, -1095, -1388, -1692, -2006, -2330, -2663, -3004, -3351,
    -3705, -4063, -4425, -4788, -5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597,
    -7910, -8209, -8491, -8755, -8998, -9219, -9416, -9585, -9727, -9838, -9916, -9959,
    -9966, -9935, -9863, -9750, -9592, -9389, -9139, -8840, -8492, -8092, -7640, -7134,
    -6574, -5959, -5288, -4561, -3776, -2935, -2037, -1082, -70, 998, 2122, 3300,
    4533, 5818, 7154, 8540, 9975, 11455, 12980, 14548, 16155, 17799, 19478, 21189,
    22929, 24694, 26482, 28289, 30112, 31947, 33791, 35640, 37489, 39336, 41176, 43006,
    44821, 46617, 48390, 50137, 51853, 53534, 55178, 56778, 58333, 59838, 61289, 62684,
    64019, 65290, 66494, 67629, 68692, 69679, 70590, 71420, 72169, 72835, 73415, 73908,
    74313, 74630, 74856, 74992, 75038
]


# ---------------------------------------------------------------------------
# validation -- a transcription error in the tables above must stop the build
# ---------------------------------------------------------------------------

def validate():
    for n in sorted(HUFF):
        dim, lens, codes = HUFF[n]
        assert len(lens) == len(codes)
        if dim:
            assert len(lens) == dim * dim, (n, dim, len(lens))
        else:
            assert len(lens) == 16, n
        kraft = sum(2.0 ** -l for l in lens)
        assert abs(kraft - 1.0) < 1e-12, (
            'table %d is not a complete prefix code: kraft=%.12f' % (n, kraft))
        seen = set()
        for i, (l, c) in enumerate(zip(lens, codes)):
            assert 0 < l <= 19, (n, i, l)
            assert c < (1 << l), (
                'table %d entry %d: code %d does not fit in %d bits' % (n, i, c, l))
            for pl in range(1, l):
                assert (c >> (l - pl), pl) not in seen, (
                    'table %d entry %d has a proper prefix already in the code' % (n, i))
            assert (c, l) not in seen, (
                'table %d entry %d duplicates a codeword' % (n, i))
            seen.add((c, l))
    for row in SFB_LONG:
        assert len(row) == 23 and row[0] == 0 and row[-1] == 576
        assert all(row[i] <= row[i + 1] for i in range(len(row) - 1)), row
    for row in SFB_SHORT:
        assert len(row) == 14 and row[0] == 0 and row[-1] == 576
        assert all(row[i] <= row[i + 1] for i in range(len(row) - 1)), row
    assert len(DEWIN_65536) == 257
    for book, lin in HT:
        assert book == 0 or book in HUFF, book
        assert 0 <= lin <= 13


# ---------------------------------------------------------------------------
# computed coefficients
# ---------------------------------------------------------------------------

# Alias reduction: ISO 11172-3 2.4.3.4.10.1 gives c_i; cs/ca are derived.
CI = [-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037]


def alias():
    cs, ca = [], []
    for c in CI:
        sq = math.sqrt(1.0 + c * c)
        cs.append(1.0 / sq)
        ca.append(c / sq)
    return cs, ca


def windows():
    """The four block windows, ISO 11172-3 2.4.3.4.10.3.  Type 2 (short) uses
    only 12 of its 36 slots; the rest stay zero so the C side can index all
    four uniformly."""
    w = [[0.0] * 36 for _ in range(4)]
    for i in range(36):
        w[0][i] = math.sin(math.pi / 36.0 * (i + 0.5))
    for i in range(18):
        w[1][i] = math.sin(math.pi / 36.0 * (i + 0.5))
    for i in range(18, 24):
        w[1][i] = 1.0
    for i in range(24, 30):
        w[1][i] = math.sin(math.pi / 12.0 * (i - 18 + 0.5))
    # w[1][30..35] stay 0
    for i in range(12):
        w[2][i] = math.sin(math.pi / 12.0 * (i + 0.5))
    # w[3][0..5] stay 0
    for i in range(6, 12):
        w[3][i] = math.sin(math.pi / 12.0 * (i - 6 + 0.5))
    for i in range(12, 18):
        w[3][i] = 1.0
    for i in range(18, 36):
        w[3][i] = math.sin(math.pi / 36.0 * (i + 0.5))
    return w


def costab(den, n):
    """cos(k*pi/den) for k in [0, n).

    The three cosine kernels this decoder needs -- the 36- and 12-point IMDCTs
    and the 64-point synthesis matrixing -- all have arguments that are integer
    multiples of pi/72, pi/24 and pi/64 respectively, so one periodic table per
    kernel represents every (i,k) product exactly rather than approximately,
    and costs 320 doubles instead of 2768."""
    return [math.cos(k * math.pi / den) for k in range(n)]


def intensity_mpeg1():
    t1, t2 = [], []
    for i in range(16):
        t = math.tan(i * math.pi / 12.0)
        t1.append(t / (1.0 + t))
        t2.append(1.0 / (1.0 + t))
    return t1, t2


def intensity_lsf():
    """MPEG-2 / 2.5 intensity stereo, ISO 13818-3 2.4.3.2: the ratio is a power
    of 2^(-1/4) or 2^(-1/2) chosen by intensity_scale, applied to whichever
    channel the parity of is_pos selects."""
    p1 = [[0.0] * 16 for _ in range(2)]
    p2 = [[0.0] * 16 for _ in range(2)]
    for j in range(2):
        base = math.pow(2.0, -0.25 * (j + 1.0))
        for i in range(16):
            a, b = 1.0, 1.0
            if i > 0:
                if i & 1:
                    a = math.pow(base, (i + 1.0) * 0.5)
                else:
                    b = math.pow(base, i * 0.5)
            p1[j][i] = a
            p2[j][i] = b
    return p1, p2


# ---------------------------------------------------------------------------
# emission
# ---------------------------------------------------------------------------

def d(x):
    """A double literal that round-trips to the same bits."""
    s = repr(float(x))
    return s if ('.' in s or 'e' in s or 'n' in s) else s + '.0'


def rows(vals, per, conv=str, indent='    '):
    out = []
    for i in range(0, len(vals), per):
        out.append(indent + ', '.join(conv(v) for v in vals[i:i + per]))
    return ',\n'.join(out)


def emit(path):
    validate()
    o = []
    A = o.append
    A('/* c/lib/audio/mp3_tables.h -- GENERATED by tests/unit/mp3_tables_gen.py.')
    A(' * Do not edit by hand; edit the generator and re-run it.')
    A(' *')
    A(' * Bitstream tables (Huffman codebooks, scalefactor bands, the synthesis')
    A(' * window) are the published tables of ISO/IEC 11172-3. Everything else is')
    A(' * computed from the formulas in the standard, at generation time rather than')
    A(' * at run time: the target links mini-libc, which has no double-precision')
    A(' * libm, and a table built by two different libm implementations would not be')
    A(' * bit-identical between the host test and the guest.')
    A(' *')
    A(' * The generator refuses to emit a codebook that is not an exactly complete')
    A(' * prefix code (Kraft sum 1.0), which is what makes a transcription slip in')
    A(' * these ~1400 entries a build failure rather than a wrong sample.')
    A(' */')
    A('#ifndef LOGIT_MP3_TABLES_H')
    A('#define LOGIT_MP3_TABLES_H')
    A('')
    A('#include <stdint.h>')
    A('')
    A('/* --- Huffman codebooks, ISO 11172-3 Table 3-B.7 -------------------------')
    A(' * Lengths exclude the sign bits and the linbits escape; the decoder reads')
    A(' * both after the codeword. Tables 17-23 share codebook 16 and tables 25-31')
    A(' * share codebook 24, differing only in linbits, which is why 15 big-value')
    A(' * codebooks appear here rather than 30. */')
    for n in sorted(HUFF):
        dim, lens, codes = HUFF[n]
        A('static const uint8_t mp3_hlen_%d[%d] = {' % (n, len(lens)))
        A(rows(lens, 24))
        A('};')
        A('static const uint16_t mp3_hcod_%d[%d] = {' % (n, len(codes)))
        A(rows(codes, 16))
        A('};')
    A('')
    A('typedef struct {')
    A('    uint16_t n;        /* entries */')
    A('    uint8_t  dim;      /* x and y each range over [0, dim); 0 for count1 */')
    A('    const uint8_t  *len;')
    A('    const uint16_t *code;')
    A('} mp3_hufftab;')
    A('')
    A('/* Indexed by ISO table number. Slots 0, 4 and 14 carry no codebook (0 is')
    A(' * "all values are zero" and 4 and 14 are unused by the standard). */')
    A('static const mp3_hufftab mp3_huff[34] = {')
    for i in range(34):
        if i in HUFF:
            dim, lens, codes = HUFF[i]
            A('    { %d, %d, mp3_hlen_%d, mp3_hcod_%d },' % (len(lens), dim, i, i))
        else:
            A('    { 0, 0, 0, 0 },')
    A('};')
    A('')
    A('/* table_select -> (codebook, linbits). */')
    A('static const uint8_t mp3_ht_book[32] = {')
    A(rows([h[0] for h in HT], 16))
    A('};')
    A('static const uint8_t mp3_ht_linbits[32] = {')
    A(rows([h[1] for h in HT], 16))
    A('};')
    A('')
    A('/* --- scalefactor band boundaries, ISO 11172-3 Table 3-B.8 ---------------')
    A(' * Rows 0-2 are MPEG-1 (44100, 48000, 32000), 3-5 MPEG-2 (22050, 24000,')
    A(' * 16000), 6-8 MPEG-2.5 (11025, 12000, 8000). Short-block values are')
    A(' * absolute line indices, i.e. already multiplied by the three windows. */')
    A('static const int16_t mp3_sfb_long[9][23] = {')
    for r in SFB_LONG:
        A('    { ' + ', '.join(str(x) for x in r) + ' },')
    A('};')
    A('static const int16_t mp3_sfb_short[9][14] = {')
    for r in SFB_SHORT:
        A('    { ' + ', '.join(str(x) for x in r) + ' },')
    A('};')
    A('')
    A('/* Pre-emphasis added to long-block scalefactors when preflag is set. */')
    A('static const uint8_t mp3_pretab[22] = {')
    A('    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0')
    A('};')
    A('')
    A('/* MPEG-1 scalefac_compress -> (slen1, slen2), Table 3-B.4. */')
    A('static const uint8_t mp3_slen1[16] = { 0,0,0,0,3,1,1,1,2,2,2,3,3,3,4,4 };')
    A('static const uint8_t mp3_slen2[16] = { 0,1,2,3,0,1,2,3,1,2,3,1,2,3,2,3 };')
    A('')
    A('/* MPEG-2/2.5 scalefactor band counts per (block class, slen class): the')
    A(' * "nr of sfb" table of ISO 13818-3 2.4.3.2. */')
    A('static const uint8_t mp3_lsf_nr[3][6][4] = {')
    A('    { {  6,  5,  5, 5 }, {  6,  5,  7, 3 }, { 11, 10, 0, 0 },')
    A('      {  7,  7,  7, 0 }, {  6,  6,  6, 3 }, {  8,  8, 5, 0 } },')
    A('    { {  9,  9,  9, 9 }, {  9,  9, 12, 6 }, { 18, 18, 0, 0 },')
    A('      { 12, 12, 12, 0 }, { 12,  9,  9, 6 }, { 15, 12, 9, 0 } },')
    A('    { {  6,  9,  9, 9 }, {  6,  9, 12, 6 }, { 15, 18, 0, 0 },')
    A('      {  6, 15, 12, 0 }, {  6, 12,  9, 6 }, {  6, 18, 9, 0 } }')
    A('};')
    A('')
    cs, ca = alias()
    A('/* --- alias reduction butterflies, 2.4.3.4.10.1 -------------------------- */')
    A('static const double mp3_aa_cs[8] = {')
    A(rows(cs, 4, d))
    A('};')
    A('static const double mp3_aa_ca[8] = {')
    A(rows(ca, 4, d))
    A('};')
    A('')
    w = windows()
    A('/* --- block windows, 2.4.3.4.10.3 (type 2 uses only [0,12)) -------------- */')
    A('static const double mp3_win[4][36] = {')
    for i in range(4):
        A('    {')
        A(rows(w[i], 4, d, '        '))
        A('    },')
    A('};')
    A('')
    A('/* --- cosine kernels ----------------------------------------------------- */')
    A('/* mp3_cos72[m] = cos(m*pi/72). The 36-point IMDCT needs')
    A(' * cos(pi/72*(2i+1+18)(2k+1)), an argument that is always m*pi/72 with m')
    A(' * taken mod 144, so this table represents it exactly. */')
    A('static const double mp3_cos72[144] = {')
    A(rows(costab(72.0, 144), 4, d))
    A('};')
    A('/* mp3_cos24[m] = cos(m*pi/24), for the three 12-point IMDCTs. */')
    A('static const double mp3_cos24[48] = {')
    A(rows(costab(24.0, 48), 4, d))
    A('};')
    A('/* mp3_cos64[m] = cos(m*pi/64), for the 64-point synthesis matrixing. */')
    A('static const double mp3_cos64[128] = {')
    A(rows(costab(64.0, 128), 4, d))
    A('};')
    A('')
    A('/* --- synthesis window D, ISO 11172-3 Table 3-B.3 ------------------------')
    A(" * The standard's coefficients are exact multiples of 2^-16, so the")
    A(' * prototype is kept as those integers rather than as the 9-decimal')
    A(' * roundings the table is usually printed with, which would otherwise put a')
    A(' * rounding error into every output sample. The prototype C is symmetric')
    A(' * about tap 256, so 257 values give all 512.')
    A(' *')
    A(' * The sign pattern is derived, not transcribed. In the ISO decoder flow the')
    A(' * window is applied to U, and U gathers V so that output j takes its tap')
    A(' * for block age i from V position 32*(i&1)+j while the natural tap index is')
    A(' * n = 32i+j. The matrixing cosine at those two positions differs by')
    A(' * cos(A + (2k+1)*m*pi) with m = floor(n/64), i.e. by (-1)^floor(n/64) for')
    A(' * every subband k at once. D therefore carries exactly that alternation on')
    A(' * top of the smooth prototype: D[n] = (-1)^floor(n/64) * C[n]. Getting this')
    A(' * wrong is not subtle -- it puts a discontinuity at every 32-sample block')
    A(' * boundary, which is what it did before it was derived. */')
    A('static const double mp3_dwin[512] = {')
    dwin = []
    for n in range(512):
        c = DEWIN_65536[n if n <= 256 else 512 - n] / 65536.0
        dwin.append(-c if (n // 64) & 1 else c)
    A(rows(dwin, 4, d))
    A('};')
    A('#define MP3_DWIN(i) (mp3_dwin[i])')
    A('')
    t1, t2 = intensity_mpeg1()
    A('/* --- intensity stereo --------------------------------------------------- */')
    A('/* MPEG-1: is_ratio = tan(is_pos * pi/12), split into the two channel gains. */')
    A('static const double mp3_is_t1[16] = {')
    A(rows(t1, 4, d))
    A('};')
    A('static const double mp3_is_t2[16] = {')
    A(rows(t2, 4, d))
    A('};')
    p1, p2 = intensity_lsf()
    A('/* MPEG-2/2.5, indexed [intensity_scale][is_pos]. */')
    A('static const double mp3_lsf_is1[2][16] = {')
    for j in range(2):
        A('    {')
        A(rows(p1[j], 4, d, '        '))
        A('    },')
    A('};')
    A('static const double mp3_lsf_is2[2][16] = {')
    for j in range(2):
        A('    {')
        A(rows(p2[j], 4, d, '        '))
        A('    },')
    A('};')
    A('')
    A('#endif /* LOGIT_MP3_TABLES_H */')
    text = '\n'.join(o) + '\n'
    with open(path, 'w') as f:
        f.write(text)
    sys.stderr.write('mp3_tables_gen: wrote %s (%d bytes); %d codebooks validated\n'
                     % (path, len(text), len(HUFF)))


if __name__ == '__main__':
    emit(sys.argv[1] if len(sys.argv) > 1 else 'c/lib/audio/mp3_tables.h')
