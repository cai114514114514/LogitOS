# Browser Wasm reader code generation — 2026-09-11

The browser's normal Wasm interpreter repeatedly decodes integer immediates.
This change asks the compiler to inline four existing reader/helper definitions
only when `js_wasm.c` includes the decoder into the browser translation unit.
The reader bodies, validation, instruction-boundary map, interpreter, and Wasm
JavaScript object lifecycle are unchanged. This is a general code-generation
choice; it has no website or algorithm condition.

## What the audit established

`js_wasm.c` already includes `wasm_parse.c`, `wasm_valid.c`, and `wasm_exec.c`
in one translation unit. The ordinary separate-TU host gate therefore is not
an exact model of browser code generation. The earlier read-only fixture in
`build-wasm-interpreter-audit/` passed 42 arithmetic checks and reported roughly
44.925 ms for one million sum iterations, but that separate-TU host result is
neither a measurement of this patch nor guest evidence.

In an x86 `-O2` unity compile using the browser's freestanding flags, the opcode
byte reader was already inline. The emitted `call_any` body still had eleven
call sites for `wasm_rd_u32` and one for `wasm_rd_s32`. Inlining only those
wrappers moved the calls down to `leb_u`/`leb_s` and regressed the measured
finite workloads. Inlining all four definitions preserved their complete
source bodies and improved the host comparison.

`run_body` is compiled into `call_any` in this build. Samples landing inside
`call_any` do not, by themselves, imply frequent Wasm function calls. The
control map and instruction-boundary bitmap are built during instantiation;
integer immediates are read again during execution. This patch does not add
preemption to a single synchronous native Wasm invocation.

## Reproducible bounded evidence

All modules in this comparison are self-authored valid finite integer sum or
identity functions. They have no imports, memory, website data, or network
requests. Only normal arithmetic execution was used.

The frozen comparison is in `build-wasm-finite-perf/`. Its
`arithmetic_bench.c` uses `CLOCK_PROCESS_CPUTIME_ID`, excluding time spent
descheduled by other local tasks. Each process checks an untimed warmup and
five runs at 1K, 10K, 100K and 1M iterations. Five processes per variant run in
alternating order. Below are medians of the five per-process 1M medians:

| Variant | Sum CPU ms | Sum + identity CPU ms | x86 unity text bytes |
| --- | ---: | ---: | ---: |
| Existing unity codegen | 42.018 | 45.674 | 50,016 |
| Temporary checked single-byte shortcut | 34.770 | 39.957 | 50,592 |
| Inline two public reader wrappers | 44.727 | 49.047 | 50,368 |
| Inline both wrappers and both helpers | 23.925 | 31.793 | 51,424 |

The selected variant reduces these host CPU times by 43.1% and 30.4% and adds
1,408 x86 text bytes (2.8%). Host architecture is arm64; x86 size comes from a
separate cross-compile. These numbers cannot establish guest latency or an
improvement on a live website. The single-byte shortcut remains an unshipped
temporary comparison; the selected patch adds no alternative decoder logic.

`run_comparison.py`, `cpu-comparison.json`, per-run CSV/logs,
`x86-code-size.json`, `compiler.txt`, and source hashes retain the apparatus.
`unchanged-body-check.json` records that removing the attribute macro/comment
additions restores the original parser source exactly. Removing only the new
include-block lines restores the prior verified `js_wasm.c` hash
`6c3216349bd69f0baee7ce8dc2bd9711d2e748c5992a7e494a47cdfe65016ab7`.
The production macro cross-compile reproduces the selected 51,424-byte text
size, versus 50,016 bytes with the macro absent.

## Narrow acceptance and guest handoff

`make BUILD=build-wasm-finite-perf test-wasm-finite-arithmetic` builds the same
normal fixture with the macro off and on. Each passes 36 valid modules and
216 result checks: local indexes 1/2 and 128/129, positive/negative one-byte and
multi-byte signed constants, optional identity calls, and finite iteration
counts from zero through 1,000. The baseline intentionally remains green;
this optimization does not change arithmetic behavior. The target has no
lifecycle, GC, fault-injection, or network dependency. `test-mk-wired` passes
with the new fragment reachable.

For independent guest timing, `build-wasm-finite-perf/sum.wasm` is a 68-byte
module exporting `sum(i32) -> i32`, computing `0 + ... + (n - 1)` modulo
2^32. `sum(1000000)` returns unsigned `1783293664`. Its SHA-256 is
`4559d69493ab4310bbb008542ef23ca2b86d2f889c2e3a6190c386127ff1b389`.
`sum-module.json` includes the exact byte array and seven normal inputs with
their expected results. At the initial handoff, guest before/after measurement
and the genuine first rendered service answer remained work owned by the
parent task; the guest timing follow-up is recorded below.

## Guest timing follow-up

The parent task ran an isolated localhost fixture in the real guest in the
order old, current, old again. A subsequent read-only audit recomputed the
medians from `finite-wasm-guest-{old,current,old-repeat}/results.json` under
`build-ds-first-answer/` and compared them with
`finite-wasm-guest-comparison.json`. Each run used the same fixture, worker
source, and 68-byte module hash recorded above. Two workers each made three
ordinary `sum(1000000)` calls, timing each call with guest `Date.now()`.

| Browser snapshot | Six guest elapsed samples (ms) | Median (ms) |
| --- | --- | ---: |
| `snapshot-fetch`, first run | 790, 790, 800, 790, 780, 780 | 790 |
| `snapshot-turn-preview` | 470, 460, 470, 520, 530, 530 | 495 |
| `snapshot-fetch`, repeated | 710, 740, 720, 740, 710, 710 | 715 |

The current snapshot's median is 37.3% below the first old run and 30.8% below
the repeated old run. This establishes lower guest elapsed time for this
bounded arithmetic workload across these snapshots. The current snapshot
also includes the Worker turn-budget changes, so this is a snapshot comparison
rather than a claim that the guest experiment varied only compiler attributes.

The old browser SHA-256 is
`1431b7434af7b8b0f51ca466e57e568706d83a2077cc8dd7aeab5bad22189b32`;
the current browser SHA-256 is
`46ca7ce8b47ddaa1c94dadbba5381226db693e78b4c9d5ac8922ad7c53a22554`.
All three reports pass their intended gate, stop their test guest, and report
no forced result. The audit also hashed the source disk and ISO files again:
they still match each report's recorded inputs. The old runs intentionally
retain a red scheduling-order check, which their old-behavior gate expects;
the current run has all three rendered checks green. The input event in this
fixture is synthetic, so this does not measure physical keyboard latency.

**Result-count correction:** the historical wrapper field
`six_results_and_positive_timings` overstates the result evidence. Its worker
overwrites `wasmResult` on each of its three calls and reports only the final
value. Each run therefore records **six positive timings and two correct final
worker results** (`1783293664`), not six separately checked Wasm results. The
original JSON evidence is preserved with that historical field name; this
report uses the narrower claim established by the wrapper and records.

A single synchronous native Wasm invocation remains non-preemptible. These
local guest results do not establish a successful first response from the
live service or resolve its separate validation failure.
