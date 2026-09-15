# Site scoreboard complete-0913-after

commit 41d7a74e8, ISO build-browser-complete-0913/after/logit.iso (sha256:8cb72e1276ff6065), 5 sites (+2 controls), 1 run(s) each, 601 s wall

TIMEOUT 1, ERRORS 3, PAINTED 1

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
| qwen               | TIMEOUT    |  240.3 |     - |        - |   0 |       1596 |           - | HTTP 200 |
| anthropic          | ERRORS     |   16.9 |    13 |    12/13 |   1 |     323948 |      76/382 | HTTP 200 |
| douyin             | ERRORS     |    1.3 |    34 |     1/34 |   0 |      18468 |      31/213 | HTTP 200 |
| jd                 | ERRORS     |   52.6 |    18 |    18/18 |   7 |      27096 |       6/114 | HTTP 200 |
| doubao             | PAINTED    |    7.3 |     8 |      6/8 |   0 |      12092 |        9/87 | HTTP 200 |

CONTROLS -- harness health, not results:
| control            | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| control-example    | PAINTED    |    1.3 |     1 |      1/1 |   0 |       8720 |      19/109 | HTTP 200 |
| control-wikipedia  | PAINTED    |   12.1 |     7 |      4/7 |   0 |      79044 |    186/1040 | HTTP 200 |

## Detail

### qwen -- TIMEOUT
url:      https://chat.qwen.ai/
reported: hangs
verdict:  no `load done` in 240s (host fetched it in 0.3s)
host:     HTTP 200, 101120 bytes, 0.3s, 17 <script> (3 src), 6 <img>
document: 2 stylesheets, 3 script src, 14 inline script, 6 img, 7 preload, 0 font -> 6 mandatory subresources
pixels:   changed 1596 (blank control ink 4256), ink 4322, colours 385, rich-tile proxy 8, bbox [152, 12, 1264, 764]
shot:     build-browser-complete-0913/evidence/live-after/qwen.png

### anthropic -- ERRORS
url:      https://www.anthropic.com/
reported: "is not a function"
verdict:  painted 323948 changed px in 16.9s but 1 JS exception(s)
host:     HTTP 200, 174476 bytes, 2.1s, 19 <script> (10 src), 0 <img>
document: 1 stylesheets, 10 script src, 9 inline script, 0 img, 0 preload, 0 font -> 12 mandatory subresources
pixels:   changed 323948 (blank control ink 4256), ink 261388, colours 796, rich-tile proxy 11, bbox [140, 12, 1264, 764]
network:  13 requests, 6 connections dialled, 7 reused, 0 modules (0 failed)
EXCEPTION (timer/event): timer: ReferenceError: 'CSSStyleDeclaration' is not defined
shot:     build-browser-complete-0913/evidence/live-after/anthropic.png

### douyin -- ERRORS
url:      https://www.douyin.com/
reported: nothing renders at all
verdict:  painted 18468 changed px, with 1 page-reported error(s) and 0 fetch error(s)
host:     HTTP 200, 72914 bytes, 0.1s, 2 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 2 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 18468 (blank control ink 4256), ink 12323, colours 701, rich-tile proxy 17, bbox [140, 12, 1264, 764]
network:  34 requests, 13 connections dialled, 22 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 3
shot:     build-browser-complete-0913/evidence/live-after/douyin.png

### jd -- ERRORS
url:      https://www.jd.com/
reported: jQuery is not defined
verdict:  painted 27096 changed px in 52.6s but 7 JS exception(s)
host:     HTTP 200, 192299 bytes, 0.1s, 35 <script> (14 src), 12 <img>
document: 3 stylesheets, 14 script src, 21 inline script, 12 img, 4 preload, 0 font -> 18 mandatory subresources
pixels:   changed 27096 (blank control ink 4256), ink 6501, colours 1113, rich-tile proxy 66, bbox [140, 12, 1260, 764]
network:  18 requests, 7 connections dialled, 11 reused, 0 modules (0 failed)
sub-resource failures (2):
    [img] fetch failed (status 0): //api.m.jd.com?functionId=pchome_cookie_check&appid=www-jd-com&cthr=1&source=pc-home: cookie header exceeds capacity
    [img] fetch failed (status 0): https://mercury.jd.com/log.gif?t=exp_log.100000&m=UA-J2011-1&pin=-&uid=null&sid=null|null&cul=https%3A%2F%2Fwww.jd.com%2F&v=%7B%22t1%22%3A%22pc_homepage%22%2C%22t2%22%3A%22basic%22: the kernel socket table is full
EXCEPTION x1: TypeError: cannot read property 'replace' of undefined
    at s (https://wl.jd.com/wl.js)
    at Ie (https://wl.jd.com/wl.js)
    at bloading (https://wl.jd.com/wl.js)
    at <anonymous> (https://wl.jd.com/wl.js)
    at <eval> (https://wl.jd.com/wl.js:2)
EXCEPTION x1: InternalError: interrupted
    at C (https://gias.jd.com/js/pc-tk.js?v=20240117)
    at hash128 (https://gias.jd.com/js/pc-tk.js?v=20240117)
    at getFp (https://gias.jd.com/js/pc-tk.js?v=20240117)
    at <anonymous> (https://gias.jd.com/js/pc-tk.js?v=20240117)
    at <eval> (https://gias.jd.com/js/pc-tk.js?v=20240117:1)
EXCEPTION x1: ReferenceError: 'getJsToken' is not defined
    at <eval> (https://www.jd.com/#inline-script-19:2)
EXCEPTION x1: TypeError: cannot read property 'length' of undefined
    at _$P6 (https://storage.360buyimg.com/webcontainer/js_security_v3_0.1.4.js?v=20240117:4)
    at cNsQP (https://storage.360buyimg.com/webcontainer/js_security_v3_0.1.4.js?v=20240117)
    at <anonymous> (https://storage.360buyimg.com/webcontainer/js_security_v3_0.1.4.js?v=20240117)
    at <anonymous> (https://storage.360buyimg.com/webcontainer/js_security_v3_0.1.4.js?v=20240117:5)
    at <eval> (https://storage.360buyimg.com/webcontainer/js_security_v3_0.1.4.js?v=20240117:5)
EXCEPTION x1: TypeError: ParamsSign is not a function (it is undefined)
    at <anonymous> (https://www.jd.com/#inline-script-20:31)
    at <eval> (https://www.jd.com/#inline-script-20:32)
EXCEPTION x1: TypeError: cannot read property 'replace' of undefined
    at getBrowserInfo (https://static.360buyimg.com/item/assets/oldman/wza1/aria.js?appid=bfeaebea192374ec1f220455f8d5f952)
    at check (https://static.360buyimg.com/item/assets/oldman/wza1/aria.js?appid=bfeaebea192374ec1f220455f8d5f952)
    at 1019 (https://static.360buyimg.com/item/assets/oldman/wza1/aria.js?appid=bfeaebea192374ec1f220455f8d5f952)
    at call (native)
    at o (https://static.360buyimg.com/item/assets/oldman/wza1/aria.js?appid=bfeaebea192374ec1f220455f8d5f952)
    at <anonymous> (https://static.360buyimg.com/item/assets/oldman/wza1/aria.js?appid=bfeaebea192374ec1f220455f8d5f952)
    at <eval> (https://static.360buyimg.com/item/assets/oldman/wza1/aria.js?appid=bfeaebea192374ec1f220455f8d5f952)
EXCEPTION x1: SyntaxError: invalid UTF-8 sequence
    at https://dc.3.cn/category/get?&callback=getCategoryCallback:1
shot:     build-browser-complete-0913/evidence/live-after/jd.png

### control-example -- PAINTED
url:      http://example.com/
reported: CONTROL: the simplest page on the web, no JS, no CSS worth the name
verdict:  painted 8720 changed px in 1.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 559 bytes, 2.0s, 0 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 0 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 8720 (blank control ink 4256), ink 9184, colours 545, rich-tile proxy 8, bbox [152, 12, 1256, 764]
network:  1 requests, 1 connections dialled, 0 reused, 0 modules (0 failed)
shot:     build-browser-complete-0913/evidence/live-after/control-example.png

### control-wikipedia -- PAINTED
url:      https://en.wikipedia.org/wiki/Operating_system
reported: CONTROL: known to render since M13
verdict:  painted 79044 changed px in 12.1s, no JS exceptions, no subresource gap
host:     HTTP 200, 676107 bytes, 1.6s, 5 <script> (1 src), 26 <img>
document: 2 stylesheets, 1 script src, 3 inline script, 24 img, 2 preload, 0 font -> 4 mandatory subresources
pixels:   changed 79044 (blank control ink 4256), ink 20262, colours 1389, rich-tile proxy 39, bbox [152, 16, 1264, 764]
network:  7 requests, 2 connections dialled, 5 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 1
shot:     build-browser-complete-0913/evidence/live-after/control-wikipedia.png

### doubao -- PAINTED
url:      https://www.doubao.com/
reported: TypeError charAt of undefined, localised to webpack module 3454
verdict:  painted 12092 changed px in 7.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 536406 bytes, 0.8s, 15 <script> (4 src), 1 <img>, redirects: 302 -> https://www.doubao.com/chat/
document: 1 stylesheets, 4 script src, 9 inline script, 1 img, 3 preload, 0 font -> 6 mandatory subresources
pixels:   changed 12092 (blank control ink 4256), ink 10215, colours 565, rich-tile proxy 12, bbox [140, 12, 1260, 764]
network:  8 requests, 3 connections dialled, 6 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 2
shot:     build-browser-complete-0913/evidence/live-after/doubao.png
