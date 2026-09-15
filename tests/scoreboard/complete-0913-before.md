# Site scoreboard complete-0913-before

commit 41d7a74e8, ISO build-browser-complete-0913/before/logit.iso (sha256:8cb72e1276ff6065), 10 sites (+2 controls), 1 run(s) each, 420 s wall

BLANK 2, ERRORS 6, PAINTED 2

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
| openai             | BLANK      |    2.5 |     2 |  125/2 ! |   2 |       3040 |           - | HTTP 200 |
| qwen               | BLANK      |   14.6 |    22 |     6/22 |   1 |       2892 |           - | HTTP 200 |
| 2345               | ERRORS     |    4.9 |    20 |    18/20 |   1 |      45212 |     100/949 | HTTP 200 |
| anthropic          | ERRORS     |   18.2 |    13 |    12/13 |   1 |     324180 |      76/382 | HTTP 200 |
| douyin             | ERRORS     |    1.3 |    32 |     1/32 |   0 |      18468 |      31/213 | HTTP 200 |
| jd                 | ERRORS     |   51.9 |    17 |  18/17 ! |   6 |      14904 |       6/114 | HTTP 200 |
| kimi               | ERRORS     |   27.2 |   127 |   33/127 |   0 |      11424 |      33/362 | HTTP 200 |
| qq                 | ERRORS     |    7.3 |     9 |      8/9 |   0 |     107268 |     77/1150 | HTTP 200 |
| doubao             | PAINTED    |    6.7 |     7 |      6/7 |   0 |      11968 |        9/87 | HTTP 200 |
| google             | PAINTED    |    3.7 |     3 |      2/3 |   0 |      20552 |      17/147 | HTTP 200 |

CONTROLS -- harness health, not results:
| control            | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| control-example    | PAINTED    |    1.3 |     1 |      1/1 |   0 |       8832 |      19/109 | HTTP 200 |
| control-wikipedia  | PAINTED    |   10.9 |     5 |      4/5 |   0 |      79040 |    186/1040 | HTTP 200 |

## Detail

### openai -- BLANK
url:      https://openai.com/
reported: an icon and nothing else
verdict:  loaded in 2.5s and painted 3040 changed pixels (2 exceptions) -- and the document asked for 125 subresources (29 stylesheets, 95 script srcs) against 2 requests issued, short by 123
host:     HTTP 200, 448292 bytes, 5.2s, 104 <script> (96 src), 19 <img>
document: 29 stylesheets, 95 script src, 8 inline script, 18 img, 3 preload, 0 font -> 125 mandatory subresources
GAP:      the guest issued 2 requests against 125 mandatory -- SHORT BY 123. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 3040 (blank control ink 4256), ink 4256, colours 411, rich-tile proxy 9, bbox [152, 12, 1256, 764]
network:  2 requests, 2 connections dialled, 0 reused, 0 modules (0 failed)
EXCEPTION x1: SyntaxError: invalid escape sequence in regular expression
    at RegExp (native)
    at <anonymous> (https://challenges.cloudflare.com/turnstile/v0/g/330e41bb475c/api.js?onload=khCN8&render=explicit)
    at <anonymous> (https://challenges.cloudflare.com/turnstile/v0/g/330e41bb475c/api.js?onload=khCN8&render=explicit:2)
    at <eval> (https://challenges.cloudflare.com/turnstile/v0/g/330e41bb475c/api.js?onload=khCN8&render=explicit:3)
EXCEPTION (timer/event): timer: TypeError: jT is not a function (it is the string "toString")
shot:     build-browser-complete-0913/evidence/live-before/openai.png

### qwen -- BLANK
url:      https://chat.qwen.ai/
reported: hangs
verdict:  loaded in 14.6s and painted 2892 changed pixels (1 exceptions)
host:     HTTP 200, 101120 bytes, 0.3s, 17 <script> (3 src), 6 <img>
document: 2 stylesheets, 3 script src, 14 inline script, 6 img, 7 preload, 0 font -> 6 mandatory subresources
pixels:   changed 2892 (blank control ink 4256), ink 4698, colours 432, rich-tile proxy 8, bbox [152, 12, 1264, 764]
network:  22 requests, 5 connections dialled, 17 reused, 15 modules (1 failed)
sub-resource failures (1):
    [img] fetch failed (status 0): http://127.0.0.1:31460/favicon.png: the connection was refused or the network is down
EXCEPTION (module): https://assets.alicdn.com/g/qwenweb/qwen-chat-fe/0.2.91/js/main.js: SyntaxError: invalid escape sequence in regular expression
shot:     build-browser-complete-0913/evidence/live-before/qwen.png

### 2345 -- ERRORS
url:      https://www.2345.com/
reported: TypeError toString of undefined
verdict:  painted 45212 changed px in 4.9s but 1 JS exception(s)
host:     HTTP 200, 170261 bytes, 0.2s, 19 <script> (12 src), 13 <img>
document: 6 stylesheets, 11 script src, 8 inline script, 13 img, 83 preload, 0 font -> 18 mandatory subresources
pixels:   changed 45212 (blank control ink 4256), ink 16306, colours 1090, rich-tile proxy 48, bbox [140, 12, 1264, 764]
network:  20 requests, 4 connections dialled, 14 reused, 0 modules (0 failed)
EXCEPTION x1: NotSupportedError: importing or adopting a node from a document created by new DOMParser().parseFromString() is not supported in this build
shot:     build-browser-complete-0913/evidence/live-before/2345.png

### anthropic -- ERRORS
url:      https://www.anthropic.com/
reported: "is not a function"
verdict:  painted 324180 changed px in 18.2s but 1 JS exception(s)
host:     HTTP 200, 174476 bytes, 2.6s, 19 <script> (10 src), 0 <img>
document: 1 stylesheets, 10 script src, 9 inline script, 0 img, 0 preload, 0 font -> 12 mandatory subresources
pixels:   changed 324180 (blank control ink 4256), ink 261388, colours 796, rich-tile proxy 11, bbox [140, 12, 1264, 764]
network:  13 requests, 6 connections dialled, 7 reused, 0 modules (0 failed)
EXCEPTION (timer/event): timer: ReferenceError: 'CSSStyleDeclaration' is not defined
shot:     build-browser-complete-0913/evidence/live-before/anthropic.png

### douyin -- ERRORS
url:      https://www.douyin.com/
reported: nothing renders at all
verdict:  painted 18468 changed px, with 0 page-reported error(s) and 1 fetch error(s)
host:     HTTP 200, 72914 bytes, 0.1s, 2 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 2 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 18468 (blank control ink 4256), ink 12323, colours 701, rich-tile proxy 17, bbox [140, 12, 1264, 764]
network:  32 requests, 11 connections dialled, 22 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 3
shot:     build-browser-complete-0913/evidence/live-before/douyin.png

### jd -- ERRORS
url:      https://www.jd.com/
reported: jQuery is not defined
verdict:  painted 14904 changed px in 51.9s but 6 JS exception(s) -- and the document asked for 18 subresources (3 stylesheets, 14 script srcs) against 17 requests issued, short by 1
host:     HTTP 200, 192415 bytes, 0.1s, 35 <script> (14 src), 12 <img>
document: 3 stylesheets, 14 script src, 21 inline script, 12 img, 4 preload, 0 font -> 18 mandatory subresources
GAP:      the guest issued 17 requests against 18 mandatory -- SHORT BY 1. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 14904 (blank control ink 4256), ink 5969, colours 595, rich-tile proxy 10, bbox [140, 12, 1256, 764]
network:  17 requests, 6 connections dialled, 11 reused, 0 modules (0 failed)
sub-resource failures (9):
    [img] fetch failed (status 0): //api.m.jd.com?functionId=pchome_cookie_check&appid=www-jd-com&cthr=1&source=pc-home: cookie header exceeds capacity
    [img] fetch failed (status 0): https://img12.360buyimg.com/img/jfs/t1/308913/22/4711/14224/6835f060Fae0d0a1e/c107eadb786b0977.png: the kernel socket table is full
    [img] fetch failed (status 0): https://img10.360buyimg.com/img/jfs/t1/411194/16/15153/11848/69ce6678Fe111dea3/0276072072349683.png: the kernel socket table is full
    [img] fetch failed (status 0): https://img14.360buyimg.com/img/jfs/t1/344109/5/19140/80205/6904abaeF596cea23/0327eb1c61609a58.png: the kernel socket table is full
    [img] fetch failed (status 0): https://mercury.jd.com/log.gif?t=exp_log.100000&m=UA-J2011-1&pin=-&uid=null&sid=null|null&cul=https%3A%2F%2Fwww.jd.com%2F&v=%7B%22t1%22%3A%22pc_homepage%22%2C%22t2%22%3A%22basic%22: the kernel socket table is full
    [img] fetch failed (status 0): https://mercury.jd.com/log.gif?t=exp_log.100000&m=UA-J2011-1&pin=-&uid=null&sid=null|null&cul=https%3A%2F%2Fwww.jd.com%2F&ref=&rm=1789274465400&v=%7B%22spm%22%3A%22a0636.b005255.c0: the kernel socket table is full
    [img] fetch failed (status 0): https://mercury.jd.com/log.gif?t=exp_log.100000&m=UA-J2011-1&pin=-&uid=null&sid=null|null&cul=https%3A%2F%2Fwww.jd.com%2F&ref=&rm=1789274465400&v=%7B%22spm%22%3A%22a0636.b005255.c0: the kernel socket table is full
    [img] fetch failed (status 0): https://mercury.jd.com/log.gif?t=exp_log.100000&m=UA-J2011-1&pin=-&uid=null&sid=null|null&cul=https%3A%2F%2Fwww.jd.com%2F&v=%7B%22t1%22%3A%22JD_Main%22%2C%22t2%22%3A%22LogoExpo%22%: the kernel socket table is full
EXCEPTION x1: TypeError: cannot read property 'replace' of undefined
    at s (https://wl.jd.com/wl.js)
    at Ie (https://wl.jd.com/wl.js)
    at bloading (https://wl.jd.com/wl.js)
    at <anonymous> (https://wl.jd.com/wl.js)
    at <eval> (https://wl.jd.com/wl.js:2)
EXCEPTION x1: InternalError: interrupted
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
shot:     build-browser-complete-0913/evidence/live-before/jd.png

### kimi -- ERRORS
url:      https://www.kimi.com/
reported: the one the whole browser arc was aimed at -- an LLM chat page: React, streaming fetch, and a text input that has to accept keystrokes
verdict:  painted 11424 changed px, with 1 page-reported error(s) and 0 fetch error(s)
host:     HTTP 200, 477324 bytes, 0.2s, 4 <script> (2 src), 2 <img>
document: 30 stylesheets, 2 script src, 2 inline script, 2 img, 96 preload, 0 font -> 33 mandatory subresources
pixels:   changed 11424 (blank control ink 4256), ink 8033, colours 486, rich-tile proxy 9, bbox [144, 12, 1264, 764]
network:  127 requests, 3 connections dialled, 124 reused, 93 modules (0 failed)
shot:     build-browser-complete-0913/evidence/live-before/kimi.png

### qq -- ERRORS
url:      https://www.qq.com/
reported: Uncaught (in promise) undefined
verdict:  painted 107268 changed px, with 1 page-reported error(s) and 3 fetch error(s)
host:     HTTP 200, 133843 bytes, 0.2s, 13 <script> (7 src), 17 <img>
document: 1 stylesheets, 6 script src, 6 inline script, 16 img, 0 preload, 0 font -> 8 mandatory subresources
pixels:   changed 107268 (blank control ink 4256), ink 26902, colours 852, rich-tile proxy 116, bbox [140, 20, 1260, 764]
network:  9 requests, 3 connections dialled, 6 reused, 0 modules (0 failed)
shot:     build-browser-complete-0913/evidence/live-before/qq.png

### control-example -- PAINTED
url:      http://example.com/
reported: CONTROL: the simplest page on the web, no JS, no CSS worth the name
verdict:  painted 8832 changed px in 1.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 559 bytes, 0.9s, 0 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 0 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 8832 (blank control ink 4256), ink 9184, colours 545, rich-tile proxy 8, bbox [152, 12, 1264, 764]
network:  1 requests, 1 connections dialled, 0 reused, 0 modules (0 failed)
shot:     build-browser-complete-0913/evidence/live-before/control-example.png

### control-wikipedia -- PAINTED
url:      https://en.wikipedia.org/wiki/Operating_system
reported: CONTROL: known to render since M13
verdict:  painted 79040 changed px in 10.9s, no JS exceptions, no subresource gap
host:     HTTP 200, 676107 bytes, 2.6s, 5 <script> (1 src), 26 <img>
document: 2 stylesheets, 1 script src, 3 inline script, 24 img, 2 preload, 0 font -> 4 mandatory subresources
pixels:   changed 79040 (blank control ink 4256), ink 20262, colours 1389, rich-tile proxy 39, bbox [152, 20, 1260, 764]
network:  5 requests, 2 connections dialled, 3 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 1
shot:     build-browser-complete-0913/evidence/live-before/control-wikipedia.png

### doubao -- PAINTED
url:      https://www.doubao.com/
reported: TypeError charAt of undefined, localised to webpack module 3454
verdict:  painted 11968 changed px in 6.7s, no JS exceptions, no subresource gap
host:     HTTP 200, 536491 bytes, 1.1s, 15 <script> (4 src), 1 <img>, redirects: 302 -> https://www.doubao.com/chat/
document: 1 stylesheets, 4 script src, 9 inline script, 1 img, 3 preload, 0 font -> 6 mandatory subresources
pixels:   changed 11968 (blank control ink 4256), ink 10215, colours 565, rich-tile proxy 12, bbox [140, 12, 1264, 764]
network:  7 requests, 3 connections dialled, 5 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 2
shot:     build-browser-complete-0913/evidence/live-before/doubao.png

### google -- PAINTED
url:      https://www.google.com/
reported: "验证都无法加载出来" -- the bot check appears and its challenge will not render
verdict:  painted 20552 changed px in 3.7s, no JS exceptions, no subresource gap
host:     HTTP 200, 87829 bytes, 1.0s, 10 <script> (0 src), 1 <img>
document: 1 stylesheets, 0 script src, 10 inline script, 1 img, 0 preload, 0 font -> 2 mandatory subresources
pixels:   changed 20552 (blank control ink 4256), ink 5643, colours 715, rich-tile proxy 15, bbox [140, 16, 1264, 764]
network:  3 requests, 3 connections dialled, 0 reused, 0 modules (0 failed)
shot:     build-browser-complete-0913/evidence/live-before/google.png
