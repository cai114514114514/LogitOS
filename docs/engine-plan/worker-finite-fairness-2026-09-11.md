# Finite Worker turn fairness

The permanent host gate now verifies that completed Worker tasks yield to the
parent event loop when the shared turn budget is spent. It also verifies that
deferred work stays discoverable and resumes in order. This is cooperative
scheduling: one synchronous JavaScript or native call still runs until it returns.
The test does not establish an 8 ms upper bound on such a call or a real-site
completion result.

## Fixture and acceptance

`tests/unit/worker_fairness_test.c` uses the real page and Worker schedulers,
separate Worker runtimes, and their exported pending/due interfaces. A dedicated
host wrapper installs an ordinary native `fixtureAdd(20, 22, label)` in each
Worker. The function returns 42 and, for selected labels, advances the injected
monotonic clock by 20 ms. There is no timed host spin, infinite loop, fault
injection, native interruption, or scheduler imitation.

The 59 assertions cover:

- Ready and startup-buffered messages: the first addition completes, parent
  message and input handlers can run, and the next pump completes the second
  addition with the intervening parent event visible.
- The same behavior through `js_page_run_due`, checking the shared page entry.
- A completed HTTP fetch with no remaining socket: deferred Promise jobs remain
  pending and immediately due, retain their results, and precede that Worker's
  next incoming message task.
- Two fetch owners: one reaction consumes the first turn, and its already-due
  parent result is delivered before the other owner's next reaction.
- Normal terminate and page close after a verified budget boundary, followed by
  a fresh Worker which completes arithmetic and receives only its own results.

HTTP response bytes use the existing in-memory socket vtable and real HTTP
parser. Script loading uses the local synthetic script loader. Parent text input
is DOM mutation plus synthetic event dispatch, not an OS keyboard or screenshot
test. No VM, public endpoint, account, site script, or challenge algorithm is used.

## Observed results

These are the initial turn-budget results. The later parent-callback fetch
handoff adds one strict page-entry assertion, producing 60 checks and updated
negative matrices. See [the follow-up report](worker-parent-fetch-handoff-2026-09-11.md)
for that final validation. The initial build logs below remain preserved.

Build directory: `build-worker-fairness-fixture`.

| Variant | Checks | Failures | Gate result |
| --- | ---: | ---: | --- |
| Current scheduler | 59 | 0 | Pass |
| Current, ASan/UBSan | 59 | 0 | Pass on the same finite inputs |
| Actual saved pre-budget `js_worker.c` | 59 | 21 | Exact old failure matrix |
| `JS_TASK_UNBOUNDED_TURN` | 59 | 21 | Exact expected matrix |
| `JS_TASK_HIDE_PENDING_JOBS` | 59 | 6 | Exact expected wake failures |
| `JS_TASK_NO_PARENT_SWEEP` | 59 | 1 | Exact expected parent-order failure |

The old-source probe compiles the saved Worker source with the current fixture
and other current dependencies; the page's optional interface falls back to the
old `js_worker_run_due` entry. It is not represented as an old whole-browser
image. Its 21 failures match the permanent unbounded control assertion for
assertion. The control checker compares the full multiset of failed assertion
names and the total check count, and rejects unrelated JavaScript exceptions.

Initial validation found a one-turn delay resuming startup-buffered messages.
After that fix, the added page-entry case found an analogous empty phase-tail
turn. Both were corrected in production before the final 59-check pass; the
assertions were retained. Initial and intermediate logs remain in the build
directory.

Commands and evidence:

```sh
make BUILD=build-worker-fairness-fixture test-worker-fairness -j4
make BUILD=build-worker-fairness-fixture test-mk-wired
```

- `final-gates.log` and `worker-fairness/{current,unbounded,hidden-jobs,no-parent-sweep}.log`
- `old-probe.mk`, `old/js_worker.c`, and `worker-fairness/old-source.log`
- `verified-hashes.json`: tested source and executable hashes
- `mk-wired.log`: 293 fragments, 292 reachable, one documented standalone wrapper
- `san-probe.mk` and `worker-fairness/san.log`: AddressSanitizer and
  UndefinedBehaviorSanitizer on exactly the same 59 checks, with leak detection
  disabled and undefined-behavior errors configured to stop the run. No extra
  fault, GC, or invalid-memory scenario was introduced.

The positive target requires all three permanent negative controls and is wired
into `ci-host`. This report covers finite host behavior only; it does not claim
parallel execution, preemption inside a long native computation, or successful
DS chat completion.
