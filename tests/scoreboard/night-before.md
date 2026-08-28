# Site scoreboard night-before

commit d367cfb1c, ISO build/logit.iso (sha256:d1dab312940643bd), 4 sites (+2 controls), 1 run(s) each, 407 s wall

ERRORS 2, GAP 1, PAINTED 1

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
| github             | ERRORS     |   90.5 |    94 |    28/94 |   1 |     203708 |        3/14 | HTTP 200 |
| google-search      | ERRORS     |    2.5 |     3 |      1/3 |   1 |      16204 |      41/271 | HTTP 200 |
| bing-search        | GAP        |    5.5 |    95 |  96/95 ! |   0 |      13020 |        9/60 | HTTP 200 |
| deepseek           | PAINTED    |    3.7 |    22 |    22/22 |   0 |      17144 |         0/0 | HTTP 200 |

CONTROLS -- harness health, not results:
| control            | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| control-example    | PAINTED    |    0.7 |     1 |      1/1 |   0 |       8820 |      19/109 | HTTP 200 |
| control-wikipedia  | PAINTED    |   16.4 |    35 |     4/35 |   0 |      82580 |    207/1176 | HTTP 200 |

## Detail

### github -- ERRORS
url:      https://github.com/
reported: "很奇怪"
verdict:  painted 203708 changed px in 90.5s but 1 JS exception(s)
host:     HTTP 200, 573761 bytes, 1.2s, 15 <script> (9 src), 24 <img>
document: 18 stylesheets, 9 script src, 0 inline script, 22 img, 77 preload, 1 font -> 28 mandatory subresources
pixels:   changed 203708 (blank control ink 4256), ink 186122, colours 400, rich-tile proxy 8, bbox [140, 12, 1264, 764]
network:  94 requests, 5 connections dialled, 89 reused, 59 modules (0 failed)
non-executable <script> blocks skipped: 6
EXCEPTION (module): https://github.githubassets.com/assets/behaviors-0201f5326d68156e.js: TypeError: not an object
shot:     tests/scoreboard/night-before/github.png

### google-search -- ERRORS
url:      https://www.google.com/search?q=python
reported: "只要搜索入一个 Paton" -- the homepage renders; this is the path the owner actually uses
verdict:  painted 16204 changed px in 2.5s but 1 JS exception(s)
host:     HTTP 200, 90987 bytes, 0.6s, 5 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 5 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 16204 (blank control ink 4256), ink 11024, colours 642, rich-tile proxy 17, bbox [152, 16, 1264, 764]
network:  3 requests, 2 connections dialled, 2 reused, 0 modules (0 failed)
EXCEPTION (timer/event): event listener: ReferenceError: 'solveSimpleChallenge' is not defined
shot:     tests/scoreboard/night-before/google-search.png

### bing-search -- GAP
url:      https://www.bing.com/search?q=python
reported: "我使用bing搜索python成功返回了结果!!!可是......CSS渲染根本无法正常使用"
verdict:  painted 13020 changed px in 5.5s and threw nothing, but never requested 1 of the 96 subresources the document asks for (89 stylesheets in the document)
host:     HTTP 200, 99045 bytes, 0.6s, 20 <script> (6 src), 3 <img>, redirects: 302 -> https://cn.bing.com/search?q=python
document: 89 stylesheets, 6 script src, 14 inline script, 1 img, 4 preload, 0 font -> 96 mandatory subresources
GAP:      the guest issued 95 requests against 96 mandatory -- SHORT BY 1. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 13020 (blank control ink 4256), ink 4776, colours 510, rich-tile proxy 10, bbox [140, 16, 1260, 764]
network:  95 requests, 5 connections dialled, 91 reused, 0 modules (0 failed)
shot:     tests/scoreboard/night-before/bing-search.png

### control-example -- PAINTED
url:      http://example.com/
reported: CONTROL: the simplest page on the web, no JS, no CSS worth the name
verdict:  painted 8820 changed px in 0.7s, no JS exceptions, no subresource gap
host:     HTTP 200, 559 bytes, 0.4s, 0 <script> (0 src), 0 <img>
document: 0 stylesheets, 0 script src, 0 inline script, 0 img, 0 preload, 0 font -> 1 mandatory subresources
pixels:   changed 8820 (blank control ink 4256), ink 9184, colours 545, rich-tile proxy 9, bbox [140, 12, 1264, 764]
network:  1 requests, 1 connections dialled, 0 reused, 0 modules (0 failed)
shot:     tests/scoreboard/night-before/control-example.png

### control-wikipedia -- PAINTED
url:      https://en.wikipedia.org/wiki/Operating_system
reported: CONTROL: known to render since M13
verdict:  painted 82580 changed px in 16.4s, no JS exceptions, no subresource gap
host:     HTTP 200, 676093 bytes, 1.9s, 5 <script> (1 src), 26 <img>
document: 2 stylesheets, 1 script src, 3 inline script, 24 img, 2 preload, 0 font -> 4 mandatory subresources
pixels:   changed 82580 (blank control ink 4256), ink 20391, colours 1362, rich-tile proxy 52, bbox [152, 12, 1264, 764]
network:  35 requests, 4 connections dialled, 31 reused, 0 modules (0 failed)
sub-resource failures (8):
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/9/96/Symbol_category_class.svg/20px-Symbol_category_class.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/8/8a/OOjs_UI_icon_edit-ltr-progressive.svg/20px-OOjs_UI_icon_edit-ltr-progressive.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/1/1b/Semi-protection-shackle.svg/20px-Semi-protection-shackle.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/d/db/Diagram_of_a_security_descriptor_for_a_file_on_Windows.png/330px-Diagram_of_a_security_descriptor_for_a_file_on_Windows.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/9/99/Wiktionary-logo-en-v2.svg/40px-Wiktionary-logo-en-v2.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/4/4a/Commons-logo.svg/40px-Commons-logo.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/4/41/Global_thinking.svg/20px-Global_thinking.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/e/e0/Symbol_question.svg/20px-Symbol_question.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
non-executable <script> blocks skipped: 1
shot:     tests/scoreboard/night-before/control-wikipedia.png

### deepseek -- PAINTED
url:      https://www.deepseek.com/
reported: blank; needs Worker
verdict:  painted 17144 changed px in 3.7s, no JS exceptions, no subresource gap
host:     HTTP 200, 86879 bytes, 0.2s, 42 <script> (19 src), 2 <img>
document: 3 stylesheets, 18 script src, 23 inline script, 2 img, 3 preload, 0 font -> 22 mandatory subresources
pixels:   changed 17144 (blank control ink 4256), ink 14145, colours 637, rich-tile proxy 14, bbox [140, 16, 1264, 768]
network:  22 requests, 2 connections dialled, 20 reused, 0 modules (0 failed)
shot:     tests/scoreboard/night-before/deepseek.png
