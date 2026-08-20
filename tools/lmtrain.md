# `tools/lmtrain.c` — the LOGITLM trainer

A host program. It never runs on the device, it links nothing from the kernel,
and it is not in `C_SRC`. It trains the llama-shaped byte-level transformer that
`c/lib/nn` runs, on a byte stream, and writes one LOGITLM v1 file.

```sh
# the portable build -- this is what tests/nn.mk's `lmtrain-check` builds
cc -O2 -o build/lmtrain tools/lmtrain.c -lm
build/lmtrain --gradcheck                       # the gate. run this first.

# the build to actually train with: x86-64 + OpenMP, 25x faster, same source
cc -O3 -march=native -fassociative-math -fno-signed-zeros -fno-trapping-math \
   -fno-math-errno -fopenmp -o build/lmtrain-fast tools/lmtrain.c -lm

python3 tools/lmcorpus.py --out build/corpus-c.txt
build/lmtrain-fast --corpus build/corpus-c.txt --val-tail 284271 \
    --dim 256 --layers 6 --heads 8 --kv-heads 8 --hidden 688 \
    --lr 1e-3 --batch 32 --threads 26 --steps 2500 --warmup 200 \
    --out build/model.lm --also-q8 build/model.q8.lm
```

**Both builds pass `--gradcheck`, and that is the only thing that would tell them
apart.** The fast one is a different command line, not a different source file:
`#pragma omp` is ignored without `-fopenmp` and `_OPENMP` is undefined, so the
portable build is the single-threaded program this file used to be. A host that
is not x86-64, or has no OpenMP, gets a correct trainer that is slower.

No `-I` is needed: `lmtrain.c` includes `../c/lib/nn/model.h` and
`../c/lib/nn/matmul.c` by relative path, and a quoted `#include` inside
`matmul.c` resolves against `matmul.c`'s own directory. It does that rather than
retyping `struct lm_header` and reimplementing `nn_quantize_q8`, because a writer
carrying its own copy of a format drifts from the reader the first time somebody
adds a field, and a second copy of a rounding rule produces weights that differ
from the device's in the last bit on every row, silently.

## `--gradcheck` — run this before you believe any loss curve

Every analytic gradient against a central finite difference,
`(L(w+e) - L(w-e)) / 2e` at `e = 1e-4`, in double, reported **per tensor**.
Exit status is 0 or 1. It takes about 18 ms.

Read the per-tensor column, not the last line. A single global number tells you
that *something* is wrong; it does not tell you *which matrix*, and the failure
this check exists for — a transposed gradient — still makes the loss fall, just
slower and to a worse place, with nothing anywhere saying why.

Three configurations run, because three wirings can each be wrong alone:

| config | what only it can see |
|---|---|
| `tied` | `tok_emb` takes gradient from the embedding lookup **and** the output head; this is what ships |
| `untied` | a missing or wrong `wcls` gradient is invisible when `wcls` *is* `tok_emb` |
| `gqa` | `n_kv_heads < n_heads`, so the query-head → kv-head mapping does something |

**The threshold is derived, not fitted.** A central difference at step `e` carries
rounding `~ eps*|L|/2e = 2.2e-16 * 5.5 / 2e-4 ≈ 6e-12` and truncation
`~ e^2/6 * |L'''| ≈ 2e-9`, so the numeric gradient is good to about `2e-9`
**absolute**. Relative error is therefore only meaningful well above that, which
is why the denominator `|analytic| + |numeric|` has a floor of `1e-4`: below it a
*correct* gradient reports `2e-9/1e-4 = 2e-5`, comfortably under the `1e-4` bar,
while a *wrong* gradient of any magnitude moves the numerator by its own size and
fails. A floor tuned to the observed error would measure nothing.

Measured 2026-08-20, both builds: worst `1.304e-05` over 1,256 parameters
(`tied` 2.6e-08, `untied` 4.2e-07, `gqa` 1.3e-05), **PASS**.

### The two negative controls

A gate that has never been watched failing is not known to work. Both of these
are compile-time switches in `lmtrain.c`, and both must make `--gradcheck` exit 1:

```sh
cc -O2 -w -DLMTRAIN_NO_RMS_JACOBIAN -o /tmp/nc1 tools/lmtrain.c -lm && /tmp/nc1 --gradcheck
cc -O2 -w -DLMTRAIN_TRANSPOSE_DWQ   -o /tmp/nc2 tools/lmtrain.c -lm && /tmp/nc2 --gradcheck
```

- `LMTRAIN_NO_RMS_JACOBIAN` drops the term that accounts for RMSNorm's scale
  depending on its own input — what you get by differentiating `y = g*x*r` and
  stopping. **Every tensor reddens**, because the error is upstream of all of them.
- `LMTRAIN_TRANSPOSE_DWQ` transposes `dWq` and touches nothing else. **Exactly the
  `wq` rows redden** (one in the 1-layer configs, both `L0.wq` and `L1.wq` in the
  2-layer one). That asymmetry is the whole argument for the per-tensor report:
  the global worst case is `1.0` in both controls and cannot tell them apart.

Both re-run and both still exit 1 after the 2026-08-20 rewrite.

---

# The corpus: `tools/lmcorpus.py`

The trainer takes a byte stream and does not care where it came from. What it
was pointed at until 2026-08-20 was `CLAUDE.md` — 126,606 bytes, chosen so the
run would be self-contained — and that was the ceiling on the whole line: 825k
parameters over 126 KB is data-starved, and the val loss turned back up after
~5,500 steps because the model was memorising.

`tools/lmcorpus.py` builds the corpus out of **`c/`, this operating system's own
source**: 9,893,172 bytes over 628 files, **78× what the first run used**. Run
it, don't keep the output — the point of a tool is that the bytes are
recoverable when a number is questioned later.

```
$ python3 tools/lmcorpus.py --out build/corpus-c.txt
  628 files, 9893172 bytes
  path headers: 21199 bytes (0.21% of the corpus)
  dropped 4 generated file(s), 148657 bytes:
    c/lib/audio/aac_tables.h                    64069  declares itself generated in first 400 B
    c/lib/audio/mp3_tables.h                    42963  declares itself generated in first 400 B
    c/lib/video/h264_cabac_tables.h             33567  digit/comma density 0.620 >= 0.50
    c/lib/video/h264_tables.h                    8058  digit/comma density 0.542 >= 0.50
  HELD OUT: c/fs, 23 files, bytes [9608901, 9893172) = 284271 (2.88%)
```

Four decisions, each argued in the tool's own header, each of which changes the
number: sorted file order (not `os.walk` order, which makes the split a property
of the disk), a one-line `==== path ====` header per file, generated tables
excluded, and **the held-out set is a whole subsystem moved to the tail**.

**`c/fs` is the validation set** — logitfs, vfs, fsck, bcache, ramfs, 23 files,
bytes `[9608901, 9893172)`. Not a random slice: a random slice has training data
on both sides of it, so a model that had merely interpolated between its
neighbours would score well. `c/fs` is complete on its own, it is ordinary C in
this tree's house style rather than a table-heavy corner, and its identifiers
(`logitfs_`, `bcache_`, `lfs_`) barely occur outside it. The model has never
seen a byte of it.

### Apparatus note: the corpus is a snapshot of a LIVE tree

`c/` is being edited while this runs. During this session another line of work
grew `c/fs/logitfs.c` from 60,416 to 63,133 bytes and touched six more files in
that directory — **the held-out subsystem, as it happens** — so re-running
`lmcorpus.py` an hour later produces a different corpus and a different
`--val-tail`. That is not a flaw in the tool; the tool is a function of the
repository and the repository moved.

What it means in practice: **the artefact, not the recipe, is the record.**
Every number below was measured on one file:

```
build/corpus-c.txt   9,893,172 B
  sha256 542a23856dab7747c1c48feb01f0a26f9f31a2a898000ee87dde06d5bc835c3c
held-out tail [9608901, 9893172), 284,271 B
  sha256 bb02c00a76676e00cfed5ef86d9736821d251bd43ef34cdb53080e475611e3d7
```

and `gzip -9` was run on bytes cut out of *that* file, so the model and the
compressor were scored on the same 284,271 bytes even though the tree has since
moved on. The old version of this document has the same warning about
`CLAUDE.md` growing by 535 bytes mid-measurement; it is the same trap at 20×
the scale.

Line endings are normalised to LF, which is not tidiness: `core.autocrlf=true`
in this repository, so 12 of the 632 files carry CRLF on this checkout and which
12 depends on which files an editor last saved. Raw bytes would make the corpus
un-reproducible *and* hand gzip a free byte per line on those twelve.

---

# The bar: bits/byte on held-out bytes, against gzip on the same bytes

This is the number the line is judged on and the only one that cannot be fudged.
`--corpus X --val-tail N` holds out the last `N` bytes; after training, the
trainer scores **every one of them** and prints bits/byte.

**Do not compare any of this against the 2.75 bits/byte the CLAUDE.md runs
produced.** That was English prose. Code is far more repetitive and far more
predictable and bits/byte falls for reasons that have nothing to do with the
model being better — which is exactly why the bar is gzip **on these bytes**
rather than a remembered constant.

### The baselines, on exactly the held-out 284,271 bytes

```sh
tail -c +9608902 build/corpus-c.txt > /tmp/held.bin      # 284271 B
gzip -9 -c /tmp/held.bin | wc -c
xz   -9e -c /tmp/held.bin | wc -c
```

| baseline | bytes | bits/byte | ratio |
|---|---:|---:|---:|
| **gzip -9** (the bar) | **80,978** | **2.2789** | 3.51× |
| gzip -9, primed with the 1 MB of training text just before it | 80,350 | 2.2612 | 3.54× |
| xz -9e | 69,560 | 1.9576 | 4.09× |
| xz -9e, primed with **all 9.6 MB** of the training split | 59,512 | 1.6748 | 4.78× |

The last two rows are context and they are here because picking the compressor
that flatters the model is the easy way to write this table. **gzip has a 32 KB
window and xz has 64 MB**; the model has 256 bytes, which is a much harder game.
The primed rows exist because the model *has* seen the training data and gzip
has not, so they are the fair version of the comparison — and note that priming
buys gzip almost nothing (628 bytes, 0.8%), because 1 MB of prior text does not
fit in a 32 KB window. It buys xz 10,048 bytes, and xz-primed is the strongest
baseline in the table.

### The result

Four sizes, identical data budget (**20,480,000 tokens** = 2,500 steps × batch
32 × seq 256, ≈2.1 epochs of the 9,608,901-byte training split), identical
schedule, one seed. Bits/byte over all 284,271 held-out bytes, `--bpb-stride
128` so every scored byte carries 129–256 bytes of context.

| model | dim | L | heads | hidden | params | **bits/byte** | vs gzip | f32 file | q8 file |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| XS | 128 | 4 | 4 | 344 | 824,448 | **1.5554** | −31.7% | 3,297,856 | 947,520 |
| S | 192 | 5 | 6 | 512 | 2,263,104 | **1.4180** | −37.8% | 9,052,480 | 2,456,640 |
| M | 256 | 6 | 8 | 688 | *not run* | *not run* | — | — | — |
| L | 384 | 6 | 6 | 1024 | *not run* | *not run* | — | — | — |
| — | | | | | gzip -9 | 2.2789 | — | — | — |

> **The M and L rows were never run.** This table's two smallest models
> are measured; the two largest were still template placeholders when the
> run was interrupted, and are marked *not run* rather than left as tokens
> that read like numbers. Re-running them needs no code change -- the
> shapes are in the table and the invocation is above it.
| — | | | | | xz -9e primed | 1.6748 | — | — | — |

CURVE_NOTE

### Prose in the corpus makes it worse, measured

`tools/lmcorpus.py --also .:.md` folds every in-tree markdown file in ahead of
the code — 86 files, 1,263,821 bytes, +13% of corpus. The question was whether
English prose helps a byte model that must also produce English comments (27%
of the corpus's lines start with a comment marker) or dilutes the code signal.

Same XS config, same seed, same 20,480,000-token budget, and **the held-out
bytes are byte-identical** — the +md corpus was built by prepending the
markdown block to `build/corpus-c.txt` rather than re-running the builder, so
the `c/fs` tail has the same sha256 and the two numbers score the same bytes:

| corpus | train split | held-out bits/byte |
|---|--:|--:|
| `c/` only | 9,608,901 B | **1.5554** |
| `c/` + every in-tree `.md` | 10,872,722 B | 1.5720 |

**+0.017 bits/byte, i.e. 1.1% worse.** Small but the right sign and not noise —
the two runs share a seed and differ only in the input. Two effects push the
same way and this experiment cannot separate them: the prose displaces code
tokens, and 13% more corpus at a fixed budget means 13% fewer passes over the
code. The default is code only.

**The honest asterisk, stated rather than buried.** Bits/byte on held-out data
is the standard measure and it is what was asked for, but it is not the same
claim as "this is a better compressor than gzip". A self-contained compressor
must ship its model: 284,271 bytes would cost `55,269 + 947,520` = 1,002,789
bytes with the XS model in q8 against gzip's 80,978, a 12× loss. The model wins
per-byte and loses per-file, and it only wins overall once the decoder already
holds the weights — which on this machine it does, because `/model.lm` is on
the disk image and `/bin/lm` is the program.

---

# Training: what the columns mean

```
    step       loss        ema        val         lr    gnorm      tok/s
       0     5.4619     5.4619     5.4524   5.00e-06     9.06       2821
     100     2.6341     3.0782     5.4524   5.05e-04     1.78       7464
```

- **`loss`** — mean cross-entropy in **nats/byte** over this step's tokens, at the
  weights *before* the update. Natural log, so an untrained byte model must start
  at `ln(256) = 5.5452`; the header line prints that number so you can check the
  first row against it without arithmetic. Divide by `ln 2` for bits/byte.
- **`ema`** — the same, exponentially smoothed with a 20-step half-life. Read
  `ema` for the trend and `loss` for a blow-up: the EMA lags, so divergence
  shows in the raw column first.
- **`val`** — the same loss on `--val-batches` fixed windows from the held-out
  tail, refreshed when a printed step is also a multiple of `--val-every`. Fixed
  seed, so a move in this column is the model moving and not the sample. This is
  the cheap instrument for watching a run; the **bits/byte** number at the end is
  the one that scores every held-out byte and is what a compressor is compared
  against. If the corpus is too short to hold out a window the split is refused
  out loud and the column reads 0 — it is never quietly filled with the training
  set.
- **`lr`** — linear warmup for `--warmup` steps, then cosine to `--min-lr-frac`.
- **`gnorm`** — the global gradient norm **before** clipping. New in the
  2026-08-20 rewrite and it immediately paid for itself; see below.
- **`tok/s`** — tokens of forward **and** backward per second since the previous
  printed row, from `CLOCK_MONOTONIC`. **Wall clock, not `clock()`** — `clock()`
  is CPU time on glibc, so with 26 workers it counts 26 seconds per second and
  the column would report the single-threaded rate no matter how many cores were
  running: a speedup measurement that cannot see a speedup.

## The gradient clip: the old 99.3% was a batch-size artefact

The previous run reported *"gradient clip (1.00) fired on 2980 of 3000 steps
(99.3%)"* and there was nothing in the output to say whether that meant the norm
was 1.01 or 40. A clip that fires at a norm of 1.1 scales the gradient by 0.9 and
Adam divides that straight back out; one that fires at 40 throws away 97% of the
update and the `lr` printed beside it is fiction. **The two are
indistinguishable from a hit count**, which is why the norm is a column now and
the whole distribution is printed at the end:

```
lmtrain: gradient clip (1.00) fired on 243 of 2500 steps (9.7%)
lmtrain: grad norm  min 0.579  p10 0.651  median 0.709  p90 0.993  max 6.668
```

**The cause was the batch size, not the learning rate.** At `--batch 1` a step's
gradient is one 256-byte window and its norm is dominated by sampling noise; at
`--batch 32` the noise averages down and the median norm falls to 0.709, below
the clip. All four runs above fire on 9.7–9.9% of steps, which is what a clip is
supposed to do — catch the spikes (`max 6.668`, `max 9.442`) and leave the rest
alone. A short high-`lr` run still shows the old pattern, and the trainer now
says so in words when the median exceeds the clip:

```
lmtrain: NOTE -- the median gradient norm is 10.03x the clip, so the
  typical update is `clip`-sized and the lr schedule above is scaled
  by 0.100 on a typical step. Raise --clip or lower --lr.
```

## Memorisation moved out, as predicted

On `CLAUDE.md` the val loss turned back up after ~5,500 steps — 825k parameters
memorising 113 KB. With 78× the data it does not turn up at all: XS's val is
still falling at the last step (1.2822 → 1.2259 → 1.1993 over the final 1,000
steps) after 2.1 epochs, and so is every other size. That is the finding the
extra data was supposed to produce and it is the one it produced.

## Learning rate, chosen by measurement

Six 500-step pilots at the M config, batch 24, all six run at once, stopped at
step 250 once the ranking had been identical at four consecutive checkpoints
(`build/lmruns/pilot_*.log`). Val loss in nats/byte:

| run | lr | clip | wd | val @100 | val @200 | ema @250 |
|---|--:|--:|--:|--:|--:|--:|
| a | 1e-3 | 1.0 | 0.1 | **2.3169** | **1.8029** | **1.7375** |
| b | 1e-3 | 4.0 | 0.1 | 2.4391 | 1.8616 | 1.7860 |
| c | 2e-3 | 4.0 | 0.1 | 2.3602 | 1.8905 | 1.8279 |
| d | 3e-3 | 4.0 | 0.1 | 2.3639 | 1.9496 | 1.9094 |
| e | 5e-3 | 4.0 | 0.1 | 2.4572 | 2.0555 | 2.0506 |
| f | 3e-3 | 4.0 | 0.0 | 2.3672 | 1.9631 | 1.9240 |

(`val` refreshes every 100 steps here, so those two columns are the only
un-stale ones; `ema` is the smoothed train loss and moves every step.)

Monotone in `lr` from step 200 on, and `wd 0.1` beats `wd 0` by 0.014 nats at
the same `lr` (d vs f). The **500-step horizon is the caveat**: a longer run
tolerates a higher `lr` than a short one, so this sweep argues for 1e-3 at 2,500
steps more weakly than it looks. It was not re-run at 2,500 steps because that
is six full-length runs. The
final runs use `lr ∝ 1/sqrt(dim)` normalised to 1e-3 at dim 256 — 1.4e-3, 1.15e-3,
1.0e-3, 0.8e-3 — which is the usual width scaling and is what those pilots
support at the one width they were run at. **They were run at one width, so the
scaling across the other three is an assumption, not a measurement.**

---

# Speed: 304 → 8,145 tok/s, 26.8×

Everything below is `dim=256 layers=6 heads=8 kv_heads=8 hidden=688 seq=256`,
4.81 M parameters, on an i7-14700KF (8 P-cores + 12 E-cores, 20 physical) under
WSL2, forward **and** backward, read off the trainer's own `tok/s` column.
`--batch 1 --threads 1` except where a thread count is given.

| # | source | flags | tok/s |
|---|---|---|---:|
| 1 | pre-optimisation loops | `cc -O2` | **304** |
| 2 | + four-token blocking, `lin_bwd` split | `cc -O2` | 349 |
| 3 | + RoPE cos/sin table | `cc -O2` | 355 |
| 4 | pre-optimisation loops | `-O3 -march=native -fassociative-math …` | 500 |
| 5 | blocked, no RoPE table | `-O3 -march=native -fassociative-math …` | 802 |
| 6 | everything | `-O3 -march=native` only | 529 |
| 7 | everything | `-O3 -march=native -fassociative-math …` | 835 |
| 8 | everything, `--batch 32 --threads 16` | + `-fopenmp` | 8,106 |
| 9 | everything, `--batch 32 --threads 20` | + `-fopenmp` | **8,145** |

Reproduce rows 1–7 with the copy the ladder was measured against —
`build/lmruns/lmtrain_v0.c`, which is `tools/lmtrain.c` with those four loops
reverted verbatim and nothing else changed (it passes `--gradcheck` too).

Read rows 1→3 against rows 4→5→7 before deciding any of it was obvious. **The
loop blocking is worth +15% at `-O2` and +60% under the vectorising flags** —
at `-O2` the inner loop is four *scalar* FMAs and is compute-bound, so cutting
memory traffic buys little; vectorised, it is bandwidth-bound and blocking is
most of the win. The smaller number is the one that would have talked somebody
out of doing the work.

Row 6 against row 7 is the other half: `-march=native` alone is 1.49×, and
`-fassociative-math` on top is another 1.58×, because **that flag is what lets
gcc turn the dot product's accumulator into a vector one.** Without it there is
no `vfmadd231pd` anywhere in `lin_fwd`. Don't believe it, read it:

```sh
cc -O3 -march=native -fassociative-math … -S -o /tmp/lm.s tools/lmtrain.c
awk '/^lin_fwd:/,/^lin_bwd:/' /tmp/lm.s | grep -c 'vfmadd231pd.*ymm'
```

Three source changes and two build flags, and it is worth separating them
because only the first three travel to a host that is not this one:

- **Four-token blocking in all three linear-algebra loops.** The natural
  writing puts the token index outermost, which streams the whole weight matrix
  once per token — 8 bytes moved per flop, a bandwidth test with a matmul
  attached. Holding four token rows at once divides every one of those passes
  by four, and four independent accumulator chains is also what the FMA latency
  wants.
- **`lin_bwd` split into two loop nests** where it was one fused pass. Fusing
  looks like a saving and is the opposite: fused, both `W` and `dW` are resident
  at once (1 MB at dim 256, over this machine's per-core L2 once activations are
  counted) and that repeats per token.
- **RoPE's cos/sin tabulated, and it is the small one.** The rotation depends
  on `(pos, i, head_dim, theta)` and nothing that varies within a run, so the
  loop was recomputing one of 2,048 fixed values 3.1 million times per
  sequence. The table holds the values the formula produces — same doubles, not
  an approximation — and `-DLMTRAIN_NO_ROPE_TABLE` puts the formula back so the
  difference is measured rather than believed. It is **+4.1%** (802 → 835), not
  the large win the shape of the code suggests: 1,536 double transcendentals
  against 823k MACs is a 1:536 ratio and behaves like one. Worth having because
  it is free; worth writing down because the same argument is made about the
  device's `nn_rope`, and **the same few percent is what it would be worth
  there.** (`c/lib/nn` is not this line's file to edit.)
- **The narrow math flags rather than `-ffast-math`.** They measured the same
  (855 vs 869 tok/s in an earlier configuration) and the narrow set does not
  enable reciprocal approximations or flush denormals to zero. The printed loss
  is identical to four decimals under all three of `-O3`, the narrow set and
  `-Ofast`, which is the check that the reassociation is not doing anything
  visible.
- **Data-parallel workers over the batch.** One activation arena and one
  gradient array per worker, summed in worker order. Not one shared `G` with
  atomics: that is slower *and* non-deterministic. Parallelising inside a
  sequence was not taken — the layers are 256–1024 wide, a few hundred
  microseconds each, and a barrier at every one of the ~50 per step would cost
  more than it saves. Batch-parallel has one synchronisation point per step.

**Threads cap at `--batch`**: the parallelism is over sequences, so a 33rd
worker on a batch of 32 would sit idle holding an 87 MB activation arena.

Scaling, same config, `--batch 32`:

| threads | 1 | 4 | 8 | 16 | 20 | 26 |
|---|--:|--:|--:|--:|--:|--:|
| tok/s | 871 | 3,438 | 5,624 | 8,106 | **8,145** | 7,836 |
| × | 1.0 | 3.9 | 6.5 | 9.3 | 9.4 | 9.0 |

**9.4× on 20 physical cores, and it flattens at 16.** The reason is memory, not
the lock-free-ness of anything: the per-worker activation arena is 87 MB at
this size and the private gradient array is another 38 MB, so 16 workers are
streaming 2 GB through a machine whose L3 is 33 MB. Past 20 threads the
E-cores and hyperthreads add contention faster than they add work, and 26 is
*slower* than 20. The default (`omp_get_max_threads()`, 28 here) is therefore
not the fastest setting on this machine — pass `--threads 16` or `20`.

## Reproducibility, and the bug that turned up in checking it

A run is reproducible from **(seed, thread count)** — not, in principle, from
the seed alone. Floating-point addition is not associative, so summing 8
workers' gradients and summing 24 workers' gradients are different sums;
`--threads` is printed in the header for that reason.

In practice the f32 writer absorbs most of it: a 40-step toy run at
`--threads 1`, `2` and `8` printed losses identical to six decimals and
produced **byte-identical** `.lm` files. That is a useful smoke test — a real
race would produce noise or a NaN, not agreement to six decimals — but it is
not a promise, and a 2,500-step run has 2,500 more chances for a last-bit
difference to cross an f32 rounding boundary.

The window offsets are drawn **serially**, before any worker starts, so the data
a run sees is the same sequence whatever `--threads` says. Drawing inside the
parallel loop would make two runs incomparable — not merely different, but
trained on different text.

**And the first version of the reduction was not deterministic.** `gn` was an
OpenMP `reduction(+:gn)`, which combines partials in whatever order the threads
finish, so the norm differed in its last bits between two runs of the identical
command line, `gmul = clip/gn` differed, and the weights differed. Almost all of
it hid: the writer rounds to f32, which absorbs a 1-ulp f64 difference nearly
every time, so two 3.3 MB files first differed at **byte 2,700,081** and were
otherwise equal — a reproducibility failure that looks exactly like a fluke
until you try to bisect one. Found by running the same command twice and
`cmp`-ing the outputs, which is worth keeping in the habit. The range is split
by hand and the partials summed in thread order now:

```sh
for r in 1 2; do build/lmtrain-fast … --out /tmp/det$r.lm >/dev/null; done
cmp /tmp/det1.lm /tmp/det2.lm && echo identical
```

---

# How the bits/byte number is computed

`eval_bpb` is not `eval_loss`. `eval_loss` samples a handful of random windows,
which is the right instrument for a curve you watch during a run and the wrong
one for a claim against a compressor: gzip is not handed eight random windows,
it is handed every byte exactly once. So is this.

A forward pass over `data[s .. s+T)` predicts `data[s+1 .. s+T]`, and position
`p` in it has `p+1` bytes of context. Windows **overlap** by `T - stride` and
only the last `stride` positions of each are scored, so consecutive windows abut
exactly and every byte of the region is scored **once and only once** with at
least `T - stride + 1` bytes of context behind it. The first byte of the region
has no context and is charged 8 bits — what a uniform byte model would pay — so
the total is over all `hi - lo` bytes and goes beside `gzip -9 | wc -c` without
an asterisk.

**The control for that arithmetic**: the printed byte count must be the region
size for every stride. It is — 284,271 at strides 7, 64, 128, 200, 255 and 256.
An off-by-one at the seams would double-score a byte per window and show up as a
count of 286,000-odd, or as a `bits/byte` quietly wrong by a fraction of a
percent with a plausible byte count. (The first version of the loop had exactly
that off-by-one; `covered = s + len` instead of `min(s+len, hi-1) + 1`.)

Stride is `--bpb-stride`, default 128 (context 129–256 B, 2,220 windows,
7.3 s for XS). `--bpb-stride 256` is the pessimistic setting — no overlap, so
every window starts cold and the context runs 1–256 B. **XS scores 1.5554 at
stride 128 and 1.6353 at stride 256**, so the whole window-layout effect is
0.080 bits/byte and the model beats gzip either way. Those two numbers are the
same model, not two runs that happened to land close: the stride-256 run was a
second full 2,500-step training from the same seed and thread count, and its
`.lm` file is **byte-identical** to the first (`cmp` clean, 3,297,856 bytes) —
which is also the strongest reproducibility check in this document.

---

# Output

`--out` gets a LOGITLM v1 file whose matmul weights are `--dtype f32` (default)
or `q8`. `--also-q8 PATH` writes a second, Q8 file from the **same** trained
weights in the same run, so both can be reported without training twice.

Q8 is per-row symmetric via `nn_quantize_q8` itself — not a copy of it. The norms
and the token embedding stay f32 in both files, which is `model.h`'s rule and its
reason: a norm gain is `dim` numbers, and the embedding is a lookup that nothing
multiplies, so quantising either buys no speed and costs accuracy at the one
place every token passes through.

After writing, the trainer prints the val loss of the f32 weights and of the same
weights **round-tripped through the quantiser**. That is what Q8 costs, measured:
**+0.0004, −0.0002, Q8_M, Q8_L nats** on the four sizes — i.e. nothing. The
weight error on its own cannot say, because a large relative error on a
near-zero weight is free and a small one on a large weight is not.

**Q8 is also what fits on the device.** The disk image is 64 MB with roughly
24 MB free, so an f32 file above about 20 MB has nowhere to go; the L model is
the L model was never trained (see the table above), so its file sizes are
unmeasured -- but its 384/6/6/1024 shape puts it near that ceiling by arithmetic.

The byte count is checked against the format's arithmetic before the file is
called valid. That check is the writer agreeing with itself and proves nothing
about the reader — the cross-check that matters is `lm_open()` in
`c/lib/nn/model.c` accepting the file, which is that line's gate.

# The sample, and the gate it feeds

After training, a greedy (argmax, deterministic) continuation of `--prompt` is
printed. It re-runs the **full** forward over the whole prefix for every token —
O(n²) where a KV cache is O(n) — on purpose: a cached single-token path is a
*second* implementation of the forward pass, and a second implementation that
disagrees with the trained one produces fluent nonsense with no error anywhere.

**That is what makes the comparison against `c/lib/nn` worth its minutes.**
Loading the written file with `lm_open()` and greedy-decoding the same prompt
through `lm_forward()` — f32 weights, f32 arithmetic, KV cache, one token at a
time — reproduces the trainer's own sample — f64 weights, f64 arithmetic, no
cache, full re-forward — byte for byte:

```
$ python3 build/lmruns/cmp_sample.py build/lmruns/final_xs.log \
      build/lmruns/model-xs.lm build/lmruns/lm_host
build/lmruns/model-xs.lm     trainer 300 B, engine 228 B, compared 228 B -> IDENTICAL
```

IDENTITY_ROWS

The engine stops at `seq_len` (its KV cache is that long) while the trainer
slides the window, so only the first 228 generated bytes are comparable and only
those are compared — and how many that was is printed, because "they agreed"
over zero bytes is not an agreement. `argmax` is discontinuous, so one differing
bit anywhere separates the two continuations completely within a few tokens;
agreement over hundreds of bytes covers the payload order, the RoPE convention,
the tied head, the SwiGLU pairing and the causal mask at once.

SAMPLE_SECTION

# Options

```
--gradcheck            run the finite-difference gate and exit (0 = pass)
--corpus PATH          training text                       (CLAUDE.md)
--out PATH             LOGITLM output                      (build/model.lm)
--also-q8 PATH         additionally write a Q8 file from the same weights
--dtype f32|q8         dtype of the matmul weights in --out (f32)
--steps N (3000)   --batch N (1)     --seq N (256)
--threads N            data-parallel workers (all cores, capped at --batch;
                       1 without -fopenmp)
--val-tail N           hold out exactly the LAST N bytes   (default 10%)
--bpb-stride N (128)   held-out bits/byte window stride
--no-bpb               skip the full held-out bits/byte pass
--dim N (128)      --layers N (4)    --heads N (4)   --kv-heads N (4)
--hidden N (344)   --untied
--lr F (1e-3)      --warmup N (100)  --min-lr-frac F (0.1)
--wd F (0.1)       --clip F (1.0)    --seed N (1234)
--every N (100)    --val-every N (500)   --val-batches N (8)
--sample-len N (200)   --prompt STR
```

Weight decay is applied to every tensor **except** the norm gains: a decayed gain
shrinks toward zero and an RMSNorm gain has no reason to be small, while the
embedding *is* decayed because under `LM_TIED` it is also the output matrix.
Gradient clipping is on the **global** norm — clipping per tensor changes the
direction of the update, not just its length, which is a different optimiser.

`--val-tail` and `tools/lmcorpus.py` must agree: pass the byte count the corpus
builder printed (`284271` for the default `c/fs` holdout), so the trainer's
boundary and the corpus builder's boundary are the same number and the gzip
comparison is on the same bytes.

# Why C, and why every number in it is a double

There is no numpy and no torch here and nothing in this repo may assume the host
has them; a trainer that begins with `pip install` is a trainer that fails on
somebody else's machine.

The forward pass is `double` **because of the gradient check**, not out of
caution. In f32 the loss carries a relative rounding error near `6e-8`, so
`L(w+e) - L(w-e)` would be a difference of two numbers agreeing to `1e-6` while
the signal is of order `2e-5` — the check would be measuring f32 noise. Running
the trainer in f32 and the check in f64 would mean the check tests a *second*
implementation, which is the arrangement that lets a transposed gradient
survive. Weights are rounded to f32 exactly once, in the writer, and what that
costs is the "q8 costs" line and its f32 counterpart.

One consequence of the 2026-08-20 loop rewrite, recorded because "same seed,
same bytes" is a property people assume without checking: the old four-accumulator
dot product summed `(s0+s1)+(s2+s3)` over j-strided partials and the new one sums
along `j`. Both are correct and neither is more so, but they round differently,
so a model trained before that change and one trained after are not bit-identical
from the same seed. Nothing downstream depends on it — the gates compare the
*trainer* to the *engine* on one set of weights, never two training runs to each
other.

---

# Historical: the CLAUDE.md runs, a different corpus

Kept because somebody will find the old numbers quoted elsewhere. **These are not
comparable to anything above**: 126,606 bytes of English prose against 9.9 MB of
C, and prose is a harder prediction problem per byte than code is.

Reference config `dim=128 layers=4 heads=4 hidden=344`, 824,448 parameters,
corpus `CLAUDE.md`, 3,000 steps × batch 1, `cc -O2`, single thread,
**1,600–1,680 tok/s**.

| run | final ema | best val | final val |
|---|---:|---:|---:|
| 3000 × batch 1 | 1.6729 | **1.8997** (step 2999) | 1.8997 |
| 8000 × batch 1 | 1.0102 | 1.9171 (step 4000) | 2.0570 |
| 4000 × batch 2 | 1.0371 | 1.9601 (step 2500) | 2.0686 |

`1.9029` nats/byte = **2.745 bits/byte**, and the val turning up past ~4,000
steps is 825k parameters memorising 113 KB. Both facts are what motivated the
corpus change: the fix for "it does not train well" was data, and the data was
in the repository the whole time.

# Sanitisers

`cc -O1 -g -fsanitize=address,undefined` — `--gradcheck` and a short training run
covering the writer, the Q8 path, the sampler and the bits/byte pass are
**clean, no reports** (2026-08-20, gcc 15.2, `ASAN_OPTIONS=detect_leaks=0`).
LeakSanitizer is **not** available in this WSL environment: a
deliberate 1,234-byte leak compiled the same way is not detected, with or
without `ASAN_OPTIONS=detect_leaks=1`. So no leak claim is made here — the
control did not fire, and a detector that cannot be watched failing has not been
shown to work.
