# Site scoreboard mapuaf-diag

commit fe748e31e, ISO build-qjs/logit.iso (sha256:7e91d1a055b62ccf), 1 sites (+0 controls), 1 run(s) each, 48 s wall

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
| deepseek           | PAINTED    |    3.7 |    22 |    22/22 |   0 |     615888 |      70/361 | HTTP 200 |

## Detail

### deepseek -- PAINTED
url:      https://www.deepseek.com/
reported: blank; needs Worker
verdict:  painted 615888 changed px in 3.7s, no JS exceptions, no subresource gap
host:     HTTP 200, 86867 bytes, 0.1s, 42 <script> (19 src), 2 <img>
document: 3 stylesheets, 18 script src, 23 inline script, 2 img, 3 preload, 0 font -> 22 mandatory subresources
pixels:   changed 615888 (blank control ink 4256), ink 18178, colours 1331, rich-tile proxy 39, bbox [140, 12, 1264, 768]
network:  22 requests, 1 connections dialled, 0 reused, 0 modules (0 failed)
shot:     ../../../../tmp/mapuaf-out/deepseek.png
