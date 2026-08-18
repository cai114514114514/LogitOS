# Site scoreboard 0817-final

commit 371c1237f, ISO build/logit.iso (sha256:ba1d0615b6a1efd8), 1 sites (+0 controls), 1 run(s) each, 60 s wall

PAINTED 1

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
| bilibili           | PAINTED    |   18.7 |    20 |     8/20 |   0 |     255272 |      59/583 | HTTP 200 |

## Detail

### bilibili -- PAINTED
url:      https://www.bilibili.com/
reported: TypeError split of undefined
verdict:  painted 255272 changed px in 18.7s, no JS exceptions, no subresource gap
host:     HTTP 200, 116533 bytes, 1.0s, 21 <script> (8 src), 13 <img>
document: 1 stylesheets, 6 script src, 12 inline script, 12 img, 1 preload, 0 font -> 8 mandatory subresources
pixels:   changed 255272 (blank control ink 3902), ink 70873, colours 7265, rich-tile proxy 349, bbox [140, 12, 1264, 764]
network:  20 requests, 8 connections dialled, 12 reused, 0 modules (0 failed)
shot:     tests/scoreboard/0817-final/bilibili.png
