# Site scoreboard im-before

commit ea47b7d39, ISO build/logit.iso (sha256:87fd259e29f855af), 0 sites (+1 controls), 1 run(s) each, 50 s wall



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

| site               | verdict    | load s |  reqs | asked/got | exc | changed px | host     |
|--------------------|------------|--------|-------|----------|-----|------------|----------|

CONTROLS -- harness health, not results:
| control            | verdict    | load s |  reqs | asked/got | exc | changed px | host     |
|--------------------|------------|--------|-------|----------|-----|------------|----------|
| control-wikipedia  | PAINTED    |   11.5 |    36 |     4/36 |   0 |      45508 | HTTP 200 |

## Detail

### control-wikipedia -- PAINTED
url:      https://en.wikipedia.org/wiki/Operating_system
reported: CONTROL: known to render since M13
verdict:  painted 45508 changed px in 11.5s, no JS exceptions, no subresource gap
host:     HTTP 200, 675411 bytes, 0.4s, 5 <script> (1 src), 26 <img>
document: 2 stylesheets, 1 script src, 3 inline script, 24 img, 2 preload, 0 font -> 4 mandatory subresources
pixels:   changed 45508 (blank control ink 3902), ink 15428, colours 1173, rich-tile proxy 32, bbox [152, 16, 1260, 764]
network:  36 requests, 4 connections dialled, 32 reused, 0 modules (0 failed)
sub-resource failures (10):
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/6/6e/Virtual_memory.svg/250px-Virtual_memory.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/5/51/Dolphin_FileManager.png/250px-Dolphin_FileManager.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/e/e0/Symbol_question.svg/20px-Symbol_question.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/9/96/Symbol_category_class.svg/20px-Symbol_category_class.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/d/d4/Layers_of_a_Linux_system.png/500px-Layers_of_a_Linux_system.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/d/db/Diagram_of_a_security_descriptor_for_a_file_on_Windows.png/330px-Diagram_of_a_security_descriptor_for_a_file_on_Windows.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/9/99/Wiktionary-logo-en-v2.svg/40px-Wiktionary-logo-en-v2.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/4/4a/Commons-logo.svg/40px-Commons-logo.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
non-executable <script> blocks skipped: 1
shot:     tests/scoreboard/im-before/control-wikipedia.png
