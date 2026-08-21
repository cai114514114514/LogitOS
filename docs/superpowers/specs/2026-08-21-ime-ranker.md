# The IME ranker: a language model, not a table

Status: design, 2026-08-21. Depends on the pinyin engine (`c/lib/ime`, in
flight) landing first. Numbers below are measured on this machine on
2026-08-20/21 unless marked as estimates.

## The three diseases, and why n-gram treats the least of them

| | symptom | n-gram |
|---|---|---|
| **does not know the word** | `shuanq` cannot produce 栓Q; the lexicon is jieba's 2020 word list | cannot help — ranking cannot surface a candidate that does not exist |
| **knows it, ranks it wrong for the context** | 觉觉子 above 绝绝子 after the user typed "这个梗" | weakly — a two-or-three-word window |
| **does not know the user** | a word typed a hundred times is still on page three | by counting — it learns what was typed, not what the user *would* type |

## The design: the ranker is an LM; the lexicon is a prior

1. **OOV words — pinyin-constrained decoding.** Candidates are *generated*
   by a character-level Chinese LM under a mask "characters whose reading
   matches this syllable", not looked up. A model trained on the Chinese
   internet already knows 栓Q / 绝绝子 / 电子榨菜; the lexicon does not and
   never will by itself. The lexicon remains as the prior and the fast path.
2. **Context — the model's own conditioning**, over everything typed so far
   in this field, not a window.
3. **Personalisation — learning, not counting.** Every commit is a training
   example `(context, chosen)`. This machine has a trainer (`tools/lmtrain.c`,
   beats gzip -9 on held-out bytes) and an inference engine (`c/lib/nn`).
   User preference is a small adapter on the last layers (a low-rank delta
   or an output-bias vector), updated in the background from the commit log
   with replay. A counter learns "you typed 栓Q"; an adapter learns "in this
   kind of context you use this kind of word" — it generalises to words the
   user has not typed yet. 自造词 (a multi-character commit assembled from
   single characters) is recorded with its pinyin as a lexicon entry as well.

## Why not Qwen directly, in numbers

Real Qwen3-0.6B, q4, on the device under KVM: **8.25 tok/s = 125 ms per
token**. Scoring ten candidates is ten forward passes even with the context's
KV cache reused — over a second per keystroke. Unusable live.

The tool is a **20–30 M parameter character-level Chinese LM**: ~1/20 of
Qwen's arithmetic, so roughly **100 tok/s on the device (estimate from the
measured 3.7–9.9 GFLOP/s), 10 ms per token**; re-ranking ten lexicon
candidates with the context cached ≈ 100 ms, inside a keystroke budget;
constrained decoding runs only on commit (space). Where it comes from: **our
own trainer with Qwen as the teacher** — Qwen3 is Apache-2.0, so distillation
output is unencumbered. Chinese Wikipedia is CC BY-SA and share-alike; it
stays out (a licence audit is running on this tree as this is written).

## The hard constraint: the model cannot be in the kernel

The IME engine runs in the window manager (ring 0) because the WM owns key
dispatch. `c/lib/nn` is ring 3 by rule (CLAUDE.md: a matmul in ring 0 holds
the BKL for the length of a layer). So the ranker is a **ring-3 service,
`imed`**, and the WM asks it over **AF_UNIX** (landed 2026-08-20) with
`(context, candidates[])` and gets back an ordering within a deadline; if the
service is absent or late, the lexicon order stands — the IME must never
block a keystroke on a model. This is exactly the consumer AF_UNIX was built
to have.

## What lands in which order

- **Layer 0 (in flight):** lexicon + syllable segmentation + frequency
  order, in `c/lib/ime`, freestanding, host-gated. The floor and the
  fallback. Its `ime_candidates()` list is where the ranker hook goes.
- **Layer 1:** the small char-LM (train with `lmtrain.c`, Qwen as teacher,
  gate: perplexity on held-out Chinese text against the teacher's, and a
  ranking gate — for a corpus of `(context, pinyin, correct word)` triples the
  LM-ranked candidate list must place the correct word first more often than
  the frequency order does, by a stated margin), `imed` over AF_UNIX, and the
  WM hook with the deadline.
- **Layer 2:** pinyin-constrained decoding for OOV; the per-user adapter
  trained from the commit log; 自造词.

## What this does not claim

- That a 20 M model knows every meme. It knows what its teacher knew as of
  the teacher's cutoff; beyond that, acquisition is local — what the user
  types and reads on this machine — and stays local.
- That learning replaces the lexicon. The lexicon is the prior that makes
  the common case cost nothing; the model is what makes the uncommon case
  possible.
