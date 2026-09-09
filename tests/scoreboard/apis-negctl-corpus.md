# Site scoreboard apis-negctl-corpus

commit 9ba575e93, ISO build-apis/logit.iso (sha256:82ec5e3a8dc5be9e), 4 sites (+1 controls), 1 run(s) each, 84 s wall

HARNESS 1, ERRORS 1, PAINTED 2

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
| stripe             | HARNESS    |      - |     - |        - |   - |          - |           - | HTTP 200 |
| kimi               | ERRORS     |   40.3 |   112 |   32/112 |   1 |      11312 |        5/59 | HTTP 200 |
| bilibili           | PAINTED    |    9.7 |    20 |     8/20 |   0 |     267896 |      55/436 | HTTP 200 |
| qq                 | PAINTED    |   15.5 |    16 |     8/16 |   0 |     243904 |     71/1138 | HTTP 200 |

CONTROLS -- harness health, not results:
| control            | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| control-wikipedia  | PAINTED    |   21.2 |    36 |     4/36 |   0 |      82588 |    207/1176 | HTTP 200 |

## Detail

### stripe -- HARNESS
url:      https://stripe.com/
reported: 73 scripts, extremely slow
verdict:  the URL never reached the address bar intact (typed 'https://stripe.com/', got [''])
host:     HTTP 200, 741414 bytes, 2.2s, 76 <script> (73 src), 30 <img>, redirects: 307 -> https://stripe.com/jp

### kimi -- ERRORS
url:      https://www.kimi.com/
reported: the one the whole browser arc was aimed at -- an LLM chat page: React, streaming fetch, and a text input that has to accept keystrokes
verdict:  painted 11312 changed px in 40.3s but 1 JS exception(s)
host:     HTTP 200, 483424 bytes, 0.4s, 4 <script> (2 src), 2 <img>
document: 29 stylesheets, 2 script src, 2 inline script, 2 img, 81 preload, 0 font -> 32 mandatory subresources
pixels:   changed 11312 (blank control ink 4256), ink 8624, colours 580, rich-tile proxy 14, bbox [144, 12, 1264, 764]
network:  112 requests, 5 connections dialled, 107 reused, 78 modules (0 failed)
EXCEPTION x1: TypeError: cannot read property 'caller' of undefined
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496672)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496672)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496672)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496672)
    at <anonymous> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496672)
    at <eval> (https://static.trustdecision.com/tdfp/cn/1adb3bba719793fb6874417a286e900d/fm.js?t=496672:4)
shot:     tests/scoreboard/apis-negctl-corpus/kimi.png

### bilibili -- PAINTED
url:      https://www.bilibili.com/
reported: TypeError split of undefined
verdict:  painted 267896 changed px in 9.7s, no JS exceptions, no subresource gap
host:     HTTP 200, 116763 bytes, 0.2s, 21 <script> (8 src), 13 <img>
document: 1 stylesheets, 6 script src, 12 inline script, 12 img, 1 preload, 0 font -> 8 mandatory subresources
pixels:   changed 267896 (blank control ink 4256), ink 83370, colours 7749, rich-tile proxy 383, bbox [140, 16, 1264, 764]
network:  20 requests, 8 connections dialled, 12 reused, 0 modules (0 failed)
shot:     tests/scoreboard/apis-negctl-corpus/bilibili.png

### control-wikipedia -- PAINTED
url:      https://en.wikipedia.org/wiki/Operating_system
reported: CONTROL: known to render since M13
verdict:  painted 82588 changed px in 21.2s, no JS exceptions, no subresource gap
host:     HTTP 200, 676098 bytes, 1.6s, 5 <script> (1 src), 26 <img>
document: 2 stylesheets, 1 script src, 3 inline script, 24 img, 2 preload, 0 font -> 4 mandatory subresources
pixels:   changed 82588 (blank control ink 4256), ink 20391, colours 1362, rich-tile proxy 52, bbox [152, 12, 1264, 764]
network:  36 requests, 4 connections dialled, 32 reused, 0 modules (0 failed)
sub-resource failures (9):
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/6/6e/Virtual_memory.svg/250px-Virtual_memory.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/4/41/Global_thinking.svg/20px-Global_thinking.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/5/51/Dolphin_FileManager.png/250px-Dolphin_FileManager.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/9/96/Symbol_category_class.svg/20px-Symbol_category_class.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/9/99/Wiktionary-logo-en-v2.svg/40px-Wiktionary-logo-en-v2.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/1/1b/Semi-protection-shackle.svg/20px-Semi-protection-shackle.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/0/0b/Wikiversity_logo_2017.svg/40px-Wikiversity_logo_2017.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/e/e0/Symbol_question.svg/20px-Symbol_question.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
non-executable <script> blocks skipped: 1
shot:     tests/scoreboard/apis-negctl-corpus/control-wikipedia.png

### qq -- PAINTED
url:      https://www.qq.com/
reported: Uncaught (in promise) undefined
verdict:  painted 243904 changed px in 15.5s, no JS exceptions, no subresource gap
host:     HTTP 200, 126971 bytes, 0.3s, 13 <script> (7 src), 17 <img>
document: 1 stylesheets, 6 script src, 6 inline script, 16 img, 0 preload, 0 font -> 8 mandatory subresources
pixels:   changed 243904 (blank control ink 4256), ink 148384, colours 1049, rich-tile proxy 45, bbox [140, 12, 1264, 764]
network:  16 requests, 7 connections dialled, 9 reused, 0 modules (0 failed)
shot:     tests/scoreboard/apis-negctl-corpus/qq.png
