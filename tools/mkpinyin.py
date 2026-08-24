#!/usr/bin/env python3
"""
Build a Pinyin dictionary for IME from pypinyin and jieba.

FORMAT (little-endian binary):
  Header (28 bytes):
    magic[4]      "PYN\x00"
    version       u32 = 1
    syllable_count u32  (number of syllable keys)
    run_count     u32  (number of candidate runs)
    reserved[4]   u32 = 0

  Syllable table (syllable_count * entry_size):
    Each entry:
      key[16]     null-terminated toneless syllable (e.g. "ni", "nihao")
      syllable_ct u16  (number of syllables; e.g. 1 for "ni", 2 for "nihao")
      run_offset  u32  (offset in the run table of first candidate for this key)
      run_count   u16  (number of candidates for this key)

  Candidate runs (sorted by frequency within each key):
    Each run:
      count       u16  (number of UTF-8 bytes)
      data[count] u8   (UTF-8 encoded character or phrase)

All offsets are from file start. Keys are sorted for binary search.
"""

import struct
import sys
from pathlib import Path
from pypinyin import lazy_pinyin, NORMAL
from pypinyin.phrases_dict import phrases_dict
import jieba

# Constants
MAGIC = b"PYN\x00"
VERSION = 1
FONT_FILE = "fsroot/fonts/ui.ttf"
OUTPUT = "fsroot/ime/pinyin.dat"

def read_font_cmap(ttf_path):
    """Read the cmap table from a TrueType font and return the set of available characters."""
    with open(ttf_path, "rb") as f:
        # Read the offset table (offset subtable)
        f.seek(0)
        version = struct.unpack(">I", f.read(4))[0]  # sfntVersion
        num_tables = struct.unpack(">H", f.read(2))[0]
        f.read(6)  # searchRange, entrySelector, rangeShift

        # Find the cmap table
        cmap_offset = None
        for _ in range(num_tables):
            tag = f.read(4)
            f.read(4)  # checksum
            offset = struct.unpack(">I", f.read(4))[0]
            f.read(4)  # length

            if tag == b"cmap":
                cmap_offset = offset
                break

        if not cmap_offset:
            raise ValueError("No cmap table found")

        # Read cmap table header
        f.seek(cmap_offset)
        cmap_version = struct.unpack(">H", f.read(2))[0]
        num_cmaps = struct.unpack(">H", f.read(2))[0]

        # Find Unicode platform (3) with encoding 1
        for _ in range(num_cmaps):
            platform_id = struct.unpack(">H", f.read(2))[0]
            encoding_id = struct.unpack(">H", f.read(2))[0]
            offset_st = struct.unpack(">I", f.read(4))[0]

            if platform_id == 3 and encoding_id == 1:
                # Read the subtable
                f.seek(cmap_offset + offset_st)
                fmt = struct.unpack(">H", f.read(2))[0]
                if fmt == 4:
                    f.read(2)  # length
                    f.read(2)  # language
                    seg_count_x2 = struct.unpack(">H", f.read(2))[0]
                    seg_count = seg_count_x2 // 2

                    end_codes = struct.unpack(f">{seg_count}H", f.read(seg_count * 2))
                    f.read(2)  # reserved
                    start_codes = struct.unpack(f">{seg_count}H", f.read(seg_count * 2))

                    charset = set()
                    for i in range(seg_count - 1):  # Skip sentinel at end
                        for c in range(start_codes[i], end_codes[i] + 1):
                            charset.add(c)

                    return charset

    raise ValueError("No Unicode cmap found")

def get_character_pinyin(char):
    """Get pinyin for a single character, using the first tone variant (toneless)."""
    try:
        pinyins = lazy_pinyin(char, style=NORMAL)
        if pinyins:
            py = pinyins[0]
            # Remove tone marks using NFD decomposition
            import unicodedata
            decomposed = unicodedata.normalize("NFD", py)
            p_toneless = ""
            for c in decomposed:
                # Skip combining marks (category Mn)
                if unicodedata.category(c) != "Mn":
                    if 0 <= ord(c) <= 127:  # ASCII only
                        p_toneless += c.lower()
            return p_toneless
    except:
        pass
    return None

def build_dictionary(font_charset, min_frequency=100):
    """Build the dictionary from pypinyin and jieba."""

    # Read jieba's dict.txt to get words and frequencies
    words = {}  # word -> frequency
    import os
    dict_path = os.path.join(os.path.dirname(jieba.__file__), "dict.txt")

    print(f"Reading jieba dictionary from {dict_path}", file=sys.stderr)
    with open(dict_path, "r", encoding="utf-8") as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 2:
                word = parts[0]
                try:
                    freq = int(parts[1])
                    if freq >= min_frequency:  # Filter by frequency
                        words[word] = freq
                except:
                    pass

    print(f"Read {len(words)} words from jieba (freq >= {min_frequency})", file=sys.stderr)

    # Get character pinyin (toneless, lowercase)
    char_pinyin = {}
    cjk_range = set(c for c in font_charset if 0x4E00 <= c <= 0x9FFF)
    print(f"Font has {len(cjk_range)} CJK characters", file=sys.stderr)

    for code in sorted(cjk_range):
        char = chr(code)
        py = get_character_pinyin(char)
        if py:
            # Remove tones and convert to lowercase
            py_toneless = ""
            for c in py:
                if ord(c) > 127:
                    # Skip tone marks
                    continue
                py_toneless += c.lower()
            if py_toneless:
                char_pinyin[char] = py_toneless

    print(f"Got pinyin for {len(char_pinyin)} characters", file=sys.stderr)

    # Build dictionary: key (toneless pinyin) -> list of (candidate, frequency)
    # Use pypinyin's phrase dictionary first, then fallback to character concatenation
    dictionary = {}  # key -> [(candidate, freq, is_phrase), ...]

    heteronym_fallback_count = 0

    for word, freq in sorted(words.items(), key=lambda x: -x[1]):
        if not word:
            continue

        # Check if all characters are in font
        if not all(ord(c) in font_charset for c in word):
            continue

        # Get pinyin for the word
        key = None
        is_phrase = False

        # First try pypinyin's phrase dictionary
        if word in phrases_dict:
            py_list = phrases_dict[word]
            key = ""
            for py_entry in py_list:
                # Each entry is a list of pronunciations (handling heteronyms)
                # Take the first (most common) one
                if isinstance(py_entry, list) and len(py_entry) > 0:
                    p = py_entry[0]
                else:
                    p = py_entry

                # Remove tone marks using NFD decomposition
                # Tone marks in pinyin are combining diacritics after base characters
                import unicodedata
                # Decompose to separate base characters from combining marks
                decomposed = unicodedata.normalize("NFD", p)
                # Keep only ASCII characters and decomposed base characters
                p_toneless = ""
                for c in decomposed:
                    # Skip combining marks (category Mn)
                    if unicodedata.category(c) != "Mn":
                        if 0 <= ord(c) <= 127:  # ASCII only
                            p_toneless += c.lower()
                key += p_toneless
            is_phrase = True
        else:
            # Fallback: concatenate character pinyin
            key = ""
            for char in word:
                if char in char_pinyin:
                    key += char_pinyin[char]
                else:
                    key = None
                    break

            if key:
                heteronym_fallback_count += len(word)

        if not key:
            continue

        if key not in dictionary:
            dictionary[key] = []
        dictionary[key].append((word, freq))

    # Sort candidates by frequency (descending)
    for key in dictionary:
        dictionary[key].sort(key=lambda x: -x[1])

    print(f"Built dictionary with {len(dictionary)} pinyin keys", file=sys.stderr)
    print(f"Heteronym fallback used for {heteronym_fallback_count} characters", file=sys.stderr)

    # Count dropped words
    all_words_in_font = sum(1 for w in words if all(ord(c) in font_charset for c in w))
    words_in_dict = sum(len(candidates) for candidates in dictionary.values())
    print(f"Words in font: {all_words_in_font}", file=sys.stderr)
    print(f"Words in dictionary: {words_in_dict}", file=sys.stderr)
    print(f"Words dropped: {all_words_in_font - words_in_dict}", file=sys.stderr)

    # Find top dropped words
    dropped = []
    for word, freq in sorted(words.items(), key=lambda x: -x[1]):
        if word and all(ord(c) in font_charset for c in word):
            # Check if it's in dictionary by reconstructing the key
            key = ""
            for char in word:
                if char in char_pinyin:
                    key += char_pinyin[char]
                else:
                    key = None
                    break

            if key is None:  # Character missing pinyin
                dropped.append((word, freq))
                if len(dropped) == 10:
                    break

    if dropped:
        print(f"Top 10 dropped words by frequency:", file=sys.stderr)
        for word, freq in dropped:
            print(f"  {word}: {freq}", file=sys.stderr)

    return dictionary, font_charset

def write_binary(dictionary, font_charset, output_path):
    """Write the dictionary to a binary file with a compact format.

    Compact format (variable-length):
      Header (12 bytes):
        magic[4]    "PYN\x00"
        version     u32 (little-endian) = 1
        key_count   u32 (little-endian)

      Key entries (one per sorted key):
        key_str     null-terminated UTF-8 string
        candidates  u16 (little-endian, number of candidates)
        For each candidate:
          length    u16 (little-endian, bytes in UTF-8)
          data      UTF-8 encoded character/phrase
    """

    # Sort keys for binary search
    sorted_keys = sorted(dictionary.keys())

    # Build the file
    output = bytearray()

    # Header
    output.extend(MAGIC)
    output.extend(struct.pack("<I", VERSION))
    output.extend(struct.pack("<I", len(sorted_keys)))

    # Key entries
    for key in sorted_keys:
        candidates = dictionary[key]

        # Write key string (null-terminated)
        output.extend(key.encode("utf-8"))
        output.append(0)  # Null terminator

        # Write candidate count
        output.extend(struct.pack("<H", len(candidates)))

        # Write candidates (sorted by frequency, descending)
        for candidate, freq in candidates:
            utf8_bytes = candidate.encode("utf-8")
            output.extend(struct.pack("<H", len(utf8_bytes)))
            output.extend(utf8_bytes)

    # Write to file
    Path(output_path).parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(output)

    print(f"Wrote {len(output)} bytes to {output_path}", file=sys.stderr)
    print(f"Dictionary size: {len(output) / 1024 / 1024:.2f} MiB", file=sys.stderr)

if __name__ == "__main__":
    print(f"Reading font from {FONT_FILE}", file=sys.stderr)
    font_charset = read_font_cmap(FONT_FILE)
    print(f"Font has {len(font_charset)} characters", file=sys.stderr)

    print(f"Building dictionary...", file=sys.stderr)
    # Use a reasonable frequency threshold to keep the file size manageable
    # Words with freq >= 100 gives us ~36k words which should be good for IME
    dictionary, charset = build_dictionary(font_charset, min_frequency=100)

    print(f"Writing binary dictionary...", file=sys.stderr)
    write_binary(dictionary, charset, OUTPUT)

    print("Done.", file=sys.stderr)
