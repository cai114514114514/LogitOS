/*
 * Pinyin Dictionary Binary Format - c/lib/ime/pinyin_fmt.h
 *
 * The on-disk form of fsroot/ime/pinyin.dat, written by tools/mkpinyin.py and
 * read (in place, never copied) by ime_open() in pinyin.c.
 *
 * THIS FILE IS THE FORMAT'S SINGLE DEFINITION SITE. tools/mkpinyin.py mirrors
 * it in Python and says so at the top of its own layout table -- one jar, two
 * doors, and the doors are named at both ends on purpose (CLAUDE.md rule 3).
 *
 * ---------------------------------------------------------------------------
 * WHY VERSION 2 EXISTS: v1 STORED AN ORDER, AND AN ORDER CANNOT BE MERGED.
 * ---------------------------------------------------------------------------
 * v1's whole ranking contract was one sentence -- "candidates within each key
 * are sorted by frequency (most common first)" -- and it stored no number. That
 * is sufficient for exactly one query: hand back one key's list in file order.
 * It is not sufficient for any query that MERGES entries from more than one
 * key, and all three of the things the engine now does are merges:
 *
 *   - incremental prefix   ("ni" must rank candidates drawn from 168 keys)
 *   - abbreviation         ("nh" -> 你好, a bucket drawn from 32 different keys)
 *   - a user weight        (a learned count has to be ADDED to something)
 *
 * Measured on the shipped dictionary: the bucket "bj" holds 108 entries whose
 * file order begins 百级 白家 百家 拜见 and whose frequency order begins 北京
 * 编辑 比较 不仅. Merging orders reproduces the first list. Only a number
 * reproduces the second.
 *
 * So v2 stores jieba's RAW u32 frequency per candidate. Not a quantised score:
 * the tightest live margin in this dictionary is 13 counts wide -- within key
 * "xian", 西安 (2576) sits directly above 鲜 (2563), and that pair straddles the
 * page-0/page-1 boundary that c/lib/ime/pinyin.h:96 and tests/unit/ime_test.c
 * both pin. Any quantisation coarse enough to save bytes is coarse enough to
 * swap them, and it would do so silently. Raw counts cost 4 bytes per candidate
 * (141,496 total) and remove the whole question.
 *
 * ---------------------------------------------------------------------------
 * FORMAT (little-endian throughout; every read in pinyin.c is byte-wise, so
 * nothing here is alignment-sensitive)
 * ---------------------------------------------------------------------------
 *
 *   Header (48 bytes, at offset 0):
 *      0  magic[4]     "PYN\0"
 *      4  version      u32 = 2
 *      8  key_count    u32   keys in the key section
 *     12  cand_count   u32   TOTAL candidate records, all keys summed
 *     16  ini_count    u32   rows in the initials table
 *     20  ini_off      u32   byte offset of the initials table
 *     24  ini_stride   u32   bytes per initials row
 *     28  ini_maxlen   u32   longest initials string, excluding its NUL
 *     32  ref_count    u32   u32 entries in the ref array
 *     36  ref_off      u32   byte offset of the ref array
 *     40  build_id     u32   FNV-1a over [0, ini_off) -- see below
 *     44  reserved     u32 = 0
 *
 *   Key section (starts at 48; keys ascending by BYTES, as in v1):
 *     key_str    NUL-terminated ASCII pinyin, toneless, no syllable separators
 *     ncand      u16
 *     per candidate, in DESCENDING freq order:
 *       nbytes   u16   UTF-8 byte count            (unchanged from v1)
 *       freq     u32   jieba corpus frequency      (NEW in v2)
 *       data     nbytes of UTF-8
 *
 *   Initials table (ini_count rows of ini_stride bytes, at ini_off,
 *   strictly ascending under a fixed-width ini_stride-6 byte comparison):
 *      0   ini[ini_stride-6]  NUL-PADDED ASCII, >= 2 letters
 *     +S   ref_first  u32   INDEX (not a byte offset) into the ref array
 *     +S+4 ref_n      u16   refs in this bucket
 *
 *   Ref array (ref_count u32 values, at ref_off):
 *     u32  byte offset from file start of a candidate record (its nbytes
 *          field). Buckets are CONTIGUOUS and tile the array in table order;
 *          each bucket is pre-sorted by descending freq at BUILD time.
 *
 * ---------------------------------------------------------------------------
 * FOUR THINGS THAT LOOK LIKE DETAIL AND ARE NOT
 * ---------------------------------------------------------------------------
 *
 * 1. THE INITIALS TABLE IS FIXED-STRIDE SO IT NEEDS NO LOAD-TIME INDEX. The
 *    key section is variable-length and NUL-delimited, which is why ime_open()
 *    has to build key_off[] -- a 131,072-byte .bss array with its own refusal
 *    bound (IME_MAX_KEYS). A second variable-length table would have needed a
 *    second such array and a second bound. A fixed stride is binary-searched
 *    IN PLACE: zero .bss, zero load-time work beyond validation.
 *
 *    NUL padding is what makes that legal. NUL sorts below every letter, so a
 *    fixed-width memcmp over ini_stride-6 bytes orders the rows identically to
 *    plain lexicographic comparison of the strings -- verified over all 5,850
 *    rows of the shipped table, not assumed.
 *
 * 2. SINGLE-CODEPOINT CANDIDATES ARE DELIBERATELY NOT INDEXED. A one-character
 *    word has a one-LETTER initials string, and a single letter is already the
 *    start of a pinyin syllable, so the engine's prefix class answers it. With
 *    them in, the table gains 17.5 KB, the largest buckets become y=357 z=351
 *    j=341 -- all single characters, all far past IME_PAGE_SIZE -- and every
 *    first keystroke turns into an abbreviation avalanche. With them out the
 *    largest bucket is zz=248 and no bucket is a single letter, so "no
 *    abbreviation below two letters" is a property of the FILE rather than a
 *    rule the engine has to remember.
 *
 * 3. REFS ARE BYTE OFFSETS, WHICH MAKES THEM A BUILD-SPECIFIC IDENTITY.
 *    Excellent as a primary key inside one file; silent corruption across two.
 *    A user-weight store keyed on ref offsets would, after any regeneration,
 *    attach the user's learned preferences to different words with no symptom
 *    at all. That store must key on candidate TEXT. build_id is here so it can
 *    also record WHICH dictionary it learned against and refuse loudly on a
 *    mismatch -- ime_open() itself never checks build_id, because it has
 *    nothing to compare it against.
 *
 * 4. THE ü SPELLING IS INCONSISTENT IN THIS DICTIONARY AND v2 DOES NOT FIX IT.
 *    Single characters are v-spelled (key "nv" -> 女, "lv" -> 绿) because they
 *    come from lazy_pinyin; phrases came from pypinyin's phrase dictionary and
 *    are u-spelled (女孩 is under "nuhai", 虐待 under "nuedai"), except 绿色
 *    which is under "lvse". 157 keys contain a literal 'v'. Normalising it
 *    MOVES KEYS, and moving keys invalidates every measured constant in
 *    pinyin.h and tests/ime.mk at the same time as a format bump. Named here,
 *    fixed in its own change, with its own gate.
 *
 * SOURCE: tools/mkpinyin.py, from
 *   - pypinyin 0.55.0 (MIT) -- character readings and the phrase dictionary
 *   - jieba 0.42.1 (MIT)    -- the word list AND the frequencies v2 now keeps
 *   - fsroot/fonts/ui.ttf's cmap -- the 6,763 CJK characters a candidate may use
 */

#ifndef C_LIB_IME_PINYIN_FMT_H_
#define C_LIB_IME_PINYIN_FMT_H_

#include <stdint.h>

#define PINYIN_MAGIC 0x4e5950  /* "PYN" in little-endian (0x50594e00 with the NUL) */
#define PINYIN_VERSION 2

/* Header, key-record and initials-row geometry. pinyin.c reads every field
 * byte-wise from these offsets rather than casting a struct over the mapping,
 * so these are the definition and there is no packed-struct ABI to get wrong. */
#define PINYIN_HDR_SIZE      48
#define PINYIN_OFF_VERSION    4
#define PINYIN_OFF_KEYCOUNT   8
#define PINYIN_OFF_CANDCOUNT 12
#define PINYIN_OFF_INICOUNT  16
#define PINYIN_OFF_INIOFF    20
#define PINYIN_OFF_INISTRIDE 24
#define PINYIN_OFF_INIMAXLEN 28
#define PINYIN_OFF_REFCOUNT  32
#define PINYIN_OFF_REFOFF    36
#define PINYIN_OFF_BUILDID   40

/* Candidate record: u16 nbytes, u32 freq, then nbytes of UTF-8. */
#define PINYIN_CAND_HDR      6

/* An initials row is ini[S] + u32 ref_first + u16 ref_n, so S = stride - 6.
 * A row's string must be >= 2 letters and NUL-terminated inside S bytes, which
 * puts a floor of 3+6 on any legal stride; 64 is a sanity ceiling so a corrupt
 * header cannot make ime_open() walk a table it computed from garbage. */
#define PINYIN_INI_TAIL      6
#define PINYIN_INI_STRIDE_MIN 9
#define PINYIN_INI_STRIDE_MAX 64

#endif /* C_LIB_IME_PINYIN_FMT_H_ */
