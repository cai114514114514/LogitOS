# Site scoreboard observers-check

commit 9ba575e93, ISO build-apis/logit.iso (sha256:592096ecf0752cb2), 1 sites (+0 controls), 1 run(s) each, 47 s wall

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
| bing               | ERRORS     |    2.5 |     6 |      6/6 |   1 |     620440 |        6/32 | HTTP 200 |

## Detail

### bing -- ERRORS
url:      https://www.bing.com/
reported: NOT OURS: the _w ReferenceError is bing's own bug (see 2026-08-28). Real defect is layout: nav stacks vertically, 32 B of text painted
verdict:  painted 620440 changed px in 2.5s but 1 JS exception(s)
host:     HTTP 200, 185398 bytes, 2.4s, 22 <script> (4 src), 3 <img>, redirects: 302 -> https://cn.bing.com/
document: 1 stylesheets, 4 script src, 9 inline script, 1 img, 2 preload, 0 font -> 6 mandatory subresources
pixels:   changed 620440 (blank control ink 4256), ink 517154, colours 454, rich-tile proxy 9, bbox [140, 16, 1260, 764]
network:  6 requests, 3 connections dialled, 4 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 9
EXCEPTION x1: ReferenceError: '_w' is not defined
    at <eval> (https://cn.bing.com/#inline-script-2:3)
shot:     tests/scoreboard/observers-check/bing.png
