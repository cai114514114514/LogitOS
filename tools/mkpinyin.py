#!/usr/bin/env python3
"""Build fsroot/ime/pinyin.dat -- the IME dictionary c/lib/ime/pinyin.c reads.

THE FORMAT IS DEFINED IN c/lib/ime/pinyin_fmt.h, NOT HERE. That header carries
the byte table and the argument for every field; this file mirrors it. When the
two disagree the header wins and this file is the bug. (The previous version of
this docstring described a 28-byte header with a fixed-stride key[16] syllable
table and a separate run table. write_binary() had never written that. Nobody
noticed for as long as it stood, which is the whole case for one definition
site -- CLAUDE.md rule 3.)

TWO WAYS TO BUILD, AND ONLY ONE OF THEM RUNS ON THE DOCUMENTED HOST
-------------------------------------------------------------------
  python3 tools/mkpinyin.py --rebuild
        The full build: pypinyin readings + jieba's word list and frequencies,
        filtered to ui.ttf's cmap. Re-derives every key. NEEDS pypinyin, which
        is not installed on macOS/arm64 here -- it SKIPS LOUDLY with the one
        command that settles it rather than failing like a code defect
        (CLAUDE.md rule 5's shape, applied to a generator).

  python3 tools/mkpinyin.py --transform      [the default]
        Reads the COMMITTED fsroot/ime/pinyin.dat (v1 or v2), keeps its key
        section content byte-for-byte identical, and adds what v2 needs:
        jieba frequencies and the initials index. Runs here; jieba is present.

The transform is not a workaround, it is the smaller blast radius. A full
rebuild re-derives 25,945 keys and 35,374 candidates from a package version
nobody can pin on this host; it would move key_count, move the 西安-at-slot-8
case pinyin.h:96 pins, and move the four backtracking witnesses tests/ime.mk
counts -- all invisibly, all at the same time as a format bump. The transform
changes exactly the bytes v2 adds and nothing else, and `--transform` run over
its own v2 output is a fixpoint (verified by --check).

WHERE THE TWO NEW COLUMNS COME FROM
-----------------------------------
frequency: jieba's dict.txt, joined on the candidate string. 100% coverage --
    all 35,374 (key, candidate) entries appear in dict.txt, single characters
    included, zero misses. That is not luck: mkpinyin's own candidate source IS
    dict.txt (build_dictionary reads it and drops anything below min_frequency),
    so the join is a recovery of a number this generator computed and discarded
    one line before writing the file.

initials: derived by segmenting each key into EXACTLY len(candidate) legal
    syllables against c/lib/ime/pinyin_syllables.inc -- the engine's own
    414-entry table, read here rather than re-listed, so the two cannot drift.
    35,099 of 35,374 entries have exactly one such parse. The other 275 are the
    whole difficulty, and greedy-longest-first must NOT be the silent tiebreak:
    it gets 鞍钢 wrong ('angang' is an+gang -> "ag", greedy says a+ngang... and
    even a correct greedy picks "aa"), and 巴拿马 wrong ("bnm", greedy "bam").

    They are resolved with a SECOND source that is already in the file: the
    dictionary's own 4,262 single-character entries are a character->reading
    map, so each segment can be required to be a recorded reading of the
    character it covers. That takes 275 down to SIX, and six is a table a
    person can read (EXCEPTIONS below) instead of 275 lines nobody will.

    The six survive because the character's recorded single-character reading
    is the OTHER reading of a heteronym (刹 is filed as "sha", here it is
    "cha") or because the character has no single-character entry at all
    (尴, 尬). Each is listed with the parse that was REJECTED beside the one
    chosen, so review is a diff over six legible lines.

    A wrong initials mapping is invisible by construction: the entry is simply
    absent from the bucket a user types and present in one they never will.
    No crash, no log line, and the candidate is still reachable by its full
    spelling. That is why this is derived from data and cross-checked, not
    typed -- and why --rebuild, when pypinyin IS available, asserts the two
    derivations agree entry for entry instead of preferring one.
"""

import argparse
import collections
import os
import re
import struct
import sys
from pathlib import Path

MAGIC = b"PYN\x00"
VERSION = 2
FONT_FILE = "fsroot/fonts/ui.ttf"
OUTPUT = "fsroot/ime/pinyin.dat"
SYLLABLE_INC = "c/lib/ime/pinyin_syllables.inc"

HDR_SIZE = 48
CAND_HDR = 6      # u16 nbytes + u32 freq
INI_TAIL = 6      # u32 ref_first + u16 ref_n

# The six (key, word) pairs exact-count segmentation cannot resolve even with
# the dictionary's own reading map. chosen -> rejected, with the reason.
EXCEPTIONS = {
    #  key          word        chosen  rejected  why the automatic answer is wrong
    ("chana",     "刹那"):     ("cn",  "ca",  "刹 is filed as sha; here it is cha (cha+na)"),
    ("changan",   "长安"):     ("ca",  "cg",  "长 is filed as zhang; here it is chang (chang+an)"),
    ("ganga",     "尴尬"):     ("gg",  "ga",  "neither 尴 nor 尬 has a single-character entry (gan+ga)"),
    ("huangei",   "还给"):     ("hg",  "he",  "还 is filed as hai; here it is huan (huan+gei)"),
    ("luhuana",   "氯化钠"):   ("lhn", "lha", "氯 is filed as lv; this key is u-spelled (lu+hua+na)"),
    ("yichana",   "一刹那"):   ("ycn", "yca", "刹 again: yi+cha+na"),
}

# The generator-side half of the ü split named in pinyin_fmt.h note 4. The
# engine's syllable table spells these with v; 7 phrase keys spell them with u.
# Accepting both HERE lets the initials derivation parse those 7 without
# touching the syllable table or moving a single key -- which is the whole
# point: normalising the spelling is a different change with a different gate.
UE_ALIAS = {"nue": "nve", "lue": "lve", "nu": "nv", "lu": "lv"}


# ---------------------------------------------------------------------------
# font cmap (imported by tests/unit/pinyin_dict_test.py -- keep it importable)
# ---------------------------------------------------------------------------

def read_font_cmap(ttf_path):
    """Return the set of codepoints in a TrueType font's Unicode cmap."""
    with open(ttf_path, "rb") as f:
        f.seek(0)
        struct.unpack(">I", f.read(4))[0]      # sfntVersion
        num_tables = struct.unpack(">H", f.read(2))[0]
        f.read(6)                              # searchRange, entrySelector, rangeShift

        cmap_offset = None
        for _ in range(num_tables):
            tag = f.read(4)
            f.read(4)                          # checksum
            offset = struct.unpack(">I", f.read(4))[0]
            f.read(4)                          # length
            if tag == b"cmap":
                cmap_offset = offset
                break
        if not cmap_offset:
            raise ValueError("No cmap table found")

        f.seek(cmap_offset)
        struct.unpack(">H", f.read(2))[0]      # version
        num_cmaps = struct.unpack(">H", f.read(2))[0]

        for _ in range(num_cmaps):
            platform_id = struct.unpack(">H", f.read(2))[0]
            encoding_id = struct.unpack(">H", f.read(2))[0]
            offset_st = struct.unpack(">I", f.read(4))[0]
            if platform_id == 3 and encoding_id == 1:
                f.seek(cmap_offset + offset_st)
                fmt = struct.unpack(">H", f.read(2))[0]
                if fmt == 4:
                    f.read(2)                  # length
                    f.read(2)                  # language
                    seg_count = struct.unpack(">H", f.read(2))[0] // 2
                    end_codes = struct.unpack(f">{seg_count}H", f.read(seg_count * 2))
                    f.read(2)                  # reserved
                    start_codes = struct.unpack(f">{seg_count}H", f.read(seg_count * 2))
                    charset = set()
                    for i in range(seg_count - 1):   # skip the 0xFFFF sentinel
                        charset.update(range(start_codes[i], end_codes[i] + 1))
                    return charset
    raise ValueError("No Unicode cmap found")


# ---------------------------------------------------------------------------
# reading the committed dictionary (v1 or v2)
# ---------------------------------------------------------------------------

def read_dat(path):
    """Parse a v1 or v2 pinyin.dat into [(key, [(word, freq_or_None), ...]), ...].

    Key order and candidate order are preserved exactly as stored, which is
    what makes --transform a content-preserving operation rather than a
    rebuild: nothing here sorts.
    """
    b = Path(path).read_bytes()
    if b[:4] != MAGIC:
        raise ValueError(f"{path}: bad magic {b[:4]!r}")
    version, key_count = struct.unpack_from("<II", b, 4)
    if version == 1:
        off, chdr, has_freq = 12, 2, False
    elif version == 2:
        off, chdr, has_freq = HDR_SIZE, CAND_HDR, True
    else:
        raise ValueError(f"{path}: unsupported version {version}")

    keys = []
    for _ in range(key_count):
        end = b.index(b"\0", off)
        key = b[off:end].decode("ascii")
        off = end + 1
        ncand, = struct.unpack_from("<H", b, off)
        off += 2
        cands = []
        for _ in range(ncand):
            nbytes, = struct.unpack_from("<H", b, off)
            freq = struct.unpack_from("<I", b, off + 2)[0] if has_freq else None
            off += chdr
            cands.append((b[off:off + nbytes].decode("utf-8"), freq))
            off += nbytes
        keys.append((key, cands))
    return version, keys


# ---------------------------------------------------------------------------
# frequencies
# ---------------------------------------------------------------------------

def jieba_frequencies():
    """word -> frequency, from jieba's dict.txt. Loud skip if jieba is absent."""
    try:
        import jieba
    except ImportError:
        die_skip("jieba", "python3 -m pip install jieba==0.42.1")
    path = os.path.join(os.path.dirname(jieba.__file__), "dict.txt")
    freq = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                try:
                    freq[parts[0]] = int(parts[1])
                except ValueError:
                    pass
    print(f"jieba dict.txt: {len(freq)} words from {path}", file=sys.stderr)
    return freq


def die_skip(module, command):
    """SKIP LOUDLY: name the missing capability and the one command that settles it.

    Not an exception and not exit 1 -- a gate that cannot run on this host must
    not be indistinguishable from a gate that found a bug (CLAUDE.md, the
    host-reality table's fifth shape).
    """
    print(f"SKIP: python module '{module}' is not importable on this host, so this "
          f"build path cannot run.\nSKIP: settle it with:  {command}", file=sys.stderr)
    sys.exit(0)


# ---------------------------------------------------------------------------
# initials
# ---------------------------------------------------------------------------

def read_syllables(path=SYLLABLE_INC):
    """The engine's own 414-entry legal-syllable table, read from its .inc.

    Read rather than re-listed: a second copy of this table in Python is the
    exact shape CLAUDE.md rule 3 is about, and the failure would be silent --
    an initials string derived against a table the engine does not share is a
    bucket the engine can never look up.
    """
    text = Path(path).read_text()
    count = int(re.search(r"#define PINYIN_SYLLABLE_COUNT (\d+)", text).group(1))
    body = text.split("g_pinyin_syllables", 1)[1]
    syls = set(re.findall(r'"([a-z]+)"', body))
    if len(syls) != count:
        raise ValueError(f"{path}: parsed {len(syls)} syllables, header says {count}")
    return syls


def derive_initials(keys, syls, verbose=True):
    """(key, word) -> initials string, for every candidate of >= 2 characters.

    Two passes over exact-count segmentation: unconstrained first, then
    constrained to the readings the dictionary's own single-character entries
    record. The constrained pass is only CONSULTED when the unconstrained one
    is ambiguous, and its answer is only taken when it is unique -- a
    heteronym whose single-character filing is the other reading over-
    constrains to zero parses, and zero parses must not be mistaken for an
    answer. Those fall through to EXCEPTIONS.
    """
    maxsyl = max(len(s) for s in syls)

    readings = collections.defaultdict(set)
    for key, cands in keys:
        for word, _ in cands:
            if len(word) == 1:
                readings[word].add(key)

    def legal(seg):
        return seg in syls or UE_ALIAS.get(seg) in syls

    def parse(key, word, constrain):
        """Set of initials strings for segmentations of `key` into len(word) syllables."""
        out = set()
        n = len(word)

        def go(pos, k, acc):
            if len(out) > 4:            # ambiguous is ambiguous; stop counting
                return
            if k == 0:
                if pos == len(key):
                    out.add("".join(acc))
                return
            rem = len(key) - pos
            if rem < k or rem > k * maxsyl:
                return
            allowed = readings.get(word[n - k]) if constrain else None
            for L in range(1, min(maxsyl, rem - (k - 1)) + 1):
                seg = key[pos:pos + L]
                if not legal(seg):
                    continue
                if allowed is not None and seg not in allowed:
                    continue
                acc.append(seg[0])
                go(pos + L, k - 1, acc)
                acc.pop()

        go(0, n, [])
        return out

    out, stats = {}, collections.Counter()
    unresolved = []
    for key, cands in keys:
        for word, _ in cands:
            if len(word) < 2:
                stats["single"] += 1
                continue
            got = parse(key, word, False)
            how = "unique"
            if len(got) != 1:
                narrowed = parse(key, word, True)
                if len(narrowed) == 1:
                    got, how = narrowed, "by-reading"
                else:
                    exc = EXCEPTIONS.get((key, word))
                    if exc is None:
                        unresolved.append((key, word, sorted(got)))
                        continue
                    got, how = {exc[0]}, "exception"
            ini = next(iter(got))
            if len(ini) != len(word):
                raise ValueError(f"{key}/{word}: initials {ini!r} is not one letter per character")
            out[(key, word)] = ini
            stats[how] += 1

    if unresolved:
        print("mkpinyin: FAILED -- these entries have no single initials parse and no "
              "EXCEPTIONS row. Add them (with the rejected parse and the reason) rather "
              "than letting a heuristic guess:", file=sys.stderr)
        for key, word, got in unresolved[:40]:
            print(f"    ({key!r}, {word!r}): candidates {got}", file=sys.stderr)
        sys.exit(1)

    if verbose:
        print(f"initials: {stats['unique']} unique, {stats['by-reading']} resolved by the "
              f"dictionary's own character readings, {stats['exception']} from EXCEPTIONS "
              f"({len(EXCEPTIONS)} listed), {stats['single']} single-character entries not "
              f"indexed", file=sys.stderr)
    return out


def crosscheck_initials(keys, derived):
    """If pypinyin IS importable, assert it agrees entry for entry.

    A cross-check, not a second source of truth: on disagreement this FAILS
    rather than preferring one derivation, because "which of my two answers is
    right" is a question a generator must not answer by itself.
    """
    try:
        from pypinyin import lazy_pinyin, NORMAL
    except ImportError:
        print("crosscheck: SKIP -- pypinyin is not importable on this host, so the "
              "initials derivation was NOT cross-checked against it.\n"
              "crosscheck: settle it with:  python3 -m pip install pypinyin==0.55.0",
              file=sys.stderr)
        return 0
    import unicodedata

    def toneless(p):
        return "".join(c.lower() for c in unicodedata.normalize("NFD", p)
                       if unicodedata.category(c) != "Mn" and ord(c) < 128)

    bad, n = [], 0
    for key, cands in keys:
        for word, _ in cands:
            if len(word) < 2:
                continue
            want = "".join(toneless(p)[0] for p in lazy_pinyin(word, style=NORMAL))
            got = derived[(key, word)]
            n += 1
            if want != got:
                bad.append((key, word, got, want))
    if bad:
        print(f"crosscheck: FAILED -- {len(bad)} of {n} initials disagree with pypinyin:",
              file=sys.stderr)
        for key, word, got, want in bad[:40]:
            print(f"    {key}/{word}: derived {got!r}, pypinyin {want!r}", file=sys.stderr)
        sys.exit(1)
    print(f"crosscheck: ok -- all {n} initials agree with pypinyin", file=sys.stderr)
    return n


# ---------------------------------------------------------------------------
# writing v2
# ---------------------------------------------------------------------------

def fnv1a(data):
    h = 0x811c9dc5
    for byte in data:
        h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h


def build_v2(keys, freq, initials):
    """Serialise the v2 file. Returns (bytes, stats dict)."""
    # ---- key section, and remember where every candidate record landed ----
    body = bytearray()
    where = {}                       # (key, word) -> absolute byte offset
    cand_count = 0
    for key, cands in keys:
        body += key.encode("ascii") + b"\0"
        body += struct.pack("<H", len(cands))
        for word, _ in cands:
            utf8 = word.encode("utf-8")
            if len(utf8) > 0xFFFF:
                raise ValueError(f"{key}/{word}: candidate longer than a u16 length")
            where[(key, word)] = HDR_SIZE + len(body)
            body += struct.pack("<HI", len(utf8), freq[word]) + utf8
            cand_count += 1

    ini_off = HDR_SIZE + len(body)

    # ---- initials table + ref array ----
    buckets = collections.defaultdict(list)
    for key, cands in keys:
        for word, _ in cands:
            if len(word) < 2:
                continue
            buckets[initials[(key, word)]].append((key, word))

    ini_maxlen = max(len(s) for s in buckets)
    stride = ini_maxlen + 1 + INI_TAIL
    field = stride - INI_TAIL

    table, refs = bytearray(), bytearray()
    nrefs = 0
    for ini in sorted(buckets):
        # Descending frequency, file order as the tiebreak: the engine reads
        # the first min(n, budget) refs and stops, so the sort IS the ranking
        # and there is none at runtime (no allocator to sort with, either).
        entries = sorted(buckets[ini],
                         key=lambda kw: (-freq[kw[1]], where[kw]))
        table += ini.encode("ascii").ljust(field, b"\0")
        table += struct.pack("<IH", nrefs, len(entries))
        for kw in entries:
            refs += struct.pack("<I", where[kw])
        nrefs += len(entries)

    ref_off = ini_off + len(table)

    header = bytearray(MAGIC)
    header += struct.pack("<IIIIIIIII",
                          VERSION, len(keys), cand_count,
                          len(buckets), ini_off, stride, ini_maxlen,
                          nrefs, ref_off)
    header += struct.pack("<II", fnv1a(bytes(header) + bytes(body)), 0)
    assert len(header) == HDR_SIZE, len(header)

    out = bytes(header) + bytes(body) + bytes(table) + bytes(refs)
    stats = dict(keys=len(keys), cands=cand_count, buckets=len(buckets), refs=nrefs,
                 stride=stride, ini_maxlen=ini_maxlen, key_bytes=len(body),
                 ini_bytes=len(table), ref_bytes=len(refs), total=len(out),
                 biggest=sorted(((len(v), k) for k, v in buckets.items()), reverse=True)[:5])
    return out, stats


# ---------------------------------------------------------------------------
# the full rebuild (needs pypinyin)
# ---------------------------------------------------------------------------

def build_from_sources(font_charset, min_frequency=100):
    """Re-derive the key section from pypinyin + jieba. Loud skip without pypinyin."""
    try:
        from pypinyin import lazy_pinyin, NORMAL
        from pypinyin.phrases_dict import phrases_dict
    except ImportError:
        die_skip("pypinyin", "python3 -m pip install pypinyin==0.55.0 jieba==0.42.1")
    import unicodedata

    def toneless(p):
        return "".join(c.lower() for c in unicodedata.normalize("NFD", p)
                       if unicodedata.category(c) != "Mn" and ord(c) < 128)

    words = {w: f for w, f in jieba_frequencies().items() if f >= min_frequency}
    print(f"{len(words)} words at freq >= {min_frequency}", file=sys.stderr)

    cjk = sorted(c for c in font_charset if 0x4E00 <= c <= 0x9FFF)
    print(f"font has {len(cjk)} CJK characters", file=sys.stderr)
    char_pinyin = {}
    for code in cjk:
        got = lazy_pinyin(chr(code), style=NORMAL)
        if got and toneless(got[0]):
            char_pinyin[chr(code)] = toneless(got[0])

    dictionary = collections.defaultdict(list)
    for word, f in sorted(words.items(), key=lambda x: -x[1]):
        if not word or not all(ord(c) in font_charset for c in word):
            continue
        if word in phrases_dict:
            key = "".join(toneless(e[0] if isinstance(e, list) and e else e)
                          for e in phrases_dict[word])
        else:
            key = ""
            for ch in word:
                if ch not in char_pinyin:
                    key = None
                    break
                key += char_pinyin[ch]
        if key:
            dictionary[key].append((word, f))

    keys = []
    for key in sorted(dictionary):
        keys.append((key, sorted(((w, f) for w, f in dictionary[key]), key=lambda x: -x[1])))
    print(f"rebuilt {len(keys)} keys, {sum(len(c) for _, c in keys)} candidates",
          file=sys.stderr)
    return keys


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rebuild", action="store_true",
                    help="re-derive every key from pypinyin + jieba (needs pypinyin)")
    ap.add_argument("--transform", action="store_true",
                    help="keep the committed dictionary's key section, add v2's columns (default)")
    ap.add_argument("--input", default=OUTPUT, help=f"dictionary to transform (default {OUTPUT})")
    ap.add_argument("--output", default=OUTPUT, help=f"where to write (default {OUTPUT})")
    ap.add_argument("--check", action="store_true",
                    help="build and COMPARE against --output instead of writing it")
    ap.add_argument("--no-crosscheck", action="store_true",
                    help="skip the pypinyin agreement check on the initials derivation")
    args = ap.parse_args()

    if args.rebuild:
        font = read_font_cmap(FONT_FILE)
        print(f"font has {len(font)} characters", file=sys.stderr)
        keys = build_from_sources(font)
    else:
        version, keys = read_dat(args.input)
        print(f"{args.input}: version {version}, {len(keys)} keys, "
              f"{sum(len(c) for _, c in keys)} candidates", file=sys.stderr)

    freq = jieba_frequencies()
    missing = sorted({w for _, cands in keys for w, _ in cands if w not in freq})
    if missing:
        print(f"mkpinyin: FAILED -- {len(missing)} candidates have no jieba frequency, "
              f"so v2 cannot give them a score: {missing[:20]}", file=sys.stderr)
        sys.exit(1)

    syls = read_syllables()
    initials = derive_initials(keys, syls)
    if not args.no_crosscheck:
        crosscheck_initials(keys, initials)

    blob, st = build_v2(keys, freq, initials)
    print(f"v2: header 48 + keys {st['key_bytes']} + initials {st['ini_bytes']} "
          f"({st['buckets']} buckets x {st['stride']} B) + refs {st['ref_bytes']} "
          f"({st['refs']} x 4) = {st['total']} bytes ({st['total']/1048576:.2f} MiB)",
          file=sys.stderr)
    print(f"v2: {st['keys']} keys, {st['cands']} candidates, longest initials "
          f"{st['ini_maxlen']}, largest buckets {st['biggest']}", file=sys.stderr)

    if args.check:
        have = Path(args.output).read_bytes()
        if have == blob:
            print(f"mkpinyin --check: ok -- {args.output} is byte-identical to a fresh build",
                  file=sys.stderr)
            return 0
        print(f"mkpinyin --check: FAILED -- {args.output} is {len(have)} bytes, a fresh "
              f"build is {len(blob)}; they differ", file=sys.stderr)
        return 1

    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    Path(args.output).write_bytes(blob)
    print(f"wrote {len(blob)} bytes to {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
