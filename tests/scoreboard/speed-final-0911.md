# Site scoreboard speed-final-0911

commit 41d7a74e8, ISO build-browser-speed-0911/final/logit.iso (sha256:89466926e7e34e11), 3 sites (+1 controls), 2 run(s) each, 632 s wall

ERRORS 2, PAINTED 1

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
| github             | ERRORS     |   29.8 |    90 |    27/90 |   0 |     579620 |      55/321 | HTTP 200 |
| python             | ERRORS     |    8.5 |    13 |  15/13 ! |   0 |     619708 |     136/589 | HTTP 200 |
| deepseek           | PAINTED    |    2.5 |    19 |    19/19 |   0 |     326280 |      42/245 | HTTP 200 |

CONTROLS -- harness health, not results:
| control            | verdict    | load s |  reqs | asked/got | exc | changed px |  text run/B | host     |
|--------------------|------------|--------|-------|----------|-----|------------|-------------|----------|
| control-wikipedia  | FLAKY      |   14.6 |     5 |      4/5 |   0 |      46468 |    186/1040 | HTTP 200 |

## Detail

### github -- ERRORS
url:      https://github.com/
reported: "很奇怪"
verdict:  painted 579620 changed px, with 0 page-reported error(s) and 4 fetch error(s)
host:     HTTP 200, 577388 bytes, 2.7s, 14 <script> (8 src), 24 <img>
document: 18 stylesheets, 8 script src, 0 inline script, 22 img, 77 preload, 1 font -> 27 mandatory subresources
pixels:   changed 579620 (blank control ink 4119), ink 384212, colours 1523, rich-tile proxy 33, bbox [112, 12, 1264, 736]
network:  90 requests, 2 connections dialled, 88 reused, 62 modules (0 failed)
sub-resource failures (4):
    [img] fetch failed (status 0): //images.ctfassets.net/8aevphvgewt8/5suLHnkyPm3laA8qENQmMZ/6b267a9e449ae94968dd85c8f96fb64e/accordion-1-38ad6b6d1b20.webp: stalled: no response
    [img] fetch failed (status 0): //images.ctfassets.net/8aevphvgewt8/20DkYnWbPsbb8mdDnIs32f/a43850b7e0e8e45ec87761d1ba91e19b/accordion-2-c0a62cfc31a1.webp: stalled: no response
    [img] fetch failed (status 0): //images.ctfassets.net/8aevphvgewt8/39mo3IRGhK995HzuwZANhY/cabfd393551e0d2aae92bc105121ca9c/accordion-3-5d5d222f1830.webp: stalled: no response
    [img] fetch failed (status 0): //images.ctfassets.net/8aevphvgewt8/7GOAeyecyEQqY12hHwvwXg/69877012458c9d42916b2a18e0e36a37/accordion-4-7abff9233556.webp: stalled: no response
non-executable <script> blocks skipped: 6
shot:     build-browser-speed-0911/evidence/live-final/github.png

### python -- ERRORS
url:      https://www.python.org/
reported: (new 2026-08-28: user reports it does not open)
verdict:  painted 619708 changed px, with 1 page-reported error(s) and 0 fetch error(s) -- and the document asked for 15 subresources (4 stylesheets, 10 script srcs) against 13 requests issued, short by 2
host:     HTTP 200, 53119 bytes, 5.6s, 13 <script> (10 src), 1 <img>
document: 4 stylesheets, 10 script src, 2 inline script, 1 img, 2 preload, 0 font -> 15 mandatory subresources
GAP:      the guest issued 13 requests against 15 mandatory -- SHORT BY 2. A request never made cannot appear in fetch_failed; this is the only column that sees it.
pixels:   changed 619708 (blank control ink 4119), ink 495078, colours 1735, rich-tile proxy 41, bbox [112, 16, 1264, 736]
network:  13 requests, 5 connections dialled, 8 reused, 0 modules (0 failed)
non-executable <script> blocks skipped: 1
shot:     build-browser-speed-0911/evidence/live-final/python.png

### control-wikipedia -- FLAKY
url:      https://en.wikipedia.org/wiki/Operating_system
reported: CONTROL: known to render since M13
verdict:  verdicts disagreed across 2 measured run(s): ERRORS, PAINTED
runs:     ERRORS, PAINTED
host:     HTTP 200, 676052 bytes, 3.3s, 5 <script> (1 src), 26 <img>
document: 2 stylesheets, 1 script src, 3 inline script, 24 img, 2 preload, 0 font -> 4 mandatory subresources
pixels:   changed 46468 (blank control ink 4119), ink 15848, colours 1242, rich-tile proxy 25, bbox [124, 16, 1260, 736]
network:  5 requests, 2 connections dialled, 3 reused, 0 modules (0 failed)
sub-resource failures (1):
    [img] fetch failed (status 0): //thumb.wikimedia.org/wikipedia/en/thumb/1/1b/Semi-protection-shackle.svg/20px-Semi-protection-shackle.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: TLS refused: handshake or certificate verification failed
non-executable <script> blocks skipped: 1
shot:     build-browser-speed-0911/evidence/live-final/control-wikipedia.png

### deepseek -- PAINTED
url:      https://www.deepseek.com/
reported: blank; needs Worker
verdict:  painted 326280 changed px in 2.5s, no JS exceptions, no subresource gap
host:     HTTP 200, 90207 bytes, 0.1s, 37 <script> (16 src), 3 <img>
document: 3 stylesheets, 15 script src, 21 inline script, 3 img, 3 preload, 0 font -> 19 mandatory subresources
pixels:   changed 326280 (blank control ink 4119), ink 24523, colours 1489, rich-tile proxy 53, bbox [112, 12, 1264, 736]
network:  19 requests, 0 connections dialled, 1 reused, 0 modules (0 failed)
shot:     build-browser-speed-0911/evidence/live-final/deepseek.png
