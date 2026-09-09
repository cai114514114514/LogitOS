# Site scoreboard full-corpus

commit d6d6a0ae5, ISO build/logit.iso (sha256:9da63eee1b542124), 21 sites (+2 controls), 1 run(s) each, 739 s wall

FETCH-FAIL 1, BLANK 3, ERRORS 13, PAINTED 4

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
| bing-search        | FETCH-FAIL |    0.7 |     - |        - |   0 |       2440 |           - | HTTP 200 |
| doubao             | BLANK      |    7.9 |     8 |      6/8 |   1 |       2980 |         0/0 | HTTP 200 |
| douyin             | BLANK      |    1.3 |    33 |     1/33 |   2 |       2276 |      29/201 | HTTP 200 |
| weixin             | BLANK      |    2.5 |     5 |      5/5 |   0 |       2552 |      23/149 | HTTP 200 |
| 2345               | ERRORS     |    5.5 |    32 |    18/32 |   2 |       6808 |      22/240 | HTTP 200 |
| anthropic          | ERRORS     |   12.1 |    14 |    13/14 |   2 |       7140 |       13/63 | HTTP 200 |
| apple              | ERRORS     |   15.7 |    49 |    25/49 |   2 |     566996 |       17/77 | HTTP 200 |
| baidu              | ERRORS     |    0.7 |    19 |     1/19 |   3 |      30124 |      32/368 | HTTP 200 |
| bing               | ERRORS     |    3.1 |     6 |      6/6 |   2 |     620596 |        6/32 | HTTP 200 |
| github             | ERRORS     |   94.8 |    94 |    28/94 |   1 |     203600 |        3/14 | HTTP 200 |
| google-search      | ERRORS     |    2.5 |     1 |      1/1 |   1 |      26756 |      35/516 | HTTP 200 |
| jd                 | ERRORS     |    7.9 |    17 |  18/17 ! |   7 |       8680 |        2/12 | HTTP 200 |
| kimi               | ERRORS     |   35.0 |   112 |   32/112 |   1 |      11524 |        5/59 | HTTP 200 |
| openai             | ERRORS     |   50.1 |   118 |        - |   1 |       5820 |       10/66 | HTTP 403 |
| python             | ERRORS     |    6.7 |    14 |  15/14 ! |   1 |     612828 |      42/124 | HTTP 200 |
| qwen               | ERRORS     |   61.0 |    23 |     6/23 |   2 |     615032 |      26/104 | HTTP 200 |
| stripe             | ERRORS     |   22.4 |    79 |  85/79 ! |   1 |      22036 |      41/125 | HTTP 200 |
| bilibili           | PAINTED    |    8.5 |    20 |     8/20 |   0 |     263724 |      65/472 | HTTP 200 |
| deepseek           | PAINTED    |    3.1 |    22 |    22/22 |   0 |      10760 |       22/95 | HTTP 200 |
| google             | PAINTED    |    3.7 |     4 |      2/4 |   0 |      17768 |       18/92 | HTTP 200 |
| qq                 | PAINTED    |   13.3 |    16 |     8/16 |   0 |     244660 |     67/1161 | HTTP 200 |

CONTROLS -- harness health, not results:
| control            | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| control-example    | PAINTED    |    1.3 |     1 |      1/1 |   0 |       8808 |      19/109 | HTTP 200 |
| control-wikipedia  | PAINTED    |   15.1 |    36 |     4/36 |   0 |      82764 |    207/1176 | HTTP 200 |

## Detail

### bing-search -- FETCH-FAIL
url:      https://www.bing.com/search?q=python
reported: "我使用bing搜索python成功返回了结果!!!可是......CSS渲染根本无法正常使用"
verdict:  the guest could not fetch it (TLS refused: handshake or certificate verification failed (https://www.bing.com/search?q=python)) while the host got HTTP 200
host:     HTTP 200, 99006 bytes, 0.4s, 20 <script> (6 src), 3 <img>, redirects: 302 -> https://cn.bing.com/search?q=python
document: 92 stylesheets, 6 script src, 14 inline script, 1 img, 4 preload, 0 font -> 99 mandatory subresources
pixels:   changed 2440 (blank control ink 4256), ink 4256, colours 362, rich-tile proxy 8, bbox [152, 12, 1256, 764]
shot:     tests/scoreboard/full-corpus/bing-search.png

### doubao -- BLANK
url:      https://www.doubao.com/
reported: TypeError charAt of undefined, localised to webpack module 3454
verdict:  loaded in 7.9s and painted 2980 changed pixels (1 exceptions)
host:     HTTP 200, 481915 bytes, 0.9s, 15 <script> (4 src), 1 <img>, redirects: 302 -> https://www.doubao.com/chat/
document: 1 stylesheets, 4 script src, 9 inline script, 1 img, 3 preload, 0 font -> 6 mandatory subresources
pixels:   changed 2980 (blank control ink 4256), ink 4256, colours 362, rich-tile proxy 8, bbox [152, 16, 1264, 764]
network:  8 requests, 4 connections dialled, 5 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 2
EXCEPTION x6: InternalError: stack overflow
    at e (https://lf-flow-web-cdn.doubao.com/obj/flow-doubao/doubao/chat/static/js/68898.7fc1ee37.js)
    at e (https://lf-flow-web-cdn.doubao.com/obj/flow-doubao/doubao/chat/static/js/68898.7fc1ee37.js)
    at e (https://lf-flow-web-cdn.doubao.com/obj/flow-doubao/doubao/chat/static/js/68898.7fc1ee37.js)
    at e (https://lf-flow-web-cdn.doubao.com/obj/flow-doubao/doubao/chat/static/js/68898.7fc1ee37.js)
    at e (https://lf-flow-web-cdn.doubao.com/obj/flow-doubao/doubao/chat/static/js/68898.7fc1ee37.js)
    at e (https://lf-flow-web-cdn.doubao.com/obj/flow-doubao/doubao/chat/static/js/68898.7fc1ee37.js)
    at e (https://lf-flow-web-cdn.doubao.com/obj/flow-doubao/doubao/chat/static/js/68898.7fc1ee37.js)
    at e (https://lf-flow-web-cdn.doubao.com/obj/flow-doubao/doubao/chat/static/js/68898.7fc1ee37.js)
shot:     tests/scoreboard/full-corpus/doubao.png

### douyin -- BLANK
url:      https://www.douyin.com/
reported: nothing renders at all
verdict:  loaded in 1.3s and painted 2276 changed pixels (2 exceptions)
host:     HTTP 200, 72914 bytes, 0.1s, 2 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 2 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 2276 (blank control ink 4256), ink 4349, colours 370, rich-tile proxy 9, bbox [140, 16, 1256, 764]
network:  33 requests, 14 connections dialled, 20 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 3
EXCEPTION x1: ReferenceError: 'Navigator' is not defined
    at t (https://lf9-sec.bytetos.com/obj/cookie-project-sdk/cmp.4_0d8db16c.js)
    at <anonymous> (https://lf9-sec.bytetos.com/obj/cookie-project-sdk/cmp.4_0d8db16c.js)
    at <anonymous> (https://lf9-sec.bytetos.com/obj/cookie-project-sdk/cmp.4_0d8db16c.js)
    at <eval> (https://lf9-sec.bytetos.com/obj/cookie-project-sdk/cmp.4_0d8db16c.js:13)
EXCEPTION x1: SyntaxError: unexpected token in expression: '%'
    at https://www.douyin.com/:1
shot:     tests/scoreboard/full-corpus/douyin.png

### weixin -- BLANK
url:      https://weixin.qq.com/
reported: same as douyin -- nothing renders
verdict:  loaded in 2.5s and painted 2552 changed pixels (0 exceptions)
host:     HTTP 200, 112720 bytes, 0.1s, 10 <script> (5 src), 0 <img>
document: 1 stylesheets, 3 script src, 3 inline script, 0 img, 5 preload, 0 font -> 5 mandatory subresources
pixels:   changed 2552 (blank control ink 4256), ink 4256, colours 370, rich-tile proxy 8, bbox [152, 12, 1264, 764]
network:  5 requests, 3 connections dialled, 2 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 1
shot:     tests/scoreboard/full-corpus/weixin.png

### 2345 -- ERRORS
url:      https://www.2345.com/
reported: TypeError toString of undefined
verdict:  painted 6808 changed px in 5.5s but 2 JS exception(s)
host:     HTTP 200, 171354 bytes, 0.2s, 19 <script> (12 src), 13 <img>
document: 6 stylesheets, 11 script src, 8 inline script, 13 img, 83 preload, 0 font -> 18 mandatory subresources
pixels:   changed 6808 (blank control ink 4256), ink 5375, colours 532, rich-tile proxy 12, bbox [152, 12, 1264, 764]
network:  32 requests, 7 connections dialled, 25 reused, 0 modules (0 failed)
EXCEPTION x1: NotSupportedError: importing or adopting a node from a document created by new DOMParser().parseFromString() is not supported in this build
EXCEPTION x1: TypeError: write is not a function (it is undefined)
    at <anonymous> (https://www.2345.com/#inline-script-4:7)
    at <eval> (https://www.2345.com/#inline-script-4:8)
shot:     tests/scoreboard/full-corpus/2345.png

### anthropic -- ERRORS
url:      https://www.anthropic.com/
reported: "is not a function"
verdict:  painted 7140 changed px in 12.1s but 2 JS exception(s)
host:     HTTP 200, 190425 bytes, 1.5s, 20 <script> (11 src), 0 <img>
document: 1 stylesheets, 11 script src, 9 inline script, 0 img, 0 preload, 0 font -> 13 mandatory subresources
pixels:   changed 7140 (blank control ink 4256), ink 6065, colours 539, rich-tile proxy 9, bbox [148, 12, 1264, 764]
network:  14 requests, 7 connections dialled, 7 reused, 0 modules (0 failed)
EXCEPTION (timer/event): timer: ReferenceError: 'CSSStyleDeclaration' is not defined
EXCEPTION (timer/event): timer: TypeError: cannot read property 'ownerDocument' of undefined
shot:     tests/scoreboard/full-corpus/anthropic.png

### apple -- ERRORS
url:      https://www.apple.com/
reported: 0 images; TypeError split of undefined
verdict:  painted 566996 changed px in 15.7s but 2 JS exception(s)
host:     HTTP 200, 254322 bytes, 0.5s, 20 <script> (15 src), 97 <img>
document: 9 stylesheets, 15 script src, 1 inline script, 41 img, 0 preload, 0 font -> 25 mandatory subresources
pixels:   changed 566996 (blank control ink 4256), ink 48936, colours 807, rich-tile proxy 9, bbox [140, 12, 1264, 764]
network:  49 requests, 4 connections dialled, 45 reused, 6 modules (0 failed)
non-executable <script> blocks skipped: 4
EXCEPTION (timer/event): timer: TypeError: javaEnabled is not a function (it is undefined)
EXCEPTION (module): https://www.apple.com/v/home/a/built/scripts/endless-entertainment-gallery.built.js: TypeError: not a function
shot:     tests/scoreboard/full-corpus/apple.png

### baidu -- ERRORS
url:      https://www.baidu.com/
reported: opens but unusable; a redirect works host-side and not on the machine
verdict:  painted 30124 changed px in 0.7s but 3 JS exception(s)
host:     HTTP 200, 227 bytes, 0.2s, 1 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 1 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 30124 (blank control ink 4256), ink 7261, colours 711, rich-tile proxy 12, bbox [152, 16, 1264, 768]
network:  19 requests, 8 connections dialled, 11 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 5
EXCEPTION (timer/event): event listener: TypeError: cannot read property 'ownerDocument' of undefined
EXCEPTION (timer/event): event listener: TypeError: cannot read property 'ownerDocument' of undefined
EXCEPTION (timer/event): timer: Error: [MODULE_TIMEOUT]Hang(none) Miss(plugins/bzPopper, superman-san/app/chat-input/result_59c562d)
shot:     tests/scoreboard/full-corpus/baidu.png

### bing -- ERRORS
url:      https://www.bing.com/
reported: NOT OURS: the _w ReferenceError is bing's own bug (see 2026-08-28). Real defect is layout: nav stacks vertically, 32 B of text painted
verdict:  painted 620596 changed px in 3.1s but 2 JS exception(s)
host:     HTTP 200, 183957 bytes, 0.4s, 22 <script> (4 src), 3 <img>, redirects: 302 -> https://cn.bing.com/
document: 1 stylesheets, 4 script src, 9 inline script, 1 img, 2 preload, 0 font -> 6 mandatory subresources
pixels:   changed 620596 (blank control ink 4256), ink 517154, colours 454, rich-tile proxy 9, bbox [140, 12, 1264, 768]
network:  6 requests, 3 connections dialled, 4 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 9
EXCEPTION x1: ReferenceError: 'PerformanceObserver' is not defined
    at <anonymous> (https://cn.bing.com/#inline-script-1)
    at <eval> (https://cn.bing.com/#inline-script-1:3)
EXCEPTION x1: ReferenceError: '_w' is not defined
    at <eval> (https://cn.bing.com/#inline-script-2:3)
shot:     tests/scoreboard/full-corpus/bing.png

### github -- ERRORS
url:      https://github.com/
reported: "很奇怪"
verdict:  painted 203600 changed px in 94.8s but 1 JS exception(s)
host:     HTTP 200, 573816 bytes, 1.2s, 15 <script> (9 src), 24 <img>
document: 18 stylesheets, 9 script src, 0 inline script, 22 img, 77 preload, 1 font -> 28 mandatory subresources
pixels:   changed 203600 (blank control ink 4256), ink 186122, colours 400, rich-tile proxy 8, bbox [140, 16, 1264, 764]
network:  94 requests, 5 connections dialled, 89 reused, 59 modules (0 failed)
non-executable <script> blocks skipped: 6
EXCEPTION (module): https://github.githubassets.com/assets/behaviors-bed88b5675d50f9e.js: TypeError: not an object
shot:     tests/scoreboard/full-corpus/github.png

### google-search -- ERRORS
url:      https://www.google.com/search?q=python
reported: "只要搜索入一个 Paton" -- the homepage renders; this is the path the owner actually uses
verdict:  painted 26756 changed px in 2.5s but 1 JS exception(s)
host:     HTTP 200, 91755 bytes, 0.6s, 5 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 5 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 26756 (blank control ink 4256), ink 17620, colours 640, rich-tile proxy 83, bbox [152, 12, 1264, 764]
network:  1 requests, 1 connections dialled, 1 reused, 0 modules (0 failed)
EXCEPTION (timer/event): event listener: ReferenceError: 'solveSimpleChallenge' is not defined
shot:     tests/scoreboard/full-corpus/google-search.png

### jd -- ERRORS
url:      https://www.jd.com/
reported: jQuery is not defined
verdict:  painted 8680 changed px in 7.9s but 7 JS exception(s) -- and the document asked for 18 subresources (3 stylesheets, 14 script srcs) against 17 requests issued, short by 1
host:     HTTP 200, 186989 bytes, 0.2s, 35 <script> (14 src), 12 <img>
document: 3 stylesheets, 14 script src, 21 inline script, 12 img, 4 preload, 0 font -> 18 mandatory subresources
GAP:      the guest issued 17 requests against 18 mandatory -- SHORT BY 1. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 8680 (blank control ink 4256), ink 4256, colours 410, rich-tile proxy 9, bbox [140, 12, 1264, 764]
network:  17 requests, 9 connections dialled, 8 reused, 0 modules (0 failed)
sub-resource failures (3):
    fetch failed (status 0) //misc.360buyimg.com/??mtd/pc/common/js/o2_ua.js,mtd/pc/base/1.0.0/event.js?v=20240117: TLS refused: handshake or certificate verification failed
    fetch failed (status 0) //misc.360buyimg.com/jdf/lib/jquery-1.6.4.js?v=20240117: TLS refused: handshake or certificate verification failed
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
shot:     tests/scoreboard/full-corpus/jd.png

### kimi -- ERRORS
url:      https://www.kimi.com/
reported: the one the whole browser arc was aimed at -- an LLM chat page: React, streaming fetch, and a text input that has to accept keystrokes
verdict:  painted 11524 changed px in 35.0s but 1 JS exception(s)
host:     HTTP 200, 483424 bytes, 0.3s, 4 <script> (2 src), 2 <img>
document: 29 stylesheets, 2 script src, 2 inline script, 2 img, 81 preload, 0 font -> 32 mandatory subresources
pixels:   changed 11524 (blank control ink 4256), ink 8640, colours 544, rich-tile proxy 13, bbox [144, 12, 1264, 764]
network:  112 requests, 5 connections dialled, 107 reused, 78 modules (0 failed)
EXCEPTION x1: TypeError: cannot read property 'caller' of undefined
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496670)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496670)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496670)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496670)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496670)
    at <eval> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496670:4)
shot:     tests/scoreboard/full-corpus/kimi.png

### openai -- ERRORS
url:      https://openai.com/
reported: an icon and nothing else
verdict:  painted 5820 changed px in 50.1s but 1 JS exception(s)
host:     HTTP 403, 0 bytes, 1.3s, 0 <script> (0 src), 0 <img>
pixels:   changed 5820 (blank control ink 4256), ink 6867, colours 537, rich-tile proxy 10, bbox [140, 12, 1264, 764]
network:  118 requests, 2 connections dialled, 116 reused, 0 modules (0 failed)
EXCEPTION (timer/event): event listener: TypeError: createElement is not a function (it is undefined)
shot:     tests/scoreboard/full-corpus/openai.png

### python -- ERRORS
url:      https://www.python.org/
reported: (new 2026-08-28: user reports it does not open)
verdict:  painted 612828 changed px in 6.7s but 1 JS exception(s) -- and the document asked for 15 subresources (4 stylesheets, 10 script srcs) against 14 requests issued, short by 1
host:     HTTP 200, 52743 bytes, 1.1s, 13 <script> (10 src), 1 <img>
document: 4 stylesheets, 10 script src, 2 inline script, 1 img, 2 preload, 0 font -> 15 mandatory subresources
GAP:      the guest issued 14 requests against 15 mandatory -- SHORT BY 1. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 612828 (blank control ink 4256), ink 501901, colours 969, rich-tile proxy 20, bbox [140, 16, 1260, 764]
network:  14 requests, 6 connections dialled, 8 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 1
EXCEPTION (timer/event): event listener: TypeError: write is not a function (it is undefined)
shot:     tests/scoreboard/full-corpus/python.png

### qwen -- ERRORS
url:      https://chat.qwen.ai/
reported: hangs
verdict:  painted 615032 changed px in 61.0s but 2 JS exception(s)
host:     HTTP 200, 196957 bytes, 0.5s, 18 <script> (3 src), 6 <img>
document: 2 stylesheets, 3 script src, 15 inline script, 6 img, 7 preload, 0 font -> 6 mandatory subresources
pixels:   changed 615032 (blank control ink 4256), ink 510887, colours 612, rich-tile proxy 10, bbox [140, 12, 1264, 764]
network:  23 requests, 6 connections dialled, 17 reused, 15 modules (1 failed)
EXCEPTION x1: InternalError: interrupted
    at H (https://o.alicdn.com/frontend-lib/common-lib/jquery.min.js)
    at wb (https://o.alicdn.com/frontend-lib/common-lib/jquery.min.js)
    at wl (https://o.alicdn.com/frontend-lib/common-lib/jquery.min.js)
    at <anonymous> (https://o.alicdn.com/frontend-lib/common-lib/jquery.min.js)
    at <anonymous> (https://o.alicdn.com/frontend-lib/common-lib/jquery.min.js)
    at Ipexfv (https://o.alicdn.com/frontend-lib/common-lib/jquery.min.js)
    at <eval> (https://o.alicdn.com/frontend-lib/common-lib/jquery.min.js)
EXCEPTION (module): https://assets.alicdn.com/g/qwenweb/qwen-chat-fe/0.2.89/js/main.js: SyntaxError: invalid escape sequence in regular expression
shot:     tests/scoreboard/full-corpus/qwen.png

### stripe -- ERRORS
url:      https://stripe.com/
reported: 73 scripts, extremely slow
verdict:  painted 22036 changed px in 22.4s but 1 JS exception(s) -- and the document asked for 85 subresources (6 stylesheets, 78 script srcs) against 79 requests issued, short by 6
host:     HTTP 200, 662654 bytes, 1.6s, 82 <script> (79 src), 35 <img>
document: 6 stylesheets, 78 script src, 0 inline script, 35 img, 14 preload, 2 font -> 85 mandatory subresources
GAP:      the guest issued 79 requests against 85 mandatory -- SHORT BY 6. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 22036 (blank control ink 4256), ink 12988, colours 690, rich-tile proxy 9, bbox [152, 12, 1264, 764]
network:  79 requests, 3 connections dialled, 76 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 3
EXCEPTION x2: SyntaxError: expecting ';'
    at https://stripe.com/:1
shot:     tests/scoreboard/full-corpus/stripe.png

### bilibili -- PAINTED
url:      https://www.bilibili.com/
reported: TypeError split of undefined
verdict:  painted 263724 changed px in 8.5s, no JS exceptions, no subresource gap
host:     HTTP 200, 116548 bytes, 0.2s, 21 <script> (8 src), 13 <img>
document: 1 stylesheets, 6 script src, 12 inline script, 12 img, 1 preload, 0 font -> 8 mandatory subresources
pixels:   changed 263724 (blank control ink 4256), ink 55338, colours 10541, rich-tile proxy 423, bbox [140, 12, 1264, 764]
network:  20 requests, 8 connections dialled, 12 reused, 0 modules (0 failed)
shot:     tests/scoreboard/full-corpus/bilibili.png

### control-example -- PAINTED
url:      http://example.com/
reported: CONTROL: the simplest page on the web, no JS, no CSS worth the name
verdict:  painted 8808 changed px in 1.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 559 bytes, 0.2s, 0 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 0 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 8808 (blank control ink 4256), ink 9184, colours 545, rich-tile proxy 9, bbox [140, 12, 1256, 764]
network:  1 requests, 1 connections dialled, 0 reused, 0 modules (0 failed)
shot:     tests/scoreboard/full-corpus/control-example.png

### control-wikipedia -- PAINTED
url:      https://en.wikipedia.org/wiki/Operating_system
reported: CONTROL: known to render since M13
verdict:  painted 82764 changed px in 15.1s, no JS exceptions, no subresource gap
host:     HTTP 200, 676091 bytes, 1.0s, 5 <script> (1 src), 26 <img>
document: 2 stylesheets, 1 script src, 3 inline script, 24 img, 2 preload, 0 font -> 4 mandatory subresources
pixels:   changed 82764 (blank control ink 4256), ink 20565, colours 1365, rich-tile proxy 52, bbox [152, 12, 1264, 764]
network:  36 requests, 4 connections dialled, 32 reused, 0 modules (0 failed)
sub-resource failures (9):
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/0/0b/Wikiversity_logo_2017.svg/40px-Wikiversity_logo_2017.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/4/41/Global_thinking.svg/20px-Global_thinking.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/8/8a/OOjs_UI_icon_edit-ltr-progressive.svg/20px-OOjs_UI_icon_edit-ltr-progressive.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/d/d4/Layers_of_a_Linux_system.png/500px-Layers_of_a_Linux_system.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/d/db/Diagram_of_a_security_descriptor_for_a_file_on_Windows.png/330px-Diagram_of_a_security_descriptor_for_a_file_on_Windows.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/4/4a/Commons-logo.svg/40px-Commons-logo.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/e/e0/Symbol_question.svg/20px-Symbol_question.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/9/96/Symbol_category_class.svg/20px-Symbol_category_class.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
non-executable <script> blocks skipped: 1
shot:     tests/scoreboard/full-corpus/control-wikipedia.png

### deepseek -- PAINTED
url:      https://www.deepseek.com/
reported: blank; needs Worker
verdict:  painted 10760 changed px in 3.1s, no JS exceptions, no subresource gap
host:     HTTP 200, 86879 bytes, 0.2s, 42 <script> (19 src), 2 <img>
document: 3 stylesheets, 18 script src, 23 inline script, 2 img, 3 preload, 0 font -> 22 mandatory subresources
pixels:   changed 10760 (blank control ink 4256), ink 13958, colours 630, rich-tile proxy 14, bbox [140, 16, 1264, 768]
network:  22 requests, 2 connections dialled, 20 reused, 0 modules (0 failed)
shot:     tests/scoreboard/full-corpus/deepseek.png

### google -- PAINTED
url:      https://www.google.com/
reported: "验证都无法加载出来" -- the bot check appears and its challenge will not render
verdict:  painted 17768 changed px in 3.7s, no JS exceptions, no subresource gap
host:     HTTP 200, 86984 bytes, 1.0s, 10 <script> (0 src), 1 <img>
document: 1 stylesheets, 0 script src, 10 inline script, 1 img, 0 preload, 0 font -> 2 mandatory subresources
pixels:   changed 17768 (blank control ink 4256), ink 5607, colours 735, rich-tile proxy 10, bbox [140, 12, 1264, 764]
network:  4 requests, 2 connections dialled, 2 reused, 0 modules (0 failed)
shot:     tests/scoreboard/full-corpus/google.png

### qq -- PAINTED
url:      https://www.qq.com/
reported: Uncaught (in promise) undefined
verdict:  painted 244660 changed px in 13.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 126459 bytes, 0.2s, 13 <script> (7 src), 17 <img>
document: 1 stylesheets, 6 script src, 6 inline script, 16 img, 0 preload, 0 font -> 8 mandatory subresources
pixels:   changed 244660 (blank control ink 4256), ink 148639, colours 1048, rich-tile proxy 45, bbox [140, 12, 1264, 764]
network:  16 requests, 7 connections dialled, 9 reused, 0 modules (0 failed)
shot:     tests/scoreboard/full-corpus/qq.png
