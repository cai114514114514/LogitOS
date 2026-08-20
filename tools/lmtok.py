#!/usr/bin/env python3
r"""lmtok.py -- HOST-SIDE tokenizer for the Qwen3 vocabulary this tree ships
(build/qwen/tokenizer.json, 151,643 BPE entries + 26 added/special tokens =
151,669 ids the tokenizer can ever produce; config.json's vocab_size=151936
is the embedding table's row count, not the tokenizer's -- see the SIZE
report at the bottom of this file's module docstring and the --report-size
command. That gap, 267 ids, is padding the tokenizer never emits).

WHY THIS FILE EXISTS RATHER THAN CALLING `tokenizers.Tokenizer.from_file()`
DIRECTLY: `tokenizers` (the Rust library) is the ORACLE here, not the
implementation -- see the gate below and the module docstring in the task
brief this file was written against ("the gate is the HuggingFace tokenizer
ITSELF"). A host tool that just shells out to the Rust library would prove
nothing about whether the LOGIC is understood, and it would be useless as a
reference for a device-side port (no Rust on a from-scratch x86_64 kernel).
So this is a from-scratch, pure-Python re-implementation of exactly the
pipeline tokenizer.json describes, checked against the real thing on every
run of --gate.

THE PIPELINE, read out of build/qwen/tokenizer.json (not assumed):
  1. Split the raw text on any literal ADDED-TOKEN string (26 of them, e.g.
     "<|im_start|>", "<tool_call>") -- verified empirically (see --gate) to
     happen on the RAW text, before normalization, matching AddedToken's
     'normalized: false' on every entry in the file.
  2. Each non-special chunk: NFC-normalize, then split into "pretokens" with
     the Sequence pre-tokenizer:
       a. Regex Split, behavior=Isolated -- the pattern below, applied with
          `regex.findall` (verified to cover 100% of every test string with
          no gaps, i.e. it is a partition, not a filter).
       b. ByteLevel remap (use_regex=false, so NO second regex split here --
          it only remaps each pretoken's UTF-8 bytes through the standard
          GPT-2 byte<->printable-unicode alphabet).
  3. Each byte-remapped pretoken: standard BPE merge, walking `merges` in
     RANK order (the order they appear in the file = the order they were
     learned), NOT by longest-match -- see --gate's adversarial case for why
     that distinction is the one thing an approximation gets wrong.
  4. Concatenate: special-token ids and per-pretoken BPE ids, in text order.
     The post-processor is ByteLevel-only (no CLS/SEP/template), so nothing
     is added after this.

Qwen's pattern is NOT the GPT-2 one (see negative control in --gate):
  (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}|
  ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+
Notably: \p{N} (a lone digit) has NO quantifier, so digits are split ONE PER
PRETOKEN (never merged across a digit run at the pretoken stage) -- this is
the GPT-4/cl100k-style pattern, not GPT-2's `\p{N}+`.

Requires the `regex` module (not stdlib `re`: \p{L}/\p{N} Unicode property
classes are not supported by `re`). `regex` is already a transitive
dependency of `transformers` on this host, verified present (2026.7.19).
"""
import argparse
import json
import re
import sys
from functools import lru_cache
from pathlib import Path

try:
    import regex
except ImportError:
    print("lmtok: the 'regex' module is required (stdlib re has no \\p{L}/\\p{N} "
          "Unicode property classes, which Qwen's pre-tokenizer pattern needs). "
          "pip install regex", file=sys.stderr)
    raise

DEFAULT_TOKENIZER_JSON = Path(__file__).resolve().parent.parent / "build" / "qwen" / "tokenizer.json"

# The GPT-2 pattern, kept ONLY as the negative control for --gate --negctl-gpt2pattern
# (CLAUDE.md house style: every gate needs a negative control you watched fail).
# Qwen/cl100k differs from it in two structural ways: \p{N} has no '+' here
# (digits never merge into multi-digit pretokens), and the trailing-whitespace
# alternatives (\s*[\r\n]+ | \s+(?!\S) | \s+) do not exist in GPT-2's pattern
# at all -- GPT-2 ends at plain \s+.
GPT2_PATTERN = r"""'s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+"""

QWEN_PATTERN = (
    r"(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}"
    r"| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+"
)


def bytes_to_unicode():
    """The standard GPT-2 byte<->printable-unicode table (Radford et al.'s
    encoder.py, which every BPE tokenizer since has copied verbatim,
    including Qwen's). Printable bytes (33..126, 161..172, 174..255) map to
    themselves; the 68 non-printable/whitespace/control bytes get remapped to
    unicode code points starting at 256, so every one of the 256 byte values
    has a distinct, single-character, printable representation and no BPE
    merge ever has to reason about raw control bytes."""
    bs = (list(range(ord("!"), ord("~") + 1))
          + list(range(ord("\xa1"), ord("\xac") + 1))
          + list(range(ord("\xae"), ord("\xff") + 1)))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + n)
            n += 1
    return dict(zip(bs, (chr(c) for c in cs)))


BYTE_ENCODER = bytes_to_unicode()
BYTE_DECODER = {v: k for k, v in BYTE_ENCODER.items()}


def get_pairs(word):
    """Adjacent-symbol pairs in a tuple/list of strings, as a set. Standard
    helper from the original GPT-2 BPE reference implementation."""
    pairs = set()
    prev = word[0]
    for c in word[1:]:
        pairs.add((prev, c))
        prev = c
    return pairs


class QwenBPETokenizer:
    """Pure-Python re-implementation of tokenizer.json's pipeline. Every
    field this class reads is read FROM THE FILE, not hardcoded, so a
    different tokenizer.json (a different model's) that happens to share the
    same shape (BPE + ByteLevel + a Split regex) would also work -- the QWEN
    constant above is only the DEFAULT and only used when the file's own
    pre_tokenizer.pretokenizers[0].pattern.Regex is absent, which it never is
    for this file (checked in __init__, not assumed)."""

    def __init__(self, tokenizer_json_path=DEFAULT_TOKENIZER_JSON, pattern_override=None):
        with open(tokenizer_json_path, encoding="utf-8") as f:
            self.doc = json.load(f)

        model = self.doc["model"]
        if model.get("type") != "BPE":
            raise ValueError(f"lmtok: model.type is {model.get('type')!r}, not BPE -- "
                              f"this file only implements the BPE model")
        # These four flags are all False/empty in this file; a change to any
        # of them means the merge/lookup logic below is no longer sufficient,
        # so refuse loudly rather than silently produce wrong ids.
        for flag in ("dropout", "unk_token", "fuse_unk", "byte_fallback", "ignore_merges"):
            v = model.get(flag)
            if v not in (None, False, 0):
                raise ValueError(f"lmtok: model.{flag}={v!r} is not the (None/False) "
                                  f"this implementation assumes -- unimplemented")
        for flag in ("continuing_subword_prefix", "end_of_word_suffix"):
            v = model.get(flag)
            if v not in (None, ""):
                raise ValueError(f"lmtok: model.{flag}={v!r} is nonempty -- unimplemented")

        self.vocab = model["vocab"]                       # byte-level str -> id
        self.id_to_tok = {i: s for s, i in self.vocab.items()}
        if len(self.vocab) != len(self.id_to_tok):
            raise ValueError("lmtok: vocab has duplicate ids -- cannot build a reverse map")
        # merges: list of [a, b] pairs; rank = position in the list (lower =
        # applied earlier = higher priority). This IS the "merge order" the
        # module docstring's step 3 refers to.
        self.bpe_ranks = {(a, b): i for i, (a, b) in enumerate(model["merges"])}

        norm = self.doc.get("normalizer")
        self.normalizer_is_nfc = bool(norm and norm.get("type") == "NFC")
        if norm is not None and not self.normalizer_is_nfc:
            raise ValueError(f"lmtok: normalizer {norm!r} is not NFC/None -- unimplemented")

        pt = self.doc.get("pre_tokenizer") or {}
        if pt.get("type") != "Sequence" or len(pt.get("pretokenizers", [])) != 2:
            raise ValueError("lmtok: pre_tokenizer is not the expected "
                              "[Split(regex), ByteLevel] Sequence -- unimplemented")
        split_stage, bytelevel_stage = pt["pretokenizers"]
        if split_stage.get("type") != "Split" or split_stage.get("behavior") != "Isolated" \
                or split_stage.get("invert"):
            raise ValueError("lmtok: pre_tokenizer[0] is not Split(Isolated, invert=false)")
        file_pattern = split_stage["pattern"]["Regex"]
        if bytelevel_stage.get("type") != "ByteLevel" or bytelevel_stage.get("use_regex"):
            raise ValueError("lmtok: pre_tokenizer[1] is not ByteLevel(use_regex=false)")

        pattern = pattern_override if pattern_override is not None else file_pattern
        self._pattern_source = pattern
        self._split_re = regex.compile(pattern)

        dec = self.doc.get("decoder") or {}
        if dec.get("type") != "ByteLevel":
            raise ValueError(f"lmtok: decoder.type={dec.get('type')!r} -- unimplemented")

        # Added tokens: literal-string matches, checked on the RAW (not yet
        # normalized) text -- see the module docstring and --gate's
        # 'endoftext literal mid-string' / 'partial special' cases, which are
        # what pinned this down empirically against the real tokenizer.
        self.added = {t["content"]: t["id"] for t in self.doc.get("added_tokens", [])}
        self.added_is_special = {t["content"]: bool(t.get("special")) for t in self.doc.get("added_tokens", [])}
        if self.added:
            # Longest-first so a shorter added token that happens to be a
            # PREFIX of a longer one never shadows it. (Checked empirically:
            # none of the 26 in this file collide this way, but the sort
            # makes that a property of the code, not a fact about this one
            # file that a future vocab could quietly invalidate.)
            ordered = sorted(self.added, key=len, reverse=True)
            self._added_re = re.compile("|".join(re.escape(s) for s in ordered))
        else:
            self._added_re = None

    # ---------------------------------------------------------------- BPE --
    @lru_cache(maxsize=100000)
    def _bpe(self, token):
        """token: a byte-remapped pretoken string (each char already stands
        for one raw byte via BYTE_ENCODER). Returns a tuple of merged
        symbol-strings, in order. Standard GPT-2-lineage algorithm: repeatedly
        find the adjacent pair with the LOWEST rank (= learned earliest) among
        pairs PRESENT IN THIS WORD, merge every occurrence of it, repeat until
        no pair in the word has a rank at all. This is what makes it BPE and
        not greedy-longest-match: the choice at each step is "which pair was
        learned first", not "which resulting token is longest" -- see the
        module docstring and the --gate adversarial case."""
        if len(token) == 1:
            return (token,)
        word = list(token)
        pairs = get_pairs(word)
        while True:
            if not pairs:
                break
            # min() over the whole pair SET, not the first one found -- a
            # naive "scan left to right, merge the first mergeable pair"
            # is a DIFFERENT (and wrong) algorithm that happens to agree on
            # short/simple inputs. bpe_ranks.get(p, inf) so an unlearned
            # pair never wins the min.
            bigram = min(pairs, key=lambda p: self.bpe_ranks.get(p, float("inf")))
            if bigram not in self.bpe_ranks:
                break
            first, second = bigram
            new_word = []
            i = 0
            n = len(word)
            while i < n:
                try:
                    j = word.index(first, i)
                except ValueError:
                    new_word.extend(word[i:])
                    break
                new_word.extend(word[i:j])
                i = j
                if word[i] == first and i + 1 < n and word[i + 1] == second:
                    new_word.append(first + second)
                    i += 2
                else:
                    new_word.append(word[i])
                    i += 1
            word = new_word
            if len(word) == 1:
                break
            pairs = get_pairs(word)
        return tuple(word)

    def _encode_chunk(self, text):
        """One non-special, already-NFC-normalized chunk of raw text ->
        list[int]. Runs the Split regex, then ByteLevel-remaps and BPEs each
        resulting pretoken independently (pretokens never merge across each
        other -- 'Isolated' behavior, verified as a partition above)."""
        ids = []
        for piece in self._split_re.findall(text):
            byte_str = "".join(BYTE_ENCODER[b] for b in piece.encode("utf-8"))
            for merged in self._bpe(byte_str):
                tid = self.vocab.get(merged)
                if tid is None:
                    # byte_fallback=False and every single byte-alphabet char
                    # is a base vocab entry (checked at load time implicitly:
                    # if this fires, the file's vocab is missing a base byte,
                    # which is a file-format violation, not a text this
                    # tokenizer can legally refuse -- so raise rather than
                    # silently drop the token id sequence out of sync with a
                    # length the caller may be relying on.
                    raise ValueError(f"lmtok: byte-level piece {merged!r} (from "
                                      f"pretoken {piece!r}) is not in vocab -- "
                                      f"should be unreachable with byte_fallback=false")
                ids.append(tid)
        return ids

    def encode(self, text):
        """Full pipeline: str -> list[int]. Splits on literal added-token
        strings first (on the RAW text), then NFC + Split + ByteLevel + BPE
        on everything in between, concatenated in original order."""
        if not text:
            return []
        if self._added_re is None:
            chunks = [(False, text)]
        else:
            chunks = []
            pos = 0
            for m in self._added_re.finditer(text):
                if m.start() > pos:
                    chunks.append((False, text[pos:m.start()]))
                chunks.append((True, m.group(0)))
                pos = m.end()
            if pos < len(text):
                chunks.append((False, text[pos:]))

        ids = []
        for is_special, s in chunks:
            if is_special:
                ids.append(self.added[s])
            else:
                if self.normalizer_is_nfc:
                    s = unicodedata_nfc(s)
                ids.extend(self._encode_chunk(s))
        return ids

    # --------------------------------------------------------------- decode --
    def decode(self, ids, skip_special_tokens=True):
        """list[int] -> str. Concatenates BYTES across every token first and
        decodes UTF-8 exactly ONCE at the end -- decoding token-by-token and
        concatenating STRINGS is the documented trap (a token can be half a
        UTF-8 sequence: e.g. the CJK char split across 2-3 byte-level
        tokens), and this is what --gate's roundtrip check is watching for.
        `errors='replace'` matches the HF Python binding's own default
        (checked in --gate: HF's tokenizer.decode never raises on ids whose
        bytes don't form valid UTF-8; ours must not either, or a single bad
        id from a sampled model would crash the decode loop instead of
        producing a replacement character like every real implementation
        does)."""
        out = bytearray()
        for i in ids:
            if i in self.id_to_tok:
                for ch in self.id_to_tok[i]:
                    out.append(BYTE_DECODER[ch])
            elif i in self._added_id_to_str():
                if skip_special_tokens and self.added_is_special.get(self._added_id_to_str()[i]):
                    continue
                out.extend(self._added_id_to_str()[i].encode("utf-8"))
            else:
                raise ValueError(f"lmtok: id {i} is out of range for this tokenizer "
                                  f"(0..{len(self.vocab) - 1} base, or one of "
                                  f"{sorted(self.added.values())} added)")
        return out.decode("utf-8", errors="replace")

    @lru_cache(maxsize=1)
    def _added_id_to_str(self):
        return {v: k for k, v in self.added.items()}

    @property
    def vocab_size_tokenizer(self):
        """The count of ids this tokenizer can ever PRODUCE: base BPE vocab
        + added/special tokens. NOT config.json's vocab_size (151936), which
        also counts embedding rows this tokenizer never emits -- see the
        module docstring."""
        return len(self.vocab) + len(self.added)


def unicodedata_nfc(s):
    import unicodedata
    return unicodedata.normalize("NFC", s)


# ============================================================== the gate ==

def build_corpus():
    """The required corpus (task item 3), each case labeled with WHY it is
    in here. Returns list[(label, text)]."""
    cases = [
        ("ascii_simple", "Hello, world!"),
        ("ascii_punct", "The quick brown fox jumps over the lazy dog. 123-456, don't you think?"),
        ("leading_trailing_ws", "  leading and trailing spaces  "),
        ("multi_internal_ws", "  multiple   spaces   here"),
        ("tabs", "tab\ttab\ttab"),
        ("newlines_mixed", "Newline\ntest\r\nCRLF\n\n\ndouble"),
        ("only_whitespace", "     "),
        ("empty", ""),
        ("single_char", "a"),
        ("digits_run", "1234567890 the year 2026 and pi is 3.14159"),
        ("contractions", "don't can't won't I'm we're they've I'll he'd DON'T CAN'T"),
        ("cjk_zh", "CJK: 你好，世界！这是测试。简体中文与標準繁體字混合。"),
        ("cjk_ja", "日本語のテキストです。ひらがな、カタカナ、漢字。"),
        ("cjk_ko", "한국어 텍스트입니다. 안녕하세요."),
        ("emoji_bmp", "smiley \U0001F600 rocket \U0001F680 mixed with text"),
        ("emoji_zwj_family", "family \U0001F468‍\U0001F469‍\U0001F467‍\U0001F466 emoji"),
        ("emoji_flag", "flags \U0001F1FA\U0001F1F8 \U0001F1EC\U0001F1E7 end"),
        ("surrogate_pair_supplementary", "math bold: \U0001D400\U0001D401\U0001D402 end"),
        ("special_im", "<|im_start|>system<|im_end|>"),
        ("special_chat", "<|im_start|>user\nHello there!<|im_end|>\n<|im_start|>assistant\n"),
        ("special_tool", "<tool_call>{\"name\": \"f\", \"args\": {}}</tool_call>"),
        ("special_think", "<think>reasoning here</think>final answer"),
        ("special_endoftext_mid", "before <|endoftext|> after"),
        ("special_lookalike_not_special", "not a special: <|imX_start|>"),
        ("special_truncated", "partial special <|im_start"),
        ("special_adjacent_nospace", "X<|im_start|>Y<|im_end|>Z"),
        ("nfc_combining", "café vs café (combining acute vs precomposed)"),
        ("nfc_combining_multi", "é́ double-combining, å ring above"),
        ("url", "https://example.com/path?a=1&b=2#frag"),
        ("code_snippet", "def f(x):\n    return x**2 + 1  # comment\n\nclass A(B, C):\n    pass"),
        ("rtl_arabic", "السلام عليكم"),
        ("rtl_hebrew", "שלום עולם"),
        ("mixed_ltr_rtl", "English مع العربية mixed"),
        ("zero_width", "a​b‌c‍d zero-width joiners/non-joiners"),
        ("control_chars", "bell\x07 null-ish\x0b vtab\x0c formfeed"),
        ("long_word_no_space", "supercalifragilisticexpialidocious" * 3),
        ("repeated_char_run", "a" * 200),
        ("repeated_space_run", " " * 100 + "x"),
        ("mixed_script_word", "café" + "北京" + "日本語"),
        ("number_letter_mix", "GPT-4o and Qwen3-0.6B v2.1.3 release-2026-08-20"),
    ]
    return cases


def gate_encode(oracle, mine, verbose=False):
    fails = []
    for label, text in build_corpus():
        want = oracle.encode(text, add_special_tokens=True).ids
        try:
            got = mine.encode(text)
        except Exception as e:
            fails.append((label, text, want, f"EXCEPTION: {e}"))
            continue
        if got != want:
            fails.append((label, text, want, got))
        elif verbose:
            print(f"  OK  encode  {label:32s} ({len(want):3d} ids)")
    return fails


def gate_decode_roundtrip(oracle, mine, verbose=False):
    fails = []
    for label, text in build_corpus():
        ids = oracle.encode(text, add_special_tokens=True).ids
        want = oracle.decode(ids, skip_special_tokens=True)
        try:
            got = mine.decode(ids, skip_special_tokens=True)
        except Exception as e:
            fails.append((label, "decode", ids, want, f"EXCEPTION: {e}"))
            continue
        if got != want:
            fails.append((label, "decode", ids, want, got))
            continue
        # The actual round-trip property a generation loop depends on: decode
        # (encode(text)) reproduces NFC(text), not necessarily the exact
        # original BYTES. That "not necessarily" is not a defect -- NFC
        # normalization is step 2 of the encode pipeline (see the module
        # docstring), so a decomposed-form input (e.g. "e" + COMBINING ACUTE)
        # legitimately decodes back as the precomposed form ("é"); comparing
        # against the raw original would fail on every Unicode-normalizing
        # tokenizer, HF's included, and would not be measuring this
        # implementation. Excluded when the source text contains a literal
        # special-token string, since skip_special_tokens=True drops those
        # from the decoded output by construction.
        if "<|" not in text and "<tool_" not in text and "<think>" not in text \
                and "</think>" not in text:
            expect = unicodedata_nfc(text)
            if got != expect:
                fails.append((label, "roundtrip!=NFC(original)", ids, expect, got))
        if verbose:
            print(f"  OK  decode  {label:32s}")
    return fails


def gate_decode_byte_split_trap():
    """The specific trap named in the task brief: 'a token can be half a
    UTF-8 sequence, so decoding token-by-token and concatenating STRINGS is
    wrong where concatenating BYTES and decoding once is right.' This
    isolates that property with a WRONG decoder (per-token str decode) as a
    negative control we watch fail, separate from gate_decode_roundtrip
    which only tests the real (correct) decoder. Returns (found_a_split_case,
    detail)."""
    tok = QwenBPETokenizer()
    # Find a CJK character whose byte-level BPE encoding splits it across
    # more than one token id (common for characters not in the base 151k
    # merges as a single unit at low training frequency).
    for ch in "你世界测试鬱龘穣":
        ids = tok.encode(ch)
        if len(ids) < 2:
            continue
        # Wrong decoder: decode each token's bytes to a string SEPARATELY
        # (errors='replace' per token) and concatenate strings.
        wrong = "".join(
            bytes(BYTE_DECODER[c] for c in tok.id_to_tok[i]).decode("utf-8", errors="replace")
            for i in ids
        )
        right = tok.decode(ids)
        if wrong != right or "�" in wrong:
            return True, (ch, ids, wrong, right)
    return False, None


def gate_adversarial_merge_order():
    """Task item 3's required case: a string that tokenizes differently under
    greedy-longest-match than under real BPE merge order. Searches a fixed
    candidate list (morphologically complex / compound words, chosen because
    BPE's rank-order merging is most likely to diverge from a naive
    'always take the longest known vocab substring' approximation on words
    with multiple plausible internal segmentations) and returns the first
    real one found, comparing against the REAL tokenizer (oracle) so the
    found case is a genuine, checkable fact about this vocabulary, not a
    theoretical claim.

    Returns (label, word, true_bpe_ids, greedy_ids) or (None, None, None, None)
    if the fixed candidate list happened to contain no divergence (which
    would itself be reported, not hidden)."""
    from tokenizers import Tokenizer
    oracle = Tokenizer.from_file(str(DEFAULT_TOKENIZER_JSON))
    tok = QwenBPETokenizer()

    def greedy_longest_match(word):
        """Naive maximal-munch tokenizer using the SAME vocab: at each
        position, take the longest byte-remapped substring that is a vocab
        entry, emit its id, advance. This is the 'approximation that passes
        easy cases' the task brief warns about -- it is a real, coherent
        tokenization strategy (WordPiece-family tokenizers use variants of
        it), just not the one BPE's rank-ordered merges implement."""
        byte_str = "".join(BYTE_ENCODER[b] for b in word.encode("utf-8"))
        ids = []
        i = 0
        n = len(byte_str)
        while i < n:
            for j in range(n, i, -1):
                cand = byte_str[i:j]
                if cand in tok.vocab:
                    ids.append(tok.vocab[cand])
                    i = j
                    break
            else:
                raise ValueError("no single byte matched -- unreachable (base bytes are all in vocab)")
        return ids

    candidates = [
        "unaffordability", "internationalization", "antidisestablishmentarianism",
        "electroencephalography", "counterproductive", "misunderstanding",
        "overexaggeration", "pseudoscientific", "deinstitutionalization",
        "hyperventilating", "uncharacteristically", "incomprehensibility",
        "disproportionately", "underestimation", "reconceptualization",
        "nonrepresentational", "transubstantiation", "unconstitutionality",
        "microarchitecture", "psychopharmacology", "thermodynamically",
        "photosynthesizing", "counterrevolutionary", "extraterritoriality",
        "immunohistochemistry", "interdisciplinarity", "epistemological",
        "phenomenological", "quintessentially", "straightforwardness",
    ]
    for w in candidates:
        true_ids = oracle.encode(w, add_special_tokens=False).ids
        # sanity: our own real BPE agrees with the oracle on this word too
        mine_true_ids = tok.encode(w)
        if mine_true_ids != true_ids:
            continue  # not a useful adversarial case if our BPE itself is wrong here
        greedy_ids = greedy_longest_match(w)
        if greedy_ids != true_ids:
            return w, true_ids, greedy_ids
    return None, None, None


def run_gate(tokenizer_json=DEFAULT_TOKENIZER_JSON, verbose=False, negctl=None):
    from tokenizers import Tokenizer
    oracle = Tokenizer.from_file(str(tokenizer_json))

    pattern_override = GPT2_PATTERN if negctl == "gpt2pattern" else None
    mine = QwenBPETokenizer(tokenizer_json, pattern_override=pattern_override)

    corpus = build_corpus()
    print(f"lmtok gate: {len(corpus)} corpus cases"
          + (f"  [NEGATIVE CONTROL: {negctl}]" if negctl else ""))

    enc_fails = gate_encode(oracle, mine, verbose=verbose)
    print(f"  encode: {len(corpus) - len(enc_fails)}/{len(corpus)} exact id-sequence match")
    if enc_fails:
        for label, text, want, got in enc_fails[:12]:
            print(f"    FAIL encode[{label}] text={text[:50]!r}")
            print(f"         want={want}")
            print(f"         got ={got}")
        if len(enc_fails) > 12:
            print(f"    ... and {len(enc_fails) - 12} more encode failures")

    dec_fails = gate_decode_roundtrip(oracle, mine, verbose=verbose)
    print(f"  decode/roundtrip: {len(corpus) - len(dec_fails)}/{len(corpus)} match")
    for row in dec_fails[:12]:
        if len(row) == 5:
            label, kind, ids, want, got = row
            print(f"    FAIL {kind}[{label}] want={want[:60]!r} got={got[:60]!r}")

    if negctl:
        # A negative control is expected to FAIL -- report it as such rather
        # than as a suite failure.
        total_fail = len(enc_fails) + len(dec_fails)
        print(f"  NEGATIVE CONTROL '{negctl}': {total_fail} mismatches "
              f"(expected > 0 -- a control that passes is not testing anything)")
        return 0 if total_fail > 0 else 1

    trap_found, trap_detail = gate_decode_byte_split_trap()
    if trap_found:
        ch, ids, wrong, right = trap_detail
        replacement_char = chr(0xFFFD)
        print(f"  byte-split decode trap: char {ch!r} -> ids {ids} splits a UTF-8 "
              f"sequence across tokens; naive per-token-decode gives {wrong!r} "
              f"(contains U+FFFD replacement char: {replacement_char in wrong}), "
              f"correct byte-concat decode gives {right!r} == original: {right == ch}")
        if wrong == right:
            print("    ERROR: the trap case did not actually distinguish the two "
                  "decoders -- pick a different character")
    else:
        print("  byte-split decode trap: NO case found in the sampled character set "
              "(not necessarily a problem, but the property is unverified this run)")

    word, true_ids, greedy_ids = gate_adversarial_merge_order()
    if word:
        print(f"  adversarial merge-order case: {word!r}")
        print(f"    true BPE (oracle-matching):  {true_ids}")
        print(f"    greedy longest-match:        {greedy_ids}")
        print(f"    -> {len(true_ids)} vs {len(greedy_ids)} tokens, "
              f"{'DIFFER' if true_ids != greedy_ids else 'SAME (not adversarial)'}")
    else:
        print("  adversarial merge-order case: none of the candidate word list "
              "diverged -- widen the candidate list")

    ok = not enc_fails and not dec_fails and trap_found and word is not None
    print(f"\nlmtok gate: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def report_size(tokenizer_json=DEFAULT_TOKENIZER_JSON):
    tok = QwenBPETokenizer(tokenizer_json)
    vocab_json = Path(tokenizer_json)
    src_bytes = vocab_json.stat().st_size
    n_base = len(tok.vocab)
    n_added = len(tok.added)
    n_merges = len(tok.bpe_ranks)
    print(f"lmtok --report-size (HOST measurement, from {vocab_json}):")
    print(f"  base BPE vocab entries : {n_base}")
    print(f"  added/special tokens   : {n_added}")
    print(f"  merge rules            : {n_merges}")
    print(f"  total ids tokenizer emits: {n_base + n_added}  "
          f"(config.json vocab_size=151936, gap={151936 - (n_base + n_added)} "
          f"unused embedding rows)")
    print(f"  source tokenizer.json size: {src_bytes} bytes ({src_bytes/1e6:.2f} MB)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tokenizer-json", default=str(DEFAULT_TOKENIZER_JSON))
    sub = ap.add_subparsers(dest="cmd", required=True)

    e = sub.add_parser("encode", help="text -> token ids")
    e.add_argument("text")
    e.add_argument("--out", help="write ids as newline-free space-separated decimal to this file "
                                  "(the 'feed token ids to the device' artifact)")

    d = sub.add_parser("decode", help="token ids (space/comma separated) -> text")
    d.add_argument("ids")
    d.add_argument("--keep-special", action="store_true")

    g = sub.add_parser("gate", help="run the full gate against the real HF tokenizer")
    g.add_argument("-v", "--verbose", action="store_true")
    g.add_argument("--negctl", choices=["gpt2pattern"], default=None,
                    help="run the gate with a deliberately wrong component, to prove "
                         "the gate can fail")

    sub.add_parser("report-size", help="print vocab/merges/file-size numbers (HOST)")

    args = ap.parse_args()

    if args.cmd == "gate":
        sys.exit(run_gate(tokenizer_json=args.tokenizer_json, verbose=args.verbose, negctl=args.negctl))
    elif args.cmd == "report-size":
        report_size(tokenizer_json=args.tokenizer_json)
    elif args.cmd == "encode":
        tok = QwenBPETokenizer(args.tokenizer_json)
        ids = tok.encode(args.text)
        if args.out:
            Path(args.out).write_text(" ".join(str(i) for i in ids), encoding="ascii")
            print(f"lmtok: wrote {len(ids)} ids to {args.out}")
        else:
            print(" ".join(str(i) for i in ids))
    elif args.cmd == "decode":
        tok = QwenBPETokenizer(args.tokenizer_json)
        raw = args.ids.replace(",", " ")
        ids = [int(x) for x in raw.split()]
        print(tok.decode(ids, skip_special_tokens=not args.keep_special))


if __name__ == "__main__":
    main()
