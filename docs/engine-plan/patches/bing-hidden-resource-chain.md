# Bing results disappearing: saved-source resource chain

2026-09-09. Read-only mechanism analysis; no production change or new guest run.

The saved response contains a concrete inline-hide/external-restore dependency.
This strengthens the earlier observation that results disappear after scripts,
but does **not** establish the sole dynamic cause in the latest guest. The
earlier resource-failure counts alone did not prove that cause.

## Saved source

Offsets below are zero-based Unicode character offsets after Python
`Path.read_text(errors='replace')`, not UTF-8 byte offsets. Paths are relative
to the repository root; snippets are deliberately shorter than minified lines.

`build/site-general/after/bing-search/bing-search.host.html`:

- Line 11, char 27147: the BM EVT compute callback tests
  `sj_cook.get("_SS","fldcp")`. Unless that value is `"1"`, it writes HV/HVE
  cookies and hides the content container.
- Line 11, char 27438: `t.style.visibility="hidden"`, where the preceding
  assignment is `t=document.getElementById("b_content")`.
- Line 11, char 27900: `n.wireup(t,{load:f,compute:e,unload:o})` registers this
  callback as EVT compute. The local `BM.trigger` calls registered computes.
- Line 23, char 35287: `BMTrigger.execute();` invokes that trigger through
  rAF/setTimeout when its feature checks pass, or directly otherwise.
- The original `b_content` and `b_results` elements have no inline hiding
  attributes. Their saved ordinary layout rules do not provide an equally
  direct root-container opacity/display/contain hiding explanation. The
  `visibility:hidden` clearfix rule applies to generated pseudo content.

`build/site-general/bing-search/script-audit/external-1.js`, line 1:

- Char 239: `BD:{basic:1,e:1}` enables its basic BD path.
- Char 22079: `id:"BD"` identifies the registered load callback. It returns
  early for `_SS.fldcp == "1"`; otherwise it installs event handling and reads
  the HVE cookie in this configuration. With a nonempty cookie, a query URL,
  and no `&rdr=1`, it requests an `&rdr=1` navigation. Its alternative restores
  the content container.
- Char 22660: `d.style.visibility="visible"`, where `d` is obtained from
  `document.getElementById("b_content")`.

Thus the external branch may restore visibility **or redirect**, depending on
state; saying it always restores visibility would omit a material condition.

## URL match and latest guest observation

`build/site-general/bing-search/script-audit/manifest.json` maps external-1.js
to `https://cn.bing.com/rp/YK8uLbZEp9NCx4w0-d6FhjwCZxw.gz.js`. The saved HTML
also references this resource immediately after the inline BM setup.

In `build/site-general/final-sites/bing-native-deferred/serial.txt`:

- Line 1121: results paint **72 text runs / 423 bytes**.
- Line 1225: that exact external URL is `script LOST`, with the reported
  reason `the kernel socket table is full`.
- Line 1234: guest load stages include scripts execution and lifecycle settle;
  total load measurement is 7490 ms.
- Line 1241: the subsequent paint contains **9 runs / 60 bytes**, matching
  the header-only `final.png`. `search-029.png` shows styled Python results
  before they disappear. No crash was observed in this replay.

The concrete candidate is: the inline callback hides content, while the
external restore/redirect branch cannot run because its resource is missing.
The HTML/script bodies inspected here were saved from earlier host fetches,
not extracted byte-for-byte from this final guest response. This analysis did
not trace the actual setter call, inspect the guest cookie values, or prove
that delivering this script alone restores the final page. Other missing
resources or runtime failures may also contribute. Do not promote this to a
unique dynamically proven root cause or call the results page compatible.

## Handoff constraints

The user confirmed another group is actively changing networking. Preserve
their `browser_rt` and `bfetch` work; this handoff changes neither. Investigate
the generic resource-delivery failure with that group. Do not force visibility,
fabricate HVE cookies or `rdr` state, spoof browser signals, or add a site branch
to make this page appear successful. No host or guest DOM was altered for this
analysis, and no new fixture/gate or guest test is claimed.
