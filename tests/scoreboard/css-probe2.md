# Site scoreboard css-probe2

commit d6d6a0ae5, ISO build/logit.iso (sha256:9da63eee1b542124), 1 sites (+0 controls), 1 run(s) each, 49 s wall

GAP 1

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
| bing-search        | GAP        |    5.5 |    97 |  98/97 ! |   0 |      13040 |        9/60 | HTTP 200 |

## Detail

### bing-search -- GAP
url:      https://www.bing.com/search?q=python
reported: "我使用bing搜索python成功返回了结果!!!可是......CSS渲染根本无法正常使用"
verdict:  painted 13040 changed px in 5.5s and threw nothing, but never requested 1 of the 98 subresources the document asks for (91 stylesheets in the document)
host:     HTTP 200, 98332 bytes, 0.4s, 20 <script> (6 src), 3 <img>, redirects: 302 -> https://cn.bing.com/search?q=python
document: 91 stylesheets, 6 script src, 14 inline script, 1 img, 4 preload, 0 font -> 98 mandatory subresources
GAP:      the guest issued 97 requests against 98 mandatory -- SHORT BY 1. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 13040 (blank control ink 4256), ink 4776, colours 510, rich-tile proxy 10, bbox [140, 16, 1264, 764]
network:  97 requests, 5 connections dialled, 93 reused, 0 modules (0 failed)
shot:     tests/scoreboard/css-probe2/bing-search.png
