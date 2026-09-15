# Site scoreboard complete-0913-capacity

commit 41d7a74e8, ISO build-browser-complete-0913/capacity/logit.iso (sha256:8cb72e1276ff6065), 2 sites (+0 controls), 1 run(s) each, 136 s wall

BLANK 1, ERRORS 1

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
| qwen               | BLANK      |   40.5 |    24 |     6/24 |   0 |       2768 |      26/103 | HTTP 200 |
| apple              | ERRORS     |   12.1 |    28 |    22/28 |   0 |     516388 |      33/150 | HTTP 200 |

## Detail

### qwen -- BLANK
url:      https://chat.qwen.ai/
reported: hangs
verdict:  loaded in 40.5s and painted 2768 changed pixels (0 exceptions)
host:     HTTP 200, 101120 bytes, 0.3s, 17 <script> (3 src), 6 <img>
document: 2 stylesheets, 3 script src, 14 inline script, 6 img, 7 preload, 0 font -> 6 mandatory subresources
pixels:   changed 2768 (blank control ink 4256), ink 4698, colours 430, rich-tile proxy 8, bbox [152, 12, 1264, 764]
network:  24 requests, 6 connections dialled, 18 reused, 16 modules (0 failed)
sub-resource failures (1):
    [img] fetch failed (status 0): http://127.0.0.1:31460/favicon.png: the connection was refused or the network is down
shot:     build-browser-complete-0913/evidence/live-capacity/qwen.png

### apple -- ERRORS
url:      https://www.apple.com/
reported: 0 images; TypeError split of undefined
verdict:  painted 516388 changed px, with 5 resource failure diagnostics
host:     HTTP 200, 312779 bytes, 0.1s, 17 <script> (12 src), 100 <img>
document: 9 stylesheets, 12 script src, 1 inline script, 43 img, 0 preload, 0 font -> 22 mandatory subresources
pixels:   changed 516388 (blank control ink 4256), ink 416674, colours 697, rich-tile proxy 12, bbox [140, 16, 1260, 764]
network:  28 requests, 2 connections dialled, 26 reused, 6 modules (0 failed)
sub-resource failures (1):
    [browser] fetch failed (status 404) /wss/fonts?families=SF+Pro,v3|SF+Pro+Icons,v3: no error
non-executable <script> blocks skipped: 4
shot:     build-browser-complete-0913/evidence/live-capacity/apple.png
