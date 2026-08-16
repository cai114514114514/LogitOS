# Site scoreboard im-after

commit ea47b7d39, ISO build/logit.iso (sha256:4d0ae54bc7b6d74b), 0 sites (+1 controls), 1 run(s) each, 53 s wall



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
| control-wikipedia  | PAINTED    |   15.1 |    37 |     4/37 |   0 |      91108 | HTTP 200 |

## Detail

### control-wikipedia -- PAINTED
url:      https://en.wikipedia.org/wiki/Operating_system
reported: CONTROL: known to render since M13
verdict:  painted 91108 changed px in 15.1s, no JS exceptions, no subresource gap
host:     HTTP 200, 675411 bytes, 0.7s, 5 <script> (1 src), 26 <img>
document: 2 stylesheets, 1 script src, 3 inline script, 24 img, 2 preload, 0 font -> 4 mandatory subresources
pixels:   changed 91108 (blank control ink 3902), ink 19905, colours 1583, rich-tile proxy 57, bbox [152, 0, 1264, 764]
network:  37 requests, 4 connections dialled, 33 reused, 0 modules (0 failed)
sub-resource failures (10):
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/6/6e/Virtual_memory.svg/250px-Virtual_memory.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/5/51/Dolphin_FileManager.png/250px-Dolphin_FileManager.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/8/8a/OOjs_UI_icon_edit-ltr-progressive.svg/20px-OOjs_UI_icon_edit-ltr-progressive.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/1/1b/Semi-protection-shackle.svg/20px-Semi-protection-shackle.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/d/d4/Layers_of_a_Linux_system.png/500px-Layers_of_a_Linux_system.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/d/db/Diagram_of_a_security_descriptor_for_a_file_on_Windows.png/330px-Diagram_of_a_security_descriptor_for_a_file_on_Windows.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/en/thumb/4/4a/Commons-logo.svg/40px-Commons-logo.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
    fetch failed (status 429) //upload.wikimedia.org/wikipedia/commons/thumb/4/41/Global_thinking.svg/20px-Global_thinking.svg.png?utm_source=en.wikipedia.org&utm_campaign=parser&utm_content=thumbnail: no error
non-executable <script> blocks skipped: 1
shot:     tests/scoreboard/im-after/control-wikipedia.png
