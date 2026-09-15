# Bfetch must join an existing H2 session before testing new-connection budget

Status: implementation handoff, 2026-09-09. This document changes no transport behavior. `browser_rt.c`, `bfetch.h` and the build files are being edited by another group; coordinate against its settled version before applying the design below.

## Problem and evidence

`req_connect()` in `c/apps/browser/browser_rt.c` currently has this admission order:

1. `hpool_acquire()` tries to borrow an idle HTTP/1.1 connection.
2. `hpool_may_open()` decides whether another connection may be opened; refusal returns with the request still queued.
3. The intended bxfer opener can join an existing HTTP/2 session or open a new socket.

The second step makes the join in the third step unreachable for an origin that already has H2. `hpool_acquire()` deliberately excludes H2 and pending-ALPN connections. `hpool_may_open()` deliberately returns false if `hpool_count_mux()` finds either. Those pool policies are correct individually: an H1 request cannot borrow an H2 socket, and another socket is unnecessary when multiplexing is available. The caller incorrectly treats permission to open a socket as permission to use an existing connection.

The retained guest evidence is `build/site-general/final-sites/bilibili/bilibili.serial.txt`:

- Lines 671–681: 23 DOM images with nonempty src; 20 image items with nonempty src; 17 unanswered images; eight request slots all BF_PENDING/status 0; `g_img_owed=1` and `load_event_pending=1`.
- The eight pending slots target i0/i1 origins. JavaScript `.avg_color` fetches also occupy those origins and later time out.
- Lines 448 and 512 show three images actually decoded; nearby i2 transport completions distinguish this from an entirely inactive image pump.
- BF_PENDING combines RQ_QUEUED, RQ_DIAL and RQ_XFER. This snapshot does **not** prove the raw state of each of the eight requests. The admission-order defect is source-proven; its contribution to each captured request still needs the network diagnostic below.

The production pool policy witness at `build/site-general/runtime/h2-admission-witness.c` printed:

```text
existing_h2_slot=0 h1_acquire=-1 may_open=0 mux_acquire=0
```

It links the actual hpool implementation: joining is possible exactly when the current caller returns. This is a policy witness, not a completed-request or guest fix test.

The integrated host gate also has a real unresolved H2 failure. `build/site-general/final-gates.log` reports `test-cache-invalidation: 652 checks, 35 failures` (the aggregate exited 2); the failed assertions include BF_DONE, status 200, the expected body at `https://h2.example/target`, request counts, and invalidation timing. The parent confirmed that this run's source snapshot and linked binary included TEMP-B. After the external group restored `bxfer_open_ex`, `build/site-general/cache-after-external-restore.log` still reports **652 checks, 35 failures**, including the same H2 status/body path. These failures must be fixed or explicitly left unresolved; they are not an unrelated gate failure and cannot be attributed solely to TEMP-B. The fourteen passing local HTTP guest fixtures do not establish H2 correctness. These observations also do not, by themselves, prove that admission ordering accounts for all 35 failed assertions.

## Specific production correction

Separate **join-only admission** from **fresh connection admission**. A minimal bounded repair should initially join only an already negotiated, usable H2 session. It can leave requests queued during another session's ALPN negotiation; joining speculative pending sessions also requires the H1 fallback work described below.

Add an internal bxfer join-only operation, or an explicit mode of a common admission operation. Its contract must be:

- It never calls `sock_open` and never falls through to a fresh dial.
- Match the actual URL origin: host, port and TLS. Do not share merely because DNS returns the same address.
- Reject sessions with socket error/EOF, unusable H2 state, GOAWAY, or exhausted allowed stream capacity. Admission must honor both pool accounting and the actual H2 peer/local stream limits.
- If a session has a pool slot, a refused mux admission is not success. The current `bxfer_open_ex` ignores the return from `hpool_acquire_mux`; copying that behavior would let a join-only repair silently bypass stream limits. Ensure the reserved pool slot belongs to the session being returned.
- Increment the session reference and pool reservation exactly once on success. Failed admission changes neither. Return a temporary “no admissible session” result without starting I/O.
- Paired completion, cancellation and failure release one stream reservation and one session reference. They must leave other streams and the shared socket alive.

Then order `req_connect()` as follows:

```text
try existing idle H1 (preserve its stale-socket check)
if acquired: begin the exchange using the existing H1 ownership path

try existing usable H2 through JOIN_ONLY
if joined:
    record bxfer ownership, bx_fresh=0, and reuse rather than a fresh dial
    begin the exchange, or use the existing connected-state transition
    preserve this request's t_start and t0
    return

if fresh connection budget is unavailable:
    remain RQ_QUEUED; preserve timestamps
    return

attempt a fresh admitted connection
if temporarily capacity-blocked:
    remain RQ_QUEUED; preserve timestamps
else if a real open failure occurred:
    report that failure through the existing failure path
else:
    record bxfer ownership, bx_fresh=1, fresh-dial statistics and RQ_DIAL
```

Treat this as an admission transaction. A new socket must receive the required pool reservation, or be closed and rolled back. Keep the existing ownership distinction for H1 yield versus shared H2 release. On failed join/start, do not leave a reference or reservation attached to a request that remains queued.

**Do not simply remove the `hpool_may_open` test or move an unrestricted `bxfer_open_ex` call before it.** The current opener may dial when its join attempt fails, and it currently tolerates failed pool admission after opening. That would turn a starvation correction into an unaccounted connection-cap bypass. Either implement JOIN_ONLY plus a separately budgeted fresh path, or make the shared admission API itself enforce those outcomes explicitly for every caller.

If joining pending-ALPN sessions is included, account for the case where ALPN selects H1. `bxfer_start()` currently opens a replacement connection when `ss->h1_taken` is already true. This replacement is a **fresh dial** and must acquire fresh budget too. When no budget is available, unwind the speculative join and retain a queued request with its original deadline; do not interleave two H1 requests, exceed the cap, or reset the wait clock. Leaving pending sessions unjoined is an acceptable smaller first correction.

## Timeout and starvation audit of the current source

Read-only inspection of the current source found these distinct bounds:

| Waiting population | Current behavior | Bound |
| --- | --- | --- |
| A bfetch request already allocated a slot, continuously RQ_QUEUED | `t0` and `t_start` are set before initial admission. `bfetch_pump()` tests `req_expired()` before retrying `req_connect()`. A rejected queue attempt does not reset either timestamp. | It fails on the first pump where age is **greater than 60,000 ms**. It is not an indefinitely pending allocated request under a continuing pump. |
| An allocated request across redirects/retry | Redirects and the single fresh retry can reset `t0`, but not `t_start`. | First pump beyond the 90,000 ms total lifetime fails it; the 60,000 ms attempt bound may fire sooner. |
| A request that entered RQ_XFER and has received no bytes | The first-byte check is inside the transfer path. | 12,000 ms without response bytes, subject to the next pump; this check does not apply while RQ_QUEUED. |
| An image that has not obtained one of the eight image slots, or for which `bfetch_start()` cannot allocate a request slot | `image_fetch_ready()` returns pending for a syntactically valid URL without storing a start time for that waiting image. | **No per-image starvation deadline before admission.** Sustained competing work or continuing DOM churn can keep such an image pending indefinitely. The allocated-request deadlines do not cover it. |

Timeout checks are cooperative. `g_img_owed` limits the event-loop sleep to `BROWSER_PUMP_MS=10`, and `settle_frame()` calls the image path, which pumps bfetch. This establishes an intended wake path, not an unconditional real-time upper bound: a blocked event loop or a caller that stops pumping cannot execute its timeout checks. A request can therefore be observed overdue until the next pump. The eight captured slots have actual request IDs, so the **allocated** bound applies to them; do not describe them as having no deadline merely because the snapshot shows pending.

There is also no fairness guarantee that every queued request succeeds. The current admission defect can make a request wait until it **fails** at its deadline while a usable H2 connection was available throughout. Replacing starvation with timeout is a bound on lifetime, not correct service.

The repair should preserve these existing budgets. Do not raise them to make the affected page pass. A separate pre-admission image-lifetime policy would need explicit ownership, cancellation and tests; it is not necessary to conceal or inflate as part of the H2 join repair.

## Required controls and production-consumer tests

Use the real browser resource-fetch consumer, bxfer adapter and pool with a deterministic local H2 peer or the existing protocol harness. Merely calling `hpool_acquire_mux()` directly does not test the failed caller boundary. Do not omit js_urlbind/other production TUs and infer unrelated web APIs are absent.

1. **Old-order negative control:** retain the current early fresh-dial gate under a compile control. Hold a JavaScript fetch stream open on an H2 session; then load an image/script through bfetch from the same origin. The controlled build must leave the bfetch request queued while the peer is ready to answer its new stream. The corrected build must submit and complete it on the same socket before the held stream finishes. Assert request body/decoded image or executed script, one TCP connection, distinct H2 stream IDs, and a live original stream. Make the watched red control a prerequisite of the positive gate.
2. **Connection-cap control:** occupy the global and per-origin connection budget with busy H1 connections and provide no joinable H2 session. The corrected path must remain queued and emit no extra socket open/SYN. A control that changes JOIN_ONLY to the unrestricted opener must visibly violate the cap and fail the test. Repeat with a different origin to catch accidental cross-origin reuse.
3. **Mux-capacity control:** fill the negotiated/local allowed H2 stream capacity. A further request must remain queued without increasing references, pool streams or active protocol streams. Free one stream and show exactly one waiting request starts. A control that ignores mux admission failure must go red.
4. **Ownership and failure:** complete/cancel a joined bfetch request while another stream remains active. Check the surviving stream's actual response and socket, as well as balanced references/reservations. Exercise unusable/GOAWAY sessions and failed stream creation. No double-close, leaked reservation or retry on an already submitted unsafe request is permitted.
5. **Queue deadline control:** freeze connection capacity, inject monotonic time, and keep pumping. At 60,000 ms a fresh continuously queued request still satisfies the current strict `>` boundary; at 60,001 ms it must fail. A control skipping queued-request expiry must visibly remain pending and fail the assertion. Verify the 90-second total bound survives redirects/retry, and releasing capacity before expiry lets the request complete without resetting its original lifetime.
6. **Pending ALPN/H1 fallback, if implemented:** join during negotiation, select H1, exhaust fresh budget, and verify the second request waits without a new socket or interleaved request bytes. Then release capacity and complete it. If the initial fix joins negotiated H2 only, instead assert pending-ALPN requests wait until protocol resolution.

For guest acceptance, use an ordinary page whose initial script keeps a same-origin fetch active while its visible image is loaded through the normal page pipeline. Show the image pixels and the fetch's actual body, then repeat the Bilibili specimen. Record guest timing and current about:images state. Do not count the three inline/vector decode categories as successful thumbnail loads.

## Demand-only network evidence to add in the owning patch

Extend the existing about:images request dump, without per-frame logging, to print for each of the eight slots:

- raw RQ state, requested URL, fd, bx/bx_fresh and pool slot;
- current monotonic time, t_start/t0/t_xfer and derived ages;
- origin H1/H2/pending-ALPN counts and available stream capacity;
- session identity/refcount, current protocol, active stream IDs and any error/GOAWAY state;
- the last admission reason (joined / fresh budget blocked / mux full / no usable session), stored only when needed rather than reconstructed by calling mutating admission functions during the dump.

A diagnostic must not call `hpool_acquire_mux` or another mutating operation just to ask whether a join is possible. It would consume the capacity being measured.

## Shared-workspace warning retained beside the intended fix

During the initial audit another group's `browser_rt.c:req_connect` contained a `/* TEMP-B */` direct `sock_open(... SOCK_F_ALPN_HTTP11)` experiment while still assigning `r->bx=1` and `bx_fresh=0`. That is not the intended bxfer admission path described by the surrounding comment. It was still present at the initial read-only review.

Correction retained beside that observation: the parent reported restoration at 21:13:05 on 2026-09-09, and this handoff's latest source inspection confirms `bxfer_open_ex` is restored, with `hpool_may_open` still before it. The parent reports that the 21:06 disk still contains TEMP-B; no subsequent disk build was performed by this task. Thus current source, the older disk, and each gate log represent distinct states. The H2 gate failed in both tested source states as recorded above. Preserve the external group's edits, coordinate its settled handoff, and recheck the actual compiled source before applying the admission repair or attributing guest behavior.

This handoff performed no build and edited no transport/build files. Supporting investigation: `build/site-general/runtime/bilibili-images-audit.md`.
