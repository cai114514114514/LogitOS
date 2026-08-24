#!/usr/bin/env python3
"""
Test the Pinyin dictionary binary format.

Tests that the dictionary:
1. Has the correct magic and version
2. Contains expected words (ni -> 你, nihao -> 你好, zhongguo -> 中国)
3. All candidates are valid UTF-8
4. All characters in candidates are in the font's cmap
5. Keys are sorted (binary search precondition)
"""

import struct
import sys
from pathlib import Path

# Import font reading from mkpinyin
sys.path.insert(0, str(Path(__file__).parent.parent.parent))
from tools.mkpinyin import read_font_cmap


def read_pinyin_dict(dict_path):
    """Read the Pinyin dictionary and return a dict of key -> list of candidates."""
    with open(dict_path, "rb") as f:
        # Read header
        magic = f.read(4)
        if magic != b"PYN\x00":
            raise ValueError(f"Bad magic: {magic}")

        version = struct.unpack("<I", f.read(4))[0]
        if version != 1:
            raise ValueError(f"Bad version: {version}")

        key_count = struct.unpack("<I", f.read(4))[0]

        dictionary = {}
        for _ in range(key_count):
            # Read key (null-terminated UTF-8)
            key_bytes = bytearray()
            while True:
                b = f.read(1)
                if not b or b[0] == 0:
                    break
                key_bytes.extend(b)

            key = key_bytes.decode("utf-8")

            # Read candidate count
            candidate_count = struct.unpack("<H", f.read(2))[0]

            candidates = []
            for _ in range(candidate_count):
                length = struct.unpack("<H", f.read(2))[0]
                data = f.read(length)
                candidate = data.decode("utf-8")
                candidates.append(candidate)

            dictionary[key] = candidates

    return dictionary


def test_pinyin_dict():
    """Test the Pinyin dictionary."""
    dict_path = Path(__file__).parent.parent.parent / "fsroot" / "ime" / "pinyin.dat"
    font_path = Path(__file__).parent.parent.parent / "fsroot" / "fonts" / "ui.ttf"

    if not dict_path.exists():
        print(f"SKIP: Dictionary not found at {dict_path}")
        return 0

    print(f"Testing {dict_path}")

    # Read font charset
    font_charset = read_font_cmap(str(font_path))
    print(f"  Font has {len(font_charset)} characters")

    # Read dictionary
    dictionary = read_pinyin_dict(str(dict_path))
    print(f"  Dictionary has {len(dictionary)} keys")

    errors = []

    # Test 1: Check that keys are sorted
    sorted_keys = sorted(dictionary.keys())
    if list(dictionary.keys()) != sorted_keys:
        errors.append("FAIL: Keys are not sorted")
    else:
        print("  PASS: Keys are sorted")

    # Test 2: Check expected words
    test_cases = [
        ("ni", "你"),  # You (singular)
        ("nihao", "你好"),  # Hello
        ("zhongguo", "中国"),  # China
        ("wo", "我"),  # I/me
        ("men", "们"),  # Plural marker
        ("de", "的"),  # Possessive
    ]

    for key, expected_first in test_cases:
        if key not in dictionary:
            errors.append(f"FAIL: Key '{key}' not in dictionary")
        elif dictionary[key][0] != expected_first:
            errors.append(
                f"FAIL: Key '{key}' first candidate is '{dictionary[key][0]}', expected '{expected_first}'"
            )
        else:
            print(f"  PASS: {key} -> {expected_first}")

    # Test 3: Check that all candidates are valid UTF-8 and in font
    total_candidates = 0
    invalid_utf8 = 0
    chars_not_in_font = set()

    for key, candidates in dictionary.items():
        for candidate in candidates:
            total_candidates += 1
            # Check UTF-8 validity (already decoded, so valid)
            # Check that all characters are in font
            for char in candidate:
                code = ord(char)
                if code not in font_charset:
                    chars_not_in_font.add((char, hex(code)))

    if chars_not_in_font:
        error_msg = f"FAIL: Found {len(chars_not_in_font)} characters not in font:"
        for char, code in sorted(chars_not_in_font)[:10]:
            error_msg += f"\n    {char} ({code})"
        errors.append(error_msg)
    else:
        print(f"  PASS: All {total_candidates} candidates have characters in font")

    # Report results
    if errors:
        print("\nERRORS:")
        for error in errors:
            print(f"  {error}")
        return 1
    else:
        print(f"\nAll tests passed ({len(dictionary)} keys, {total_candidates} candidates)")
        return 0


if __name__ == "__main__":
    sys.exit(test_pinyin_dict())
