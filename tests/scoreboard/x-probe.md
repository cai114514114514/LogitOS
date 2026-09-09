# Site scoreboard x-probe

commit 9ba575e93, ISO build/logit.iso (sha256:9da63eee1b542124), 1 sites (+0 controls), 1 run(s) each, 165 s wall

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
| x                  | ERRORS     |  122.6 |   408 |    3/408 |   5 |      10692 |       14/75 | HTTP 200 |

## Detail

### x -- ERRORS
url:      https://x.com/
reported: "登录上X刷上第一条帖子" -- never measured; the third of the owner three product goals
verdict:  painted 10692 changed px in 122.6s but 5 JS exception(s)
host:     HTTP 200, 32593 bytes, 1.0s, 5 <script> (1 src), 0 <img>
document: 1 stylesheets, 1 script src, 4 inline script, 0 img, 4 preload, 3 font -> 3 mandatory subresources
pixels:   changed 10692 (blank control ink 4256), ink 10079, colours 625, rich-tile proxy 8, bbox [152, 12, 1264, 764]
network:  408 requests, 6 connections dialled, 402 reused, 405 modules (0 failed)
EXCEPTION x1: TypeError: cannot read property 'remove' of null
    at <eval> (https://x.com/#inline-script-1)
EXCEPTION x1: TypeError: cannot read property 'remove' of null
    at <eval> (https://x.com/#inline-script-2)
EXCEPTION x1: TypeError: cannot read property 'remove' of null
    at <eval> (https://x.com/#inline-script-3:78)
EXCEPTION x1: TypeError: cannot read property 'remove' of null
    at <eval> (https://x.com/#inline-script-4)
EXCEPTION (module): https://abs.twimg.com/x-web/x-web/entry-client-logged-out-ChZS7a9V.js: InternalError: interrupted
shot:     tests/scoreboard/x-probe/x.png
