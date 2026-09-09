# Site scoreboard apiprobe-guest-negctl

commit 9ba575e93, ISO build-apis/logit.iso (sha256:82ec5e3a8dc5be9e), 1 sites (+0 controls), 1 run(s) each, 46 s wall

PAINTED 1

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
| apiprobe           | PAINTED    |    0.7 |     1 |        - |   0 |      20160 |      13/153 | unreachable |

## Detail

### apiprobe -- PAINTED
url:      http://10.0.2.2:8747/
reported: LOCAL: guest capability probe for the three built items
verdict:  painted 20160 changed px in 0.7s, no JS exceptions, no subresource gap
host:     UNREACHABLE (RemoteDisconnected: Remote end closed connection without response)
pixels:   changed 20160 (blank control ink 4256), ink 19505, colours 604, rich-tile proxy 11, bbox [152, 12, 1264, 764]
network:  1 requests, 1 connections dialled, 0 reused, 0 modules (0 failed)
shot:     tests/scoreboard/apiprobe-guest-negctl/apiprobe.png
