# Parent result callback to fetch transport handoff

The local finite fixture confirmed a scheduling gap: a parent Worker-result
callback could create a fetch, but another Worker's next native call began
before the new request's first transport write. The updated page entry now
services the ready request before that next Worker call. This result concerns
ordinary event-loop scheduling; it does not establish the cause of a site's
validation response or successful chat completion.

## Test boundary

`tests/unit/worker_parent_fetch_test.c` drives the real `js_page_run_due` entry.
Two dedicated Workers run ordinary native addition and return 42. Each addition
advances an injected monotonic clock by 20 ms and returns normally. Worker 1's
parent result handler creates one same-origin fetch for fixed local text. A
second case creates the same fetch in that handler's Promise microtask.

`worker_parent_fetch_net.c` uses the existing in-process socket fixture and real
HTTP parser. Its network clock shares the page's injected clock. The transport is
ready immediately; the observer records its first successful `send`, separately
from socket opening and fetch creation. The fixture does not wait on a real
clock, connect to a host, or read site input, credentials or authentication data.
It contains no unbounded loop, GC or transport failure scenario. Every request finishes
with the expected ordinary text and both additions finish with 42.

The test allows a return to the outer event loop between parent delivery and
Worker 2. It fixes the observable ordering, not a private scheduler API or an
assumption that a remote handshake must finish synchronously.

## Observed final verification

Build directory: `build-parent-fetch-order`.

| Variant | Checks | Failures | Observed event order |
| --- | ---: | ---: | --- |
| Saved pre-handoff production units | 18 | 2 | Add 1, create fetch, add 2, first send |
| `JS_TASK_NO_PARENT_FETCH_HANDOFF` | 18 | 2 | Same precise old order |
| Current | 18 | 0 | Add 1, create fetch, first send, add 2 |
| Current, ASan/UBSan | 18 | 0 | Same corrected order |

Only the two ordering assertions fail in the old/control variants. Request
count, completed response, arithmetic results, and normal termination assertions
continue to pass. `worker_parent_fetch_check.py` checks every failed assertion,
the exact check count and both event sequences, and rejects unrelated JS errors.

The baseline uses saved `js_worker.c`, `js_page.c` and `js_webapi.c` with the same
local fixture and other current dependencies. It is not an old whole-browser
image. The first apparatus run had an unset fake-loader base for a relative
Worker URL; its log is preserved as `before-apparatus.log` and is not counted as
product evidence. The final fixture uses an explicit synthetic script URL in
both variants.

The existing finite fairness gate was updated to retain a strict sequence at
the page entry: first Worker task, parent-result handoff with no second native
call, then the next page turn's second Worker task. This added one assertion;
the gate is now **60/60**, with exact control matrices **22**, **6**, and **2**
failures for the old complete batch, hidden-job wake observation, and absent
parent sweep respectively. The old-complete-batch control disables both the turn
budget and the separate new parent-fetch handoff so the latter cannot conceal
the old scheduling behavior. No assertion was replaced by an unbounded
eventual-completion check.

```sh
make BUILD=build-parent-fetch-order test-worker-parent-fetch-san test-worker-fairness -j4
make BUILD=build-parent-fetch-order test-mk-wired
```

Evidence:

- `final-gates.log`, `worker-parent-fetch/{current,control,san}.log`
- `worker-fairness/{current,unbounded,hidden-jobs,no-parent-sweep}.log`
- `before.log`, `before/`, `probe.mk`: saved-source baseline
- `mk-wired.log`: 296 fragments, 295 reachable and one documented standalone wrapper
- `verified-hashes.json`: source and executable identity

Sanitizers ran the same finite fixture and all 18 checks with leak detection disabled and
undefined-behavior errors configured to stop. The parent-fetch gate requires its
precise negative control and is reachable from `ci-host`. This fixture does not
test the separate first-pump timeout compensation or preemption within a long
synchronous computation.
