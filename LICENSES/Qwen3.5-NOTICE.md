# Qwen3.5 vocabulary supplement

Source: https://huggingface.co/Qwen/Qwen3.5-4B/tree/851bf6e806efd8d0a36b00ddf55e13ccb7b8cd0a

Tokenizer SHA-256: `5f9e4d4901a92b997e463c1f46055088b6cca5ca61a6522d1b9f64c4bb81cb42`.
Downloaded via hf-mirror.com after the origin connection timed out; the
mirror's X-Repo-Commit and X-Linked-ETag match the revision/hash above.
`LICENSES/Qwen3.5.txt` is the Apache-2.0 license from that revision.

This is a modified, derived vocabulary, not the model or its tokenizer:
byte-level BPE symbols were reversed to bytes, decoded with strict UTF-8,
and filtered to pure U+4E00..U+9FFF text covered by the shipped font's actual
cmap. Added/control tokens and incomplete byte sequences were excluded.
53,426 pure Han tokens were found; 30,814 multi-character tokens were absent
from the legacy word list before the font filter. Tokens can be word fragments;
unknown terms have frequency 1, never their token ID as a fabricated count.

Pronunciations: pypinyin 0.55.0 (MIT), phrase-aware NORMAL with v_to_u=False.
Frequencies: the existing jieba 0.42.1 dictionary (MIT); observed counts are
kept, and entries absent from jieba get 1. Canonical pronunciations supplement
legacy pronunciations rather than deleting them (e.g. 女孩/nvhai plus nuhai).
Polyphones still depend on the pronunciation lexicon; no language model runs.

`qwen35-zh.tsv.gz` contains sorted key, text, frequency and initials columns.
Extraction dependencies are only needed to regenerate this input. Normal
builds use Python's standard library and the repository's existing v2 writer:

    python3 c/lib/ime/build_dictionary.py --root . \
      --tsv c/lib/ime/qwen35-zh.tsv.gz --output fsroot/ime/pinyin-qwen.dat

The output has 49,000 keys, 63,693 candidates and 1,983,418 bytes. SHA-256:
`aa9e62d93c6bb84be33a5b01418ab8a41bff3761ee12baea9ea9ab3b99cc42d8`.
The legacy `fsroot/ime/pinyin.dat` remains unchanged and independently tested.
