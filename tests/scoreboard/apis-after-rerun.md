# Site scoreboard apis-after-rerun

commit 9ba575e93, ISO build-apis/logit.iso (sha256:82ec5e3a8dc5be9e), 4 sites (+0 controls), 2 run(s) each, 210 s wall

ERRORS 2, PAINTED 2

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
| kimi               | ERRORS     |   40.4 |   112 |   32/112 |   1 |      11320 |        5/59 | HTTP 200 |
| stripe             | ERRORS     |   29.6 |    78 |    78/78 |   1 |      20504 |      37/181 | HTTP 200 |
| bilibili           | PAINTED    |    9.1 |    20 |     8/20 |   0 |     269148 |      66/512 | HTTP 200 |
| qq                 | PAINTED    |   13.3 |    16 |     8/16 |   0 |     243856 |     71/1138 | HTTP 200 |

## Detail

### kimi -- ERRORS
url:      https://www.kimi.com/
reported: the one the whole browser arc was aimed at -- an LLM chat page: React, streaming fetch, and a text input that has to accept keystrokes
verdict:  painted 11320 changed px in 40.4s but 1 JS exception(s)
host:     HTTP 200, 483424 bytes, 0.3s, 4 <script> (2 src), 2 <img>
document: 29 stylesheets, 2 script src, 2 inline script, 2 img, 81 preload, 0 font -> 32 mandatory subresources
pixels:   changed 11320 (blank control ink 4256), ink 8624, colours 580, rich-tile proxy 14, bbox [144, 12, 1264, 764]
network:  112 requests, 5 connections dialled, 107 reused, 78 modules (0 failed)
EXCEPTION x1: TypeError: cannot read property 'caller' of undefined
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496672)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496672)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496672)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496672)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496672)
    at <eval> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496672:4)
shot:     tests/scoreboard/apis-after-rerun/kimi.png

### stripe -- ERRORS
url:      https://stripe.com/
reported: 73 scripts, extremely slow
verdict:  painted 20504 changed px in 29.6s but 1 JS exception(s)
host:     HTTP 200, 741414 bytes, 2.4s, 76 <script> (73 src), 30 <img>, redirects: 307 -> https://stripe.com/jp
document: 5 stylesheets, 72 script src, 0 inline script, 30 img, 13 preload, 2 font -> 78 mandatory subresources
pixels:   changed 20504 (blank control ink 4256), ink 10241, colours 698, rich-tile proxy 11, bbox [152, 12, 1264, 764]
network:  78 requests, 3 connections dialled, 76 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 3
EXCEPTION x2: SyntaxError: expecting ';'
    at https://stripe.com/:1
shot:     tests/scoreboard/apis-after-rerun/stripe.png

### bilibili -- PAINTED
url:      https://www.bilibili.com/
reported: TypeError split of undefined
verdict:  painted 269148 changed px in 9.1s, no JS exceptions, no subresource gap
host:     HTTP 200, 117271 bytes, 0.2s, 21 <script> (8 src), 13 <img>
document: 1 stylesheets, 6 script src, 12 inline script, 12 img, 1 preload, 0 font -> 8 mandatory subresources
pixels:   changed 269148 (blank control ink 4256), ink 80064, colours 9883, rich-tile proxy 420, bbox [140, 12, 1264, 764]
network:  20 requests, 8 connections dialled, 12 reused, 0 modules (0 failed)
shot:     tests/scoreboard/apis-after-rerun/bilibili.png

### qq -- PAINTED
url:      https://www.qq.com/
reported: Uncaught (in promise) undefined
verdict:  painted 243856 changed px in 13.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 126000 bytes, 0.2s, 13 <script> (7 src), 17 <img>
document: 1 stylesheets, 6 script src, 6 inline script, 16 img, 0 preload, 0 font -> 8 mandatory subresources
pixels:   changed 243856 (blank control ink 4256), ink 148384, colours 1049, rich-tile proxy 45, bbox [140, 12, 1264, 764]
network:  16 requests, 7 connections dialled, 9 reused, 0 modules (0 failed)
shot:     tests/scoreboard/apis-after-rerun/qq.png
