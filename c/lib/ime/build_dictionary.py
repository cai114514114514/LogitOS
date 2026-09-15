#!/usr/bin/env python3
"""Build the optional Qwen3.5 IME dictionary without loading a model.

Extraction needs pypinyin 0.55.0, jieba and fontTools; normal rebuilds use
only the checked-in TSV and Python's standard library. Token IDs are never
used as word frequencies: jieba supplies observed counts, unknown fragments
receive weight 1. The original dictionary remains a supported fallback.
"""
import argparse
import collections
import gzip
import hashlib
import importlib.util
import json
import sys
sys.dont_write_bytecode = True
from pathlib import Path


def decoder_map():
    bs = list(range(33, 127)) + list(range(161, 173)) + list(range(174, 256))
    cs = bs[:]
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + len(cs) - 188)
    return {chr(c): b for b, c in zip(bs, cs)}


def han_tokens(data):
    if data['model']['type'] != 'BPE' or data['decoder']['type'] != 'ByteLevel':
        raise ValueError('expected the Qwen3.5 byte-level BPE tokenizer')
    inv = decoder_map()
    excluded = {x['content'] for x in data['added_tokens']}
    words = set()
    for token in data['model']['vocab']:
        if token in excluded:
            continue
        try:
            word = bytes(inv[c] for c in token).decode('utf-8', errors='strict')
        except (KeyError, UnicodeDecodeError):
            continue
        if word and all('\u4e00' <= c <= '\u9fff' for c in word):
            words.add(word)
    return words


def extract(args, mk):
    from pypinyin import lazy_pinyin, Style, __version__
    from fontTools.ttLib import TTFont
    if __version__ != '0.55.0':
        raise ValueError('extraction is pinned to pypinyin 0.55.0')
    raw = args.tokenizer.read_bytes()
    if hashlib.sha256(raw).hexdigest() != '5f9e4d4901a92b997e463c1f46055088b6cca5ca61a6522d1b9f64c4bb81cb42':
        raise ValueError('tokenizer differs from the recorded Qwen3.5-4B revision')
    words = han_tokens(json.loads(raw))
    _, baseline = mk.read_dat(args.root / 'fsroot/ime/pinyin.dat')
    original = {w for _, cs in baseline for w, _ in cs}
    frequencies = mk.jieba_frequencies()
    with TTFont(args.root / 'fsroot/fonts/ui.ttf') as font:
        cmap = font.getBestCmap()
    # Also add canonical readings for existing phrases, e.g. 女孩 was stored
    # as nuhai; pypinyin's phrase-aware v spelling makes nvhai reachable.
    rows = set()
    font_dropped = 0
    for word in sorted(words | original):
        if not 1 <= len(word) <= 20:
            continue
        if any(ord(c) not in cmap for c in word):
            font_dropped += 1
            continue
        readings = lazy_pinyin(word, style=Style.NORMAL, v_to_u=False, errors='default')
        key = ''.join(readings)
        if len(readings) != len(word) or not key.isascii() or not key.isalpha() or len(key) > 64:
            continue
        rows.add((key, word, max(1, frequencies.get(word, 1)), ''.join(s[0] for s in readings)))
    text = ''.join(f'{k}\t{w}\t{f}\t{i}\n' for k, w, f, i in sorted(rows))
    args.tsv.parent.mkdir(parents=True, exist_ok=True)
    args.tsv.write_bytes(gzip.compress(text.encode(), mtime=0))
    print(json.dumps({'pure_han_tokens':len(words), 'new_multi_tokens':len({w for w in words-original if len(w)>1}),
                      'font_dropped':font_dropped, 'tsv_rows':len(rows)}, ensure_ascii=False))


def build(args, mk):
    _, baseline = mk.read_dat(args.root / 'fsroot/ime/pinyin.dat')
    # Preserve baseline pronunciations and frequency ties; canonical aliases
    # and Qwen terms are additional records, never a reassignment of offsets
    # in the legacy file. Initials for legacy entries use its verified mapping.
    initials = mk.derive_initials(baseline, mk.read_syllables(args.root / 'c/lib/ime/pinyin_syllables.inc'))
    if isinstance(initials, tuple):
        initials = initials[0]
    table = collections.defaultdict(dict)
    freq = {}
    for key, cs in baseline:
        for word, f in cs:
            table[key][word] = f
            freq[word] = f
    for line in gzip.decompress(args.tsv.read_bytes()).decode().splitlines():
        key, word, f, ini = line.split('\t')
        if not key.isascii() or not key.isalpha() or key != key.lower():
            raise ValueError(f'bad key {key!r}')
        if not 1 <= len(word) <= 20 or not all('\u4e00' <= c <= '\u9fff' for c in word):
            raise ValueError('bad Han candidate')
        if not 1 <= int(f) <= 0xffffffff or len(ini) != len(word):
            raise ValueError('bad frequency or initials')
        table[key][word] = int(f)
        freq.setdefault(word, int(f))
        initials.setdefault((key, word), ini)
    keys = [(k, sorted(cs.items(), key=lambda wf: -freq[wf[0]])) for k,cs in sorted(table.items())]
    if len(keys) > 65536:
        raise ValueError('expanded dictionary exceeds IME_MAX_KEYS')
    blob, stats = mk.build_v2(keys, freq, initials)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(blob)
    print(json.dumps(stats, ensure_ascii=False))
    print('sha256', hashlib.sha256(blob).hexdigest())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--tsv', type=Path, required=True)
    p.add_argument('--tokenizer', type=Path)
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    spec = importlib.util.spec_from_file_location('mkpinyin', args.root / 'tools/mkpinyin.py')
    mk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mk)
    if args.tokenizer:
        extract(args, mk)
    if args.output:
        build(args, mk)

if __name__ == '__main__':
    main()
