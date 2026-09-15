# LogitOS R9 responsiveness result

This report binds the completed responsiveness work to the frozen artifacts in
`release-tested/`.  Run `python3 verify.py` from this directory to recheck the
hashes and the categorical claims below.

## Browser loading

The fixed, host-local fixtures were each run three times inside one QEMU TCG
guest.  All 15 current samples are complete.

| fixture | baseline median | R9 median | change |
| --- | ---: | ---: | ---: |
| lean | 1660 ms | 560 ms | -66.27% |
| stylesheet | 2040 ms | 750 ms | -63.24% |
| script | 2220 ms | 730 ms | -67.12% |
| DeepSeek-shaped | 7190 ms | 750 ms | -89.57% |
| Wikipedia-shaped | 19520 ms | 5110 ms | -73.82% |

The interleaved-log negative capture contains a malformed sample and is kept in
`evidence/browser-invalid-interleaved/`; the verifier requires that corruption
to remain rejected.

## Interaction and animation

Closing the browser while a stylesheet response was held completed within a
368.6 ms host-side bound.  Closing during busy JavaScript completed within
490.4 ms.  While stylesheet, initial script, and image responses were still
held, wheel input visibly moved the page by 120, 120, and 160 pixels, all within
about 162 ms host-side bounds.

The deterministic 48-frame animation guest fixture fell from 3520 to 1340 ms;
its maximum frame gap fell from 320 to 40 ms.  Input count, final geometry, and
layout-build count remained equal.

## Network and repeat visits

The historical seven-pair local-fixture network run moved from a 590 ms median
to 519 ms, with a paired median change of -92 ms and six of seven pairs faster.
The corresponding whole-guest median moved from 600 to 530 ms.  This is a
source-hash-bound historical A/B result, not a fresh public-Internet result.

On the fresh frozen R9 artifacts, the heavy-page repeat-visit run measured 3180
ms first visit and 2430 ms second visit.  The second visit made 14 requests,
opened zero connections, and recorded 14 cache hits.

## Artifacts and proof boundary

| artifact | SHA-256 |
| --- | --- |
| `logit.iso` | `aa35f02fac7f5a93b13868398201c37a82519dc48b75e37a8e6f52c6e1217810` |
| `disk.img` | `a3dd855fe6713b07e0f59788bf57fd11d68da0e54cdfc09597f82ac3b184bdf3` |
| `browser.aex` | `04e9f18dd8cebd4b3da58afc41e6cae30f1ec46fcfca65676397a5af4a9188a0` |

These results prove deterministic QEMU TCG behavior against local fixtures.
They do not establish physical-machine frame pacing, GTX 1050 acceleration, or
public-Internet latency.  The current worktree contains later parallel source
changes; the files above are the byte-stable tested R9 release set.
