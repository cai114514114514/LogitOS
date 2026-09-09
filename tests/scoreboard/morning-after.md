# Site scoreboard morning-after

commit d6d6a0ae5, ISO build/logit.iso (sha256:9da63eee1b542124), 4 sites (+2 controls), 1 run(s) each, 382 s wall

ERRORS 2, PAINTED 2

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
| github             | ERRORS     |   73.5 |    94 |    28/94 |   1 |     203604 |        3/14 | HTTP 200 |
| google-search      | ERRORS     |    1.9 |     3 |      1/3 |   1 |      17164 |      41/270 | HTTP 200 |
| bing-search        | PAINTED    |    6.1 |    97 |    95/97 |   0 |      13004 |        9/60 | HTTP 200 |
| deepseek           | PAINTED    |    3.7 |    22 |    22/22 |   0 |      10776 |       22/95 | HTTP 200 |

CONTROLS -- harness health, not results:
| control            | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| control-example    | PAINTED    |    0.7 |     1 |      1/1 |   0 |       8788 |      19/109 | HTTP 200 |
| control-wikipedia  | PAINTED    |   11.6 |    35 |     4/35 |   0 |      82612 |    207/1176 | HTTP 200 |

## Detail

### github -- ERRORS
url:      https://github.com/
reported: "很奇怪"
verdict:  painted 203604 changed px in 73.5s but 1 JS exception(s)
host:     HTTP 200, 573766 bytes, 0.7s, 15 <script> (9 src), 24 <img>
document: 18 stylesheets, 9 script src, 0 inline script, 22 img, 77 preload, 1 font -> 28 mandatory subresources
pixels:   changed 203604 (blank control ink 4256), ink 186122, colours 400, rich-tile proxy 8, bbox [140, 12, 1264, 764]
network:  94 requests, 5 connections dialled, 89 reused, 59 modules (0 failed)
non-executable <script> blocks skipped: 6
EXCEPTION (module): https://github.githubassets.com/assets/behaviors-5cd19426b9dcce3b.js: TypeError: not an object
shot:     tests/scoreboard/morning-after/github.png

### google-search -- ERRORS
url:      https://www.google.com/search?q=python
reported: "只要搜索入一个 Paton" -- the homepage renders; this is the path the owner actually uses
verdict:  painted 17164 changed px in 1.9s but 1 JS exception(s)
host:     HTTP 200, 90931 bytes, 0.4s, 5 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 5 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 17164 (blank control ink 4256), ink 11919, colours 648, rich-tile proxy 18, bbox [152, 16, 1264, 764]
network:  3 requests, 2 connections dialled, 2 reused, 0 modules (0 failed)
EXCEPTION (timer/event): event listener: ReferenceError: 'solveSimpleChallenge' is not defined
shot:     tests/scoreboard/morning-after/google-search.png

### bing-search -- PAINTED
url:      https://www.bing.com/search?q=python
reported: "我使用bing搜索python成功返回了结果!!!可是......CSS渲染根本无法正常使用"
verdict:  painted 13004 changed px in 6.1s, no JS exceptions, no subresource gap
host:     HTTP 200, 98338 bytes, 0.6s, 20 <script> (6 src), 3 <img>, redirects: 302 -> https://cn.bing.com/search?q=python
document: 88 stylesheets, 6 script src, 14 inline script, 1 img, 4 preload, 0 font -> 95 mandatory subresources
pixels:   changed 13004 (blank control ink 4256), ink 4776, colours 510, rich-tile proxy 10, bbox [140, 12, 1260, 764]
network:  97 requests, 5 connections dialled, 93 reused, 0 modules (0 failed)
shot:     tests/scoreboard/morning-after/bing-search.png

### control-example -- PAINTED
url:      http://example.com/
reported: CONTROL: the simplest page on the web, no JS, no CSS worth the name
verdict:  painted 8788 changed px in 0.7s, no JS exceptions, no subresource gap
host:     HTTP 200, 559 bytes, 0.2s, 0 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 0 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 8788 (blank control ink 4256), ink 9184, colours 545, rich-tile proxy 9, bbox [140, 12, 1264, 764]
network:  1 requests, 1 connections dialled, 0 reused, 0 modules (0 failed)
shot:     tests/scoreboard/morning-after/control-example.png

### control-wikipedia -- PAINTED
url:      https://en.wikipedia.org/wiki/Operating_system
reported: CONTROL: known to render since M13
verdict:  painted 82612 changed px in 11.6s, no JS exceptions, no subresource gap
host:     HTTP 200, 676093 bytes, 0.4s, 5 <script> (1 src), 26 <img>
document: 2 stylesheets, 1 script src, 3 inline script, 24 img, 2 preload, 0 font -> 4 mandatory subresources
pixels:   changed 82612 (blank control ink 4256), ink 20391, colours 1362, rich-tile proxy 52, bbox [152, 12, 1264, 764]
network:  35 requests, 4 connections dialled, 31 reused, 0 modules (0 failed)
sub-resource failures (8):
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/9/96/Symbol_category_class.svg/20px-Symbol_category_class.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/8/8a/OOjs_UI_icon_edit-ltr-progressive.svg/20px-OOjs_UI_icon_edit-ltr-progressive.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/1/1b/Semi-protection-shackle.svg/20px-Semi-protection-shackle.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/9/99/Wiktionary-logo-en-v2.svg/40px-Wiktionary-logo-en-v2.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/4/4a/Commons-logo.svg/40px-Commons-logo.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/0/0b/Wikiversity_logo_2017.svg/40px-Wikiversity_logo_2017.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/4/41/Global_thinking.svg/20px-Global_thinking.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/e/e0/Symbol_question.svg/20px-Symbol_question.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
non-executable <script> blocks skipped: 1
shot:     tests/scoreboard/morning-after/control-wikipedia.png

### deepseek -- PAINTED
url:      https://www.deepseek.com/
reported: blank; needs Worker
verdict:  painted 10776 changed px in 3.7s, no JS exceptions, no subresource gap
host:     HTTP 200, 86879 bytes, 0.1s, 42 <script> (19 src), 2 <img>
document: 3 stylesheets, 18 script src, 23 inline script, 2 img, 3 preload, 0 font -> 22 mandatory subresources
pixels:   changed 10776 (blank control ink 4256), ink 13958, colours 630, rich-tile proxy 14, bbox [140, 12, 1264, 768]
network:  22 requests, 2 connections dialled, 20 reused, 0 modules (0 failed)
shot:     tests/scoreboard/morning-after/deepseek.png
