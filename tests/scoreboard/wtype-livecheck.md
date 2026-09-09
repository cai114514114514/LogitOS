# Site scoreboard wtype-livecheck

commit fe748e31e, ISO build-wtype/logit.iso (sha256:259949873b2a2da7), 1 sites (+0 controls), 1 run(s) each, 46 s wall

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
| google-search      | ERRORS     |    1.9 |     3 |      1/3 |   1 |      17200 |      41/274 | HTTP 200 |

## Detail

### google-search -- ERRORS
url:      https://www.google.com/search?q=python
reported: "只要搜索入一个 Paton" -- the homepage renders; this is the path the owner actually uses
verdict:  painted 17200 changed px in 1.9s but 1 JS exception(s)
host:     HTTP 200, 91391 bytes, 0.8s, 5 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 5 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 17200 (blank control ink 4256), ink 11933, colours 646, rich-tile proxy 19, bbox [152, 16, 1264, 764]
network:  3 requests, 2 connections dialled, 2 reused, 0 modules (0 failed)
EXCEPTION (timer/event): event listener: ReferenceError: 'solveSimpleChallenge' is not defined
shot:     tests/scoreboard/wtype-livecheck/google-search.png
