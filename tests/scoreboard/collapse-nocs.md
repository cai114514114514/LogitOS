# Site scoreboard collapse-nocs

commit 9ba575e93, ISO build-collapse/logit.iso (sha256:e92960a838ea8006), 11 sites (+1 controls), 1 run(s) each, 204 s wall

BLANK 2, ERRORS 6, PAINTED 3

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
| doubao             | BLANK      |    7.9 |     8 |      6/8 |   0 |       2996 |        6/45 | HTTP 200 |
| douyin             | BLANK      |    1.3 |    33 |     1/33 |   2 |       2360 |      31/213 | HTTP 200 |
| 2345               | ERRORS     |    6.1 |    32 |    18/32 |   2 |       6488 |      22/249 | HTTP 200 |
| github             | ERRORS     |   90.7 |    94 |    28/94 |   1 |     221268 |      71/271 | HTTP 200 |
| jd                 | ERRORS     |    8.5 |    17 |  18/17 ! |   7 |       8732 |        2/12 | HTTP 200 |
| kimi               | ERRORS     |   37.6 |   112 |   32/112 |   1 |      12240 |      21/157 | HTTP 200 |
| python             | ERRORS     |    4.9 |    14 |  15/14 ! |   2 |     613216 |      42/124 | HTTP 200 |
| stripe             | ERRORS     |   18.8 |    80 |  85/80 ! |   1 |      21980 |      41/125 | HTTP 200 |
| bilibili           | PAINTED    |   10.3 |    22 |     8/22 |   0 |     223832 |      58/587 | HTTP 200 |
| deepseek           | PAINTED    |    4.3 |    22 |    22/22 |   0 |     615912 |      70/361 | HTTP 200 |
| qq                 | PAINTED    |   13.4 |    16 |     8/16 |   0 |     243928 |     71/1141 | HTTP 200 |

CONTROLS -- harness health, not results:
| control            | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| control-wikipedia  | PAINTED    |   12.8 |    38 |     4/38 |   0 |      82576 |    207/1176 | HTTP 200 |

## Detail

### doubao -- BLANK
url:      https://www.doubao.com/
reported: TypeError charAt of undefined, localised to webpack module 3454
verdict:  loaded in 7.9s and painted 2996 changed pixels (0 exceptions)
host:     HTTP 200, 482522 bytes, 0.8s, 15 <script> (4 src), 1 <img>, redirects: 302 -> https://www.doubao.com/chat/
document: 1 stylesheets, 4 script src, 9 inline script, 1 img, 3 preload, 0 font -> 6 mandatory subresources
pixels:   changed 2996 (blank control ink 4256), ink 4256, colours 363, rich-tile proxy 8, bbox [152, 12, 1260, 764]
network:  8 requests, 4 connections dialled, 5 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 2
shot:     tests/scoreboard/collapse-nocs/doubao.png

### douyin -- BLANK
url:      https://www.douyin.com/
reported: nothing renders at all
verdict:  loaded in 1.3s and painted 2360 changed pixels (2 exceptions)
host:     HTTP 200, 72914 bytes, 0.2s, 2 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 2 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 2360 (blank control ink 4256), ink 4349, colours 370, rich-tile proxy 9, bbox [140, 12, 1264, 764]
network:  33 requests, 14 connections dialled, 20 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 3
EXCEPTION x1: TypeError: cannot read property 'getAttribute' of null
    at value (https://lf-security.bytegoofy.com/obj/security-secsdk/runtime_bundler_34.js)
    at e (https://lf-security.bytegoofy.com/obj/security-secsdk/runtime_bundler_34.js)
    at <anonymous> (https://lf-security.bytegoofy.com/obj/security-secsdk/runtime_bundler_34.js:7)
    at <anonymous> (https://lf-security.bytegoofy.com/obj/security-secsdk/runtime_bundler_34.js)
    at <eval> (https://lf-security.bytegoofy.com/obj/security-secsdk/runtime_bundler_34.js:7)
EXCEPTION x1: SyntaxError: unexpected token in expression: '%'
    at https://www.douyin.com/:1
shot:     tests/scoreboard/collapse-nocs/douyin.png

### 2345 -- ERRORS
url:      https://www.2345.com/
reported: TypeError toString of undefined
verdict:  painted 6488 changed px in 6.1s but 2 JS exception(s)
host:     HTTP 200, 172022 bytes, 0.2s, 19 <script> (12 src), 13 <img>
document: 6 stylesheets, 11 script src, 8 inline script, 13 img, 83 preload, 0 font -> 18 mandatory subresources
pixels:   changed 6488 (blank control ink 4256), ink 5401, colours 523, rich-tile proxy 13, bbox [152, 12, 1264, 764]
network:  32 requests, 7 connections dialled, 25 reused, 0 modules (0 failed)
EXCEPTION x1: NotSupportedError: importing or adopting a node from a document created by new DOMParser().parseFromString() is not supported in this build
EXCEPTION x1: TypeError: write is not a function (it is undefined)
    at <anonymous> (https://www.2345.com/#inline-script-4:7)
    at <eval> (https://www.2345.com/#inline-script-4:8)
shot:     tests/scoreboard/collapse-nocs/2345.png

### github -- ERRORS
url:      https://github.com/
reported: "很奇怪"
verdict:  painted 221268 changed px in 90.7s but 1 JS exception(s)
host:     HTTP 200, 573822 bytes, 1.1s, 15 <script> (9 src), 24 <img>
document: 18 stylesheets, 9 script src, 0 inline script, 22 img, 77 preload, 1 font -> 28 mandatory subresources
pixels:   changed 221268 (blank control ink 4256), ink 83024, colours 2022, rich-tile proxy 25, bbox [140, 12, 1264, 764]
network:  94 requests, 5 connections dialled, 89 reused, 59 modules (0 failed)
non-executable <script> blocks skipped: 6
EXCEPTION (module): https://github.githubassets.com/assets/behaviors-bed88b5675d50f9e.js: TypeError: not an object
shot:     tests/scoreboard/collapse-nocs/github.png

### jd -- ERRORS
url:      https://www.jd.com/
reported: jQuery is not defined
verdict:  painted 8732 changed px in 8.5s but 7 JS exception(s) -- and the document asked for 18 subresources (3 stylesheets, 14 script srcs) against 17 requests issued, short by 1
host:     HTTP 200, 192022 bytes, 0.1s, 35 <script> (14 src), 12 <img>
document: 3 stylesheets, 14 script src, 21 inline script, 12 img, 4 preload, 0 font -> 18 mandatory subresources
GAP:      the guest issued 17 requests against 18 mandatory -- SHORT BY 1. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 8732 (blank control ink 4256), ink 4256, colours 410, rich-tile proxy 9, bbox [140, 12, 1260, 764]
network:  17 requests, 8 connections dialled, 9 reused, 0 modules (0 failed)
sub-resource failures (3):
    fetch failed (status 0) //misc.360buyimg.com/jdf/lib/jquery-1.6.4.js?v=20240117: TLS refused: handshake or certificate verification failed
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
shot:     tests/scoreboard/collapse-nocs/jd.png

### kimi -- ERRORS
url:      https://www.kimi.com/
reported: the one the whole browser arc was aimed at -- an LLM chat page: React, streaming fetch, and a text input that has to accept keystrokes
verdict:  painted 12240 changed px in 37.6s but 1 JS exception(s)
host:     HTTP 200, 483424 bytes, 0.3s, 4 <script> (2 src), 2 <img>
document: 29 stylesheets, 2 script src, 2 inline script, 2 img, 81 preload, 0 font -> 32 mandatory subresources
pixels:   changed 12240 (blank control ink 4256), ink 9303, colours 684, rich-tile proxy 18, bbox [152, 12, 1264, 764]
network:  112 requests, 5 connections dialled, 107 reused, 78 modules (0 failed)
EXCEPTION x1: TypeError: cannot read property 'caller' of undefined
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496673)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496673)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496673)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496673)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496673)
    at <eval> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496673:4)
shot:     tests/scoreboard/collapse-nocs/kimi.png

### python -- ERRORS
url:      https://www.python.org/
reported: (new 2026-08-28: user reports it does not open)
verdict:  painted 613216 changed px in 4.9s but 2 JS exception(s) -- and the document asked for 15 subresources (4 stylesheets, 10 script srcs) against 14 requests issued, short by 1
host:     HTTP 200, 52603 bytes, 0.6s, 13 <script> (10 src), 1 <img>
document: 4 stylesheets, 10 script src, 2 inline script, 1 img, 2 preload, 0 font -> 15 mandatory subresources
GAP:      the guest issued 14 requests against 15 mandatory -- SHORT BY 1. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 613216 (blank control ink 4256), ink 501824, colours 971, rich-tile proxy 20, bbox [140, 12, 1264, 764]
network:  14 requests, 6 connections dialled, 8 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 1
EXCEPTION x1: TypeError: cannot read property 'getAttribute' of null
    at t (https://analytics.python.org/js/script.file-downloads.outbound-links.js)
    at <anonymous> (https://analytics.python.org/js/script.file-downloads.outbound-links.js)
    at <eval> (https://analytics.python.org/js/script.file-downloads.outbound-links.js)
EXCEPTION (timer/event): event listener: TypeError: write is not a function (it is undefined)
shot:     tests/scoreboard/collapse-nocs/python.png

### stripe -- ERRORS
url:      https://stripe.com/
reported: 73 scripts, extremely slow
verdict:  painted 21980 changed px in 18.8s but 1 JS exception(s) -- and the document asked for 85 subresources (6 stylesheets, 78 script srcs) against 80 requests issued, short by 5
host:     HTTP 200, 662662 bytes, 1.0s, 82 <script> (79 src), 35 <img>
document: 6 stylesheets, 78 script src, 0 inline script, 35 img, 14 preload, 2 font -> 85 mandatory subresources
GAP:      the guest issued 80 requests against 85 mandatory -- SHORT BY 5. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 21980 (blank control ink 4256), ink 12988, colours 693, rich-tile proxy 9, bbox [152, 16, 1260, 764]
network:  80 requests, 4 connections dialled, 76 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 3
EXCEPTION x2: SyntaxError: expecting ';'
    at https://stripe.com/:1
shot:     tests/scoreboard/collapse-nocs/stripe.png

### bilibili -- PAINTED
url:      https://www.bilibili.com/
reported: TypeError split of undefined
verdict:  painted 223832 changed px in 10.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 119462 bytes, 0.4s, 21 <script> (8 src), 13 <img>
document: 1 stylesheets, 6 script src, 12 inline script, 12 img, 1 preload, 0 font -> 8 mandatory subresources
pixels:   changed 223832 (blank control ink 4256), ink 138924, colours 16839, rich-tile proxy 778, bbox [140, 12, 1264, 764]
network:  22 requests, 11 connections dialled, 11 reused, 0 modules (0 failed)
shot:     tests/scoreboard/collapse-nocs/bilibili.png

### control-wikipedia -- PAINTED
url:      https://en.wikipedia.org/wiki/Operating_system
reported: CONTROL: known to render since M13
verdict:  painted 82576 changed px in 12.8s, no JS exceptions, no subresource gap
host:     HTTP 200, 676091 bytes, 1.2s, 5 <script> (1 src), 26 <img>
document: 2 stylesheets, 1 script src, 3 inline script, 24 img, 2 preload, 0 font -> 4 mandatory subresources
pixels:   changed 82576 (blank control ink 4256), ink 20391, colours 1362, rich-tile proxy 52, bbox [152, 12, 1264, 764]
network:  38 requests, 4 connections dialled, 34 reused, 0 modules (0 failed)
sub-resource failures (11):
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/3/39/IBM_system_360-50_console_-_MfK_Bern.jpg/250px-IBM_system_360-50_console_-_MfK_Bern.jpg?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/5/51/Dolphin_FileManager.png/250px-Dolphin_FileManager.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/e/e0/Symbol_question.svg/20px-Symbol_question.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/8/8a/OOjs_UI_icon_edit-ltr-progressive.svg/20px-OOjs_UI_icon_edit-ltr-progressive.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/1/1b/Semi-protection-shackle.svg/20px-Semi-protection-shackle.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/d/d4/Layers_of_a_Linux_system.png/500px-Layers_of_a_Linux_system.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/d/db/Diagram_of_a_security_descriptor_for_a_file_on_Windows.png/330px-Diagram_of_a_security_descriptor_for_a_file_on_Windows.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/9/99/Wiktionary-logo-en-v2.svg/40px-Wiktionary-logo-en-v2.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
non-executable <script> blocks skipped: 1
shot:     tests/scoreboard/collapse-nocs/control-wikipedia.png

### deepseek -- PAINTED
url:      https://www.deepseek.com/
reported: blank; needs Worker
verdict:  painted 615912 changed px in 4.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 86879 bytes, 0.1s, 42 <script> (19 src), 2 <img>
document: 3 stylesheets, 18 script src, 23 inline script, 2 img, 3 preload, 0 font -> 22 mandatory subresources
pixels:   changed 615912 (blank control ink 4256), ink 18178, colours 1331, rich-tile proxy 39, bbox [140, 12, 1264, 768]
network:  22 requests, 2 connections dialled, 20 reused, 0 modules (0 failed)
shot:     tests/scoreboard/collapse-nocs/deepseek.png

### qq -- PAINTED
url:      https://www.qq.com/
reported: Uncaught (in promise) undefined
verdict:  painted 243928 changed px in 13.4s, no JS exceptions, no subresource gap
host:     HTTP 200, 126974 bytes, 0.2s, 13 <script> (7 src), 17 <img>
document: 1 stylesheets, 6 script src, 6 inline script, 16 img, 0 preload, 0 font -> 8 mandatory subresources
pixels:   changed 243928 (blank control ink 4256), ink 148384, colours 1049, rich-tile proxy 45, bbox [140, 12, 1264, 764]
network:  16 requests, 7 connections dialled, 9 reused, 0 modules (0 failed)
shot:     tests/scoreboard/collapse-nocs/qq.png
