# Site scoreboard speed-0913-after

commit 41d7a74e8, ISO build-browser-speed-0913/after/logit.iso (sha256:5e33a17a9acfce48), 6 sites (+1 controls), 1 run(s) each, 238 s wall

ERRORS 5, PAINTED 1

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
| apple              | ERRORS     |    7.9 |    28 |    22/28 |   0 |     514856 |      37/270 | HTTP 200 |
| baidu              | ERRORS     |    0.7 |    15 |     1/15 |   3 |      81048 |      42/494 | HTTP 200 |
| bilibili           | ERRORS     |    5.5 |     9 |      8/9 |   0 |     266340 |      49/372 | HTTP 200 |
| bing-search        | ERRORS     |    3.1 |    98 |    98/98 |   0 |      11804 |       10/67 | HTTP 200 |
| stripe             | ERRORS     |   17.6 |    78 |    78/78 |   0 |       8856 |      11/330 | HTTP 200 |
| weixin             | PAINTED    |    1.9 |     5 |      5/5 |   0 |     618572 |      24/152 | HTTP 200 |

CONTROLS -- harness health, not results:
| control            | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| control-example    | PAINTED    |    1.3 |     1 |      1/1 |   0 |       8752 |      19/109 | HTTP 200 |

## Detail

### apple -- ERRORS
url:      https://www.apple.com/
reported: 0 images; TypeError split of undefined
verdict:  painted 514856 changed px, with 1 resource failure diagnostics
host:     HTTP 200, 312779 bytes, 0.4s, 17 <script> (12 src), 100 <img>
document: 9 stylesheets, 12 script src, 1 inline script, 43 img, 0 preload, 0 font -> 22 mandatory subresources
pixels:   changed 514856 (blank control ink 4256), ink 419317, colours 692, rich-tile proxy 12, bbox [140, 12, 1264, 764]
network:  28 requests, 2 connections dialled, 26 reused, 6 modules (0 failed)
sub-resource failures (1):
    [browser] fetch failed (status 404) /wss/fonts?families=SF+Pro,v3|SF+Pro+Icons,v3: no error
non-executable <script> blocks skipped: 4
shot:     build-browser-speed-0913/evidence/live-after/apple.png

### baidu -- ERRORS
url:      https://www.baidu.com/
reported: opens but unusable; a redirect works host-side and not on the machine
verdict:  painted 81048 changed px in 0.7s but 3 JS exception(s)
host:     HTTP 200, 227 bytes, 0.1s, 1 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 1 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 81048 (blank control ink 4256), ink 8461, colours 788, rich-tile proxy 13, bbox [152, 12, 1264, 764]
network:  15 requests, 5 connections dialled, 10 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 5
EXCEPTION x1: TypeError: cannot read property 'ownerDocument' of undefined
    at <anonymous> (https://pss.bdstatic.com/static/superman/js/lib/jquery-1-edb203c114.10.2.js)
    at buildFragment (https://pss.bdstatic.com/static/superman/js/lib/jquery-1-edb203c114.10.2.js:200)
    at parseHTML (https://pss.bdstatic.com/static/superman/js/lib/jquery-1-edb203c114.10.2.js:14)
    at init (https://pss.bdstatic.com/static/superman/js/lib/jquery-1-edb203c114.10.2.js:5)
    at jQuery (https://pss.bdstatic.com/static/superman/js/lib/jquery-1-edb203c114.10.2.js)
    at <anonymous> (http://pss.bdstatic.com/r/www/cache/static/home/js/nu_instant_search_a4c21b9.js:122)
    at apply (native)
    at fire (https://pss.bdstatic.com/static/superman/js/lib/jquery-1-edb203c114.10.2.js:87)
EXCEPTION (timer/event): event listener: TypeError: cannot read property 'ownerDocument' of undefined
EXCEPTION (timer/event): timer: Error: [MODULE_TIMEOUT]Hang: none; Miss: plugins/bzPopper, superman-san/app/chat-input/result_954e738, superman/components/placeholder
shot:     build-browser-speed-0913/evidence/live-after/baidu.png

### bilibili -- ERRORS
url:      https://www.bilibili.com/
reported: TypeError split of undefined
verdict:  painted 266340 changed px, with 1 page-reported error(s) and 0 fetch error(s)
host:     HTTP 200, 117920 bytes, 0.2s, 21 <script> (8 src), 13 <img>
document: 1 stylesheets, 6 script src, 12 inline script, 12 img, 1 preload, 0 font -> 8 mandatory subresources
pixels:   changed 266340 (blank control ink 4256), ink 178801, colours 15884, rich-tile proxy 784, bbox [140, 12, 1264, 764]
network:  9 requests, 3 connections dialled, 6 reused, 0 modules (0 failed)
shot:     build-browser-speed-0913/evidence/live-after/bilibili.png

### bing-search -- ERRORS
url:      https://www.bing.com/search?q=python
reported: "我使用bing搜索python成功返回了结果!!!可是......CSS渲染根本无法正常使用"
verdict:  painted 11804 changed px, with 0 page-reported error(s) and 1 fetch error(s)
host:     HTTP 200, 97751 bytes, 0.4s, 20 <script> (6 src), 3 <img>, redirects: 302 -> https://cn.bing.com/search?q=python
document: 91 stylesheets, 6 script src, 14 inline script, 1 img, 4 preload, 0 font -> 98 mandatory subresources
pixels:   changed 11804 (blank control ink 4256), ink 5295, colours 523, rich-tile proxy 8, bbox [140, 12, 1264, 764]
network:  98 requests, 4 connections dialled, 95 reused, 0 modules (0 failed)
shot:     build-browser-speed-0913/evidence/live-after/bing-search.png

### stripe -- ERRORS
url:      https://stripe.com/
reported: 73 scripts, extremely slow
verdict:  painted 8856 changed px, with 16 page-reported error(s) and 0 fetch error(s)
host:     HTTP 200, 720872 bytes, 2.2s, 76 <script> (73 src), 29 <img>, redirects: 307 -> https://stripe.com/jp
document: 5 stylesheets, 72 script src, 0 inline script, 29 img, 13 preload, 2 font -> 78 mandatory subresources
pixels:   changed 8856 (blank control ink 4256), ink 6647, colours 675, rich-tile proxy 8, bbox [152, 12, 1264, 764]
network:  78 requests, 2 connections dialled, 77 reused, 0 modules (0 failed)
sub-resource failures (3):
    [img] fetch failed (status 0): https://q.stripe.com?cid=e9cfa891-e90c-2968-9c1a-bc6ca8deee61&lsid=e9cfa891-e90c-2968-9c1a-bc6ca8deee61&analytics_ua=analytics.js-CURRENT_VERSION&page=%2Fjp&referrer=&domain=stripe: cookie header exceeds capacity
    [img] fetch failed (status 0): https://q.stripe.com?cid=e9cfa891-e90c-2968-9c1a-bc6ca8deee61&lsid=e9cfa891-e90c-2968-9c1a-bc6ca8deee61&analytics_ua=analytics.js-CURRENT_VERSION&page=%2Fjp&referrer=&domain=stripe: cookie header exceeds capacity
    [img] fetch failed (status 0): https://q.stripe.com?cid=e9cfa891-e90c-2968-9c1a-bc6ca8deee61&lsid=e9cfa891-e90c-2968-9c1a-bc6ca8deee61&analytics_ua=analytics.js-CURRENT_VERSION&page=%2Fjp&referrer=&domain=stripe: cookie header exceeds capacity
non-executable <script> blocks skipped: 3
shot:     build-browser-speed-0913/evidence/live-after/stripe.png

### control-example -- PAINTED
url:      http://example.com/
reported: CONTROL: the simplest page on the web, no JS, no CSS worth the name
verdict:  painted 8752 changed px in 1.3s, no JS exceptions, no subresource gap
host:     HTTP 200, 559 bytes, 0.8s, 0 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 0 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 8752 (blank control ink 4256), ink 9184, colours 545, rich-tile proxy 8, bbox [152, 12, 1264, 764]
network:  1 requests, 1 connections dialled, 0 reused, 0 modules (0 failed)
shot:     build-browser-speed-0913/evidence/live-after/control-example.png

### weixin -- PAINTED
url:      https://weixin.qq.com/
reported: same as douyin -- nothing renders
verdict:  painted 618572 changed px in 1.9s, no JS exceptions, no subresource gap
host:     HTTP 200, 112625 bytes, 0.1s, 10 <script> (5 src), 0 <img>
document: 1 stylesheets, 3 script src, 3 inline script, 0 img, 5 preload, 0 font -> 5 mandatory subresources
pixels:   changed 618572 (blank control ink 4256), ink 4256, colours 576, rich-tile proxy 19, bbox [140, 12, 1260, 764]
network:  5 requests, 4 connections dialled, 1 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 1
shot:     build-browser-speed-0913/evidence/live-after/weixin.png
