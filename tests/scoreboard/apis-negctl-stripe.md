# Site scoreboard apis-negctl-stripe

commit 9ba575e93, ISO build-apis/logit.iso (sha256:82ec5e3a8dc5be9e), 1 sites (+0 controls), 1 run(s) each, 84 s wall

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
| stripe             | ERRORS     |   41.1 |    78 |    78/78 |   1 |      22596 |      37/181 | HTTP 200 |

## Detail

### stripe -- ERRORS
url:      https://stripe.com/
reported: 73 scripts, extremely slow
verdict:  painted 22596 changed px in 41.1s but 1 JS exception(s)
host:     HTTP 200, 741414 bytes, 3.0s, 76 <script> (73 src), 30 <img>, redirects: 307 -> https://stripe.com/jp
document: 5 stylesheets, 72 script src, 0 inline script, 30 img, 13 preload, 2 font -> 78 mandatory subresources
pixels:   changed 22596 (blank control ink 4256), ink 12498, colours 731, rich-tile proxy 12, bbox [152, 16, 1264, 764]
network:  78 requests, 3 connections dialled, 76 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 3
EXCEPTION x2: SyntaxError: expecting ';'
    at https://stripe.com/:1
shot:     tests/scoreboard/apis-negctl-stripe/stripe.png
