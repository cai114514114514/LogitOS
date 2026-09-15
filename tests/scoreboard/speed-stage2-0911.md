# Site scoreboard speed-stage2-0911

commit 41d7a74e8, ISO build-browser-speed-0911/baseline/logit.iso (sha256:89466926e7e34e11), 2 sites (+1 controls), 1 run(s) each, 156 s wall

ERRORS 1, PAINTED 1

WHAT THIS TABLE IS NOT. `changed px` counts pixels that differ from an empty tab
photographed in the same boot. It cannot tell a rendered page from a flat dark
block: bing scores 625,312 changed pixels and is exactly the "一坨黑黑的" that was
reported by hand. Nothing here checks whether the RIGHT pixels changed -- that is
what reftests are for, and none of WPT's 17,155 of them run on this machine. The
top verdict is `PAINTED`, which means pixels changed, no script threw, and the
guest asked for everything the document requires. It does not mean correct.

The two `control-` rows are NOT results. They exist to prove the harness, the
network and the build were working during the pass; if either fails, no other row
in the snapshot means anything. They say nothing about the browser.

| site               | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| python             | ERRORS     |   11.5 |    13 |  15/13 ! |   0 |     619672 |     141/594 | HTTP 200 |
| deepseek           | PAINTED    |    2.5 |    19 |    19/19 |   0 |     326268 |      42/245 | HTTP 200 |

CONTROLS -- harness health, not results:
| control            | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| control-wikipedia  | PAINTED    |    7.3 |     5 |      4/5 |   0 |      46788 |    186/1040 | HTTP 200 |

## Detail

### python -- ERRORS
url:      https://www.python.org/
reported: (new 2026-08-28: user reports it does not open)
verdict:  painted 619672 changed px, with 1 resource failure diagnostics -- and the document asked for 15 subresources (4 stylesheets, 10 script srcs) against 13 requests issued, short by 2
host:     HTTP 200, 53119 bytes, 1.4s, 13 <script> (10 src), 1 <img>
document: 4 stylesheets, 10 script src, 2 inline script, 1 img, 2 preload, 0 font -> 15 mandatory subresources
GAP:      the guest issued 13 requests against 15 mandatory -- SHORT BY 2. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 619672 (blank control ink 4119), ink 495078, colours 1735, rich-tile proxy 41, bbox [112, 16, 1256, 736]
network:  13 requests, 5 connections dialled, 8 reused, 0 modules (0 failed)
sub-resource failures (1):
    [browser] fetch failed (status 0) https://media.ethicalads.io/media/client/v1.4.0/ethicalads.min.js: TLS refused: handshake or certificate verification failed
non-executable <script> blocks skipped: 1
shot:     build-browser-speed-0911/evidence/live-stage2/python.png

### control-wikipedia -- PAINTED
url:      https://en.wikipedia.org/wiki/Operating_system
reported: CONTROL: known to render since M13
verdict:  painted 46788 changed px in 7.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 676052 bytes, 3.4s, 5 <script> (1 src), 26 <img>
document: 2 stylesheets, 1 script src, 3 inline script, 24 img, 2 preload, 0 font -> 4 mandatory subresources
pixels:   changed 46788 (blank control ink 4119), ink 16022, colours 1247, rich-tile proxy 25, bbox [124, 12, 1264, 736]
network:  5 requests, 2 connections dialled, 3 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 1
shot:     build-browser-speed-0911/evidence/live-stage2/control-wikipedia.png

### deepseek -- PAINTED
url:      https://www.deepseek.com/
reported: blank; needs Worker
verdict:  painted 326268 changed px in 2.5s, no JS exceptions, no subresource gap
host:     HTTP 200, 90207 bytes, 0.1s, 37 <script> (16 src), 3 <img>
document: 3 stylesheets, 15 script src, 21 inline script, 3 img, 3 preload, 0 font -> 19 mandatory subresources
pixels:   changed 326268 (blank control ink 4119), ink 24523, colours 1489, rich-tile proxy 53, bbox [112, 12, 1264, 736]
network:  19 requests, 0 connections dialled, 1 reused, 0 modules (0 failed)
shot:     build-browser-speed-0911/evidence/live-stage2/deepseek.png
