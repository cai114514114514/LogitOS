# Site scoreboard collapse-after

commit 9ba575e93, ISO build-collapse/logit.iso (sha256:4e5082b7393da6de), 5 sites (+0 controls), 1 run(s) each, 222 s wall

ERRORS 4, PAINTED 1

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
| github             | ERRORS     |  157.5 |    94 |    28/94 |   1 |     203536 |        3/14 | HTTP 200 |
| jd                 | ERRORS     |   11.5 |    17 |  18/17 ! |   7 |       8660 |        2/12 | HTTP 200 |
| kimi               | ERRORS     |   47.1 |   112 |   32/112 |   1 |      11372 |        5/59 | HTTP 200 |
| openai             | ERRORS     |    8.5 |     2 |  118/2 ! |   1 |      25944 |     111/574 | HTTP 200 |
| deepseek           | PAINTED    |    4.9 |    22 |    22/22 |   0 |      10852 |       22/95 | HTTP 200 |

## Detail

### github -- ERRORS
url:      https://github.com/
reported: "很奇怪"
verdict:  painted 203536 changed px in 157.5s but 1 JS exception(s)
host:     HTTP 200, 573825 bytes, 1.7s, 15 <script> (9 src), 24 <img>
document: 18 stylesheets, 9 script src, 0 inline script, 22 img, 77 preload, 1 font -> 28 mandatory subresources
pixels:   changed 203536 (blank control ink 4256), ink 186130, colours 401, rich-tile proxy 8, bbox [140, 12, 1264, 764]
network:  94 requests, 5 connections dialled, 89 reused, 59 modules (0 failed)
non-executable <script> blocks skipped: 6
EXCEPTION (module): https://github.githubassets.com/assets/behaviors-bed88b5675d50f9e.js: TypeError: not an object
shot:     tests/scoreboard/collapse-after/github.png

### jd -- ERRORS
url:      https://www.jd.com/
reported: jQuery is not defined
verdict:  painted 8660 changed px in 11.5s but 7 JS exception(s) -- and the document asked for 18 subresources (3 stylesheets, 14 script srcs) against 17 requests issued, short by 1
host:     HTTP 200, 190878 bytes, 0.1s, 35 <script> (14 src), 12 <img>
document: 3 stylesheets, 14 script src, 21 inline script, 12 img, 4 preload, 0 font -> 18 mandatory subresources
GAP:      the guest issued 17 requests against 18 mandatory -- SHORT BY 1. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 8660 (blank control ink 4256), ink 4256, colours 410, rich-tile proxy 9, bbox [140, 16, 1256, 764]
network:  17 requests, 8 connections dialled, 9 reused, 0 modules (0 failed)
sub-resource failures (3):
    fetch failed (status 0[tls] chain of 3 verified for wl.jd.com, alpn=http/1.1
    fetch failed (status 0) //misc.360buyimg.com/??mtd/pc/common/js/o2_ua.js,mtd/pc/base/1.0.0/event.js?v=20240117: TLS refused: handshake or certificate verification failed
    fetch failed (status 0) //static.360buyimg.com/item/assets/oldman/wza1/aria.js?appid=bfeaebea192374ec1f220455f8d5f952: TLS refused: handshake or certificate verification failed
EXCEPTION x1: ReferenceError: 'jQuery' is not defined
    at <eval> (https://wl.jd.com/wl.js:2)
EXCEPTION x1: ReferenceError: '$' is not defined
    at clickReport (https://www.jd.com/#inline-script-16:3)
    at <eval> (https://www.jd.com/#inline-script-16:18)
EXCEPTION x1: ReferenceError: '$' is not defined
    at getClstagPrefix (https://www.jd.com/#inline-script-17:4)
    at footerRender (https://www.jd.com/#inline-script-17:20)
    at <eval> (https://www.jd.com/#inline-script-17:24)
EXCEPTION x1: TypeError: cannot read property 'length' of undefined
    at _$f6 (https://storage.360buyimg.com/webcontainer/js_security_v3_0.1.4.js?v=20240117:4)
    at <anonymous> (https://storage.360buyimg.com/webcontainer/js_security_v3_0.1.4.js?v=20240117)
    at <anonymous> (https://storage.360buyimg.com/webcontainer/js_security_v3_0.1.4.js?v=20240117:5)
    at <eval> (https://storage.360buyimg.com/webcontainer/js_security_v3_0.1.4.js?v=20240117:5)
EXCEPTION x1: TypeError: not a function
    at <anonymous> (https://www.jd.com/#inline-script-20:31)
    at <eval> (https://www.jd.com/#inline-script-20:32)
EXCEPTION x1: ReferenceError: '$' is not defined
    at <anonymous> (https://storage.360buyimg.com/retail-mall/mall-common-component/prod/1.0.19/js/index.d0da118c.js:111)
    at <anonymous> (https://storage.360buyimg.com/retail-mall/mall-common-component/prod/1.0.19/js/index.d0da118c.js:115)
    at <eval> (https://storage.360buyimg.com/retail-mall/mall-common-component/prod/1.0.19/js/index.d0da118c.js:115)
EXCEPTION x1: TypeError: cannot read property 'eventCenter' of undefined
    at BatchQueue (https://storage.360buyimg.com/channel2022/jd_home/0.0.184/static/js/index.chunk.js)
    at tLPg (https://storage.360buyimg.com/channel2022/jd_home/0.0.184/static/js/index.chunk.js)
    at call (native)
    at __webpack_require__ (https://storage.360buyimg.com/channel2022/jd_home/0.0.184/static/js/runtime.js)
    at n5if (https://storage.360buyimg.com/channel2022/jd_home/0.0.184/static/js/index.chunk.js)
    at call (native)
    at __webpack_require__ (https://storage.360buyimg.com/channel2022/jd_home/0.0.184/static/js/runtime.js)
    at pAZN (https://storage.360buyimg.com/channel2022/jd_home/0.0.184/static/js/index.chunk.js)
shot:     tests/scoreboard/collapse-after/jd.png

### kimi -- ERRORS
url:      https://www.kimi.com/
reported: the one the whole browser arc was aimed at -- an LLM chat page: React, streaming fetch, and a text input that has to accept keystrokes
verdict:  painted 11372 changed px in 47.1s but 1 JS exception(s)
host:     HTTP 200, 483424 bytes, 0.3s, 4 <script> (2 src), 2 <img>
document: 29 stylesheets, 2 script src, 2 inline script, 2 img, 81 preload, 0 font -> 32 mandatory subresources
pixels:   changed 11372 (blank control ink 4256), ink 8624, colours 580, rich-tile proxy 14, bbox [144, 12, 1264, 764]
network:  112 requests, 5 connections dialled, 107 reused, 78 modules (0 failed)
EXCEPTION x1: TypeError: cannot read property 'caller' of undefined
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496671)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496671)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496671)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496671)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496671)
    at <eval> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496671:4)
shot:     tests/scoreboard/collapse-after/kimi.png

### openai -- ERRORS
url:      https://openai.com/
reported: an icon and nothing else
verdict:  painted 25944 changed px in 8.5s but 1 JS exception(s) -- and the document asked for 118 subresources (26 stylesheets, 91 script srcs) against 2 requests issued, short by 116
host:     HTTP 200, 450047 bytes, 1.3s, 100 <script> (92 src), 18 <img>
document: 26 stylesheets, 91 script src, 8 inline script, 17 img, 3 preload, 0 font -> 118 mandatory subresources
GAP:      the guest issued 2 requests against 118 mandatory -- SHORT BY 116. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 25944 (blank control ink 4256), ink 5094, colours 675, rich-tile proxy 11, bbox [152, 16, 1264, 764]
network:  2 requests, 2 connections dialled, 0 reused, 0 modules (0 failed)
EXCEPTION (timer/event): event listener: TypeError: createElement is not a function (it is undefined)
shot:     tests/scoreboard/collapse-after/openai.png

### deepseek -- PAINTED
url:      https://www.deepseek.com/
reported: blank; needs Worker
verdict:  painted 10852 changed px in 4.9s, no JS exceptions, no subresource gap
host:     HTTP 200, 86879 bytes, 0.2s, 42 <script> (19 src), 2 <img>
document: 3 stylesheets, 18 script src, 23 inline script, 2 img, 3 preload, 0 font -> 22 mandatory subresources
pixels:   changed 10852 (blank control ink 4256), ink 13958, colours 630, rich-tile proxy 14, bbox [140, 12, 1264, 768]
network:  22 requests, 2 connections dialled, 20 reused, 0 modules (0 failed)
shot:     tests/scoreboard/collapse-after/deepseek.png
