# Site scoreboard collapse-openai

commit 9ba575e93, ISO build-collapse/logit.iso (sha256:e92960a838ea8006), 1 sites (+0 controls), 2 run(s) each, 45 s wall

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
| openai             | ERRORS     |    2.5 |     2 |        - |   1 |      25824 |     111/574 | HTTP 403 |

## Detail

### openai -- ERRORS
url:      https://openai.com/
reported: an icon and nothing else
verdict:  painted 25824 changed px in 2.5s but 1 JS exception(s)
host:     HTTP 403, 0 bytes, 0.4s, 0 <script> (0 src), 0 <img>
pixels:   changed 25824 (blank control ink 4256), ink 5094, colours 675, rich-tile proxy 11, bbox [152, 16, 1256, 764]
network:  2 requests, 2 connections dialled, 0 reused, 0 modules (0 failed)
EXCEPTION (timer/event): event listener: TypeError: createElement is not a function (it is undefined)
shot:     tests/scoreboard/collapse-openai/openai.png
