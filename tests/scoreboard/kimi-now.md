# Site scoreboard kimi-now

commit d6d6a0ae5, ISO build/logit.iso (sha256:9da63eee1b542124), 1 sites (+0 controls), 1 run(s) each, 78 s wall

ERRORS 1

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
| kimi               | ERRORS     |   35.1 |   112 |   32/112 |   1 |      11536 |        5/59 | HTTP 200 |

## Detail

### kimi -- ERRORS
url:      https://www.kimi.com/
reported: the one the whole browser arc was aimed at -- an LLM chat page: React, streaming fetch, and a text input that has to accept keystrokes
verdict:  painted 11536 changed px in 35.1s but 1 JS exception(s)
host:     HTTP 200, 483424 bytes, 0.4s, 4 <script> (2 src), 2 <img>
document: 29 stylesheets, 2 script src, 2 inline script, 2 img, 81 preload, 0 font -> 32 mandatory subresources
pixels:   changed 11536 (blank control ink 4256), ink 8640, colours 544, rich-tile proxy 13, bbox [144, 12, 1264, 764]
network:  112 requests, 5 connections dialled, 107 reused, 78 modules (0 failed)
EXCEPTION x1: TypeError: cannot read property 'caller' of undefined
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496657)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496657)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496657)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496657)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496657)
    at <eval> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496657:4)
shot:     tests/scoreboard/kimi-now/kimi.png
