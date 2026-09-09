# Site scoreboard apis-after

commit 9ba575e93, ISO build-apis/logit.iso (sha256:82ec5e3a8dc5be9e), 10 sites (+2 controls), 1 run(s) each, 217 s wall

BLANK 1, ERRORS 6, PAINTED 3

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
| douyin             | BLANK      |    1.3 |    33 |     1/33 |   1 |       2320 |      29/201 | HTTP 200 |
| github             | ERRORS     |  108.0 |    94 |    28/94 |   1 |     203572 |        3/14 | HTTP 200 |
| jd                 | ERRORS     |    9.5 |    17 |  18/17 ! |   7 |       8720 |        2/12 | HTTP 200 |
| kimi               | ERRORS     |   39.9 |   112 |   32/112 |   1 |      11316 |        5/59 | HTTP 200 |
| openai             | ERRORS     |  125.3 |   118 |        - |   1 |       6136 |       13/58 | HTTP 403 |
| python             | ERRORS     |    7.3 |    14 |  15/14 ! |   1 |     612836 |      42/124 | HTTP 200 |
| stripe             | ERRORS     |   31.4 |    78 |    78/78 |   1 |      20556 |      37/181 | HTTP 200 |
| bilibili           | PAINTED    |    9.1 |    20 |     8/20 |   0 |     260616 |      60/555 | HTTP 200 |
| deepseek           | PAINTED    |    4.3 |    22 |    22/22 |   0 |      10844 |       22/95 | HTTP 200 |
| qq                 | PAINTED    |   13.3 |    16 |     8/16 |   0 |     243888 |     71/1138 | HTTP 200 |

CONTROLS -- harness health, not results:
| control            | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| control-example    | PAINTED    |    1.3 |     1 |      1/1 |   0 |       8856 |      19/109 | HTTP 200 |
| control-wikipedia  | PAINTED    |   20.0 |    35 |     4/35 |   0 |      82528 |    207/1176 | HTTP 200 |

## Detail

### douyin -- BLANK
url:      https://www.douyin.com/
reported: nothing renders at all
verdict:  loaded in 1.3s and painted 2320 changed pixels (1 exceptions)
host:     HTTP 200, 72914 bytes, 0.1s, 2 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 2 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 2320 (blank control ink 4256), ink 4349, colours 370, rich-tile proxy 9, bbox [140, 16, 1264, 764]
network:  33 requests, 14 connections dialled, 20 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 3
EXCEPTION x1: SyntaxError: unexpected token in expression: '%'
    at https://www.douyin.com/:1
shot:     tests/scoreboard/apis-after/douyin.png

### github -- ERRORS
url:      https://github.com/
reported: "很奇怪"
verdict:  painted 203572 changed px in 108.0s but 1 JS exception(s)
host:     HTTP 200, 573830 bytes, 2.1s, 15 <script> (9 src), 24 <img>
document: 18 stylesheets, 9 script src, 0 inline script, 22 img, 77 preload, 1 font -> 28 mandatory subresources
pixels:   changed 203572 (blank control ink 4256), ink 186130, colours 401, rich-tile proxy 8, bbox [140, 12, 1264, 764]
network:  94 requests, 5 connections dialled, 89 reused, 59 modules (0 failed)
non-executable <script> blocks skipped: 6
EXCEPTION (module): https://github.githubassets.com/assets/behaviors-bed88b5675d50f9e.js: TypeError: not an object
shot:     tests/scoreboard/apis-after/github.png

### jd -- ERRORS
url:      https://www.jd.com/
reported: jQuery is not defined
verdict:  painted 8720 changed px in 9.5s but 7 JS exception(s) -- and the document asked for 18 subresources (3 stylesheets, 14 script srcs) against 17 requests issued, short by 1
host:     HTTP 200, 190881 bytes, 0.1s, 35 <script> (14 src), 12 <img>
document: 3 stylesheets, 14 script src, 21 inline script, 12 img, 4 preload, 0 font -> 18 mandatory subresources
GAP:      the guest issued 17 requests against 18 mandatory -- SHORT BY 1. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 8720 (blank control ink 4256), ink 4256, colours 410, rich-tile proxy 9, bbox [140, 16, 1264, 764]
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
shot:     tests/scoreboard/apis-after/jd.png

### kimi -- ERRORS
url:      https://www.kimi.com/
reported: the one the whole browser arc was aimed at -- an LLM chat page: React, streaming fetch, and a text input that has to accept keystrokes
verdict:  painted 11316 changed px in 39.9s but 1 JS exception(s)
host:     HTTP 200, 483424 bytes, 0.2s, 4 <script> (2 src), 2 <img>
document: 29 stylesheets, 2 script src, 2 inline script, 2 img, 81 preload, 0 font -> 32 mandatory subresources
pixels:   changed 11316 (blank control ink 4256), ink 8624, colours 580, rich-tile proxy 14, bbox [144, 12, 1264, 764]
network:  112 requests, 5 connections dialled, 107 reused, 78 modules (0 failed)
EXCEPTION x1: TypeError: cannot read property 'caller' of undefined
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496671)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496671)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496671)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496671)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496671)
    at <eval> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496671:4)
shot:     tests/scoreboard/apis-after/kimi.png

### openai -- ERRORS
url:      https://openai.com/
reported: an icon and nothing else
verdict:  painted 6136 changed px in 125.3s but 1 JS exception(s)
host:     HTTP 403, 0 bytes, 0.8s, 0 <script> (0 src), 0 <img>
pixels:   changed 6136 (blank control ink 4256), ink 6813, colours 527, rich-tile proxy 9, bbox [152, 16, 1264, 764]
network:  118 requests, 2 connections dialled, 116 reused, 0 modules (0 failed)
EXCEPTION (timer/event): event listener: TypeError: createElement is not a function (it is undefined)
shot:     tests/scoreboard/apis-after/openai.png

### python -- ERRORS
url:      https://www.python.org/
reported: (new 2026-08-28: user reports it does not open)
verdict:  painted 612836 changed px in 7.3s but 1 JS exception(s) -- and the document asked for 15 subresources (4 stylesheets, 10 script srcs) against 14 requests issued, short by 1
host:     HTTP 200, 52379 bytes, 0.7s, 13 <script> (10 src), 1 <img>
document: 4 stylesheets, 10 script src, 2 inline script, 1 img, 2 preload, 0 font -> 15 mandatory subresources
GAP:      the guest issued 14 requests against 15 mandatory -- SHORT BY 1. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 612836 (blank control ink 4256), ink 501824, colours 971, rich-tile proxy 20, bbox [140, 12, 1264, 764]
network:  14 requests, 6 connections dialled, 8 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 1
EXCEPTION (timer/event): event listener: TypeError: write is not a function (it is undefined)
shot:     tests/scoreboard/apis-after/python.png

### stripe -- ERRORS
url:      https://stripe.com/
reported: 73 scripts, extremely slow
verdict:  painted 20556 changed px in 31.4s but 1 JS exception(s)
host:     HTTP 200, 741414 bytes, 4.2s, 76 <script> (73 src), 30 <img>, redirects: 307 -> https://stripe.com/jp
document: 5 stylesheets, 72 script src, 0 inline script, 30 img, 13 preload, 2 font -> 78 mandatory subresources
pixels:   changed 20556 (blank control ink 4256), ink 10241, colours 698, rich-tile proxy 11, bbox [152, 16, 1264, 764]
network:  78 requests, 3 connections dialled, 76 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 3
EXCEPTION x2: SyntaxError: expecting ';'
    at https://stripe.com/:1
shot:     tests/scoreboard/apis-after/stripe.png

### bilibili -- PAINTED
url:      https://www.bilibili.com/
reported: TypeError split of undefined
verdict:  painted 260616 changed px in 9.1s, no JS exceptions, no subresource gap
host:     HTTP 200, 115626 bytes, 0.2s, 21 <script> (8 src), 13 <img>
document: 1 stylesheets, 6 script src, 12 inline script, 12 img, 1 preload, 0 font -> 8 mandatory subresources
pixels:   changed 260616 (blank control ink 4256), ink 79028, colours 6276, rich-tile proxy 344, bbox [140, 16, 1264, 764]
network:  20 requests, 11 connections dialled, 9 reused, 0 modules (0 failed)
shot:     tests/scoreboard/apis-after/bilibili.png

### control-example -- PAINTED
url:      http://example.com/
reported: CONTROL: the simplest page on the web, no JS, no CSS worth the name
verdict:  painted 8856 changed px in 1.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 559 bytes, 0.2s, 0 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 0 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 8856 (blank control ink 4256), ink 9184, colours 545, rich-tile proxy 9, bbox [140, 12, 1264, 764]
network:  1 requests, 1 connections dialled, 0 reused, 0 modules (0 failed)
shot:     tests/scoreboard/apis-after/control-example.png

### control-wikipedia -- PAINTED
url:      https://en.wikipedia.org/wiki/Operating_system
reported: CONTROL: known to render since M13
verdict:  painted 82528 changed px in 20.0s, no JS exceptions, no subresource gap
host:     HTTP 200, 676098 bytes, 2.1s, 5 <script> (1 src), 26 <img>
document: 2 stylesheets, 1 script src, 3 inline script, 24 img, 2 preload, 0 font -> 4 mandatory subresources
pixels:   changed 82528 (blank control ink 4256), ink 20391, colours 1362, rich-tile proxy 52, bbox [152, 12, 1260, 764]
network:  35 requests, 4 connections dialled, 31 reused, 0 modules (0 failed)
sub-resource failures (8):
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/0/0b/Wikiversity_logo_2017.svg/40px-Wikiversity_logo_2017.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/e/e0/Symbol_question.svg/20px-Symbol_question.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/8/8a/OOjs_UI_icon_edit-ltr-progressive.svg/20px-OOjs_UI_icon_edit-ltr-progressive.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/1/1b/Semi-protection-shackle.svg/20px-Semi-protection-shackle.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/d/db/Diagram_of_a_security_descriptor_for_a_file_on_Windows.png/330px-Diagram_of_a_security_descriptor_for_a_file_on_Windows.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/4/4a/Commons-logo.svg/40px-Commons-logo.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/9/99/Wiktionary-logo-en-v2.svg/40px-Wiktionary-logo-en-v2.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/9/96/Symbol_category_class.svg/20px-Symbol_category_class.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
non-executable <script> blocks skipped: 1
shot:     tests/scoreboard/apis-after/control-wikipedia.png

### deepseek -- PAINTED
url:      https://www.deepseek.com/
reported: blank; needs Worker
verdict:  painted 10844 changed px in 4.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 86879 bytes, 0.1s, 42 <script> (19 src), 2 <img>
document: 3 stylesheets, 18 script src, 23 inline script, 2 img, 3 preload, 0 font -> 22 mandatory subresources
pixels:   changed 10844 (blank control ink 4256), ink 13958, colours 630, rich-tile proxy 14, bbox [140, 12, 1264, 768]
network:  22 requests, 2 connections dialled, 20 reused, 0 modules (0 failed)
shot:     tests/scoreboard/apis-after/deepseek.png

### qq -- PAINTED
url:      https://www.qq.com/
reported: Uncaught (in promise) undefined
verdict:  painted 243888 changed px in 13.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 125988 bytes, 0.4s, 13 <script> (7 src), 17 <img>
document: 1 stylesheets, 6 script src, 6 inline script, 16 img, 0 preload, 0 font -> 8 mandatory subresources
pixels:   changed 243888 (blank control ink 4256), ink 148384, colours 1049, rich-tile proxy 45, bbox [140, 16, 1264, 764]
network:  16 requests, 7 connections dialled, 9 reused, 0 modules (0 failed)
shot:     tests/scoreboard/apis-after/qq.png
