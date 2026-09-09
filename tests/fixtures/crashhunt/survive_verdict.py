#!/usr/bin/env python3
"""Verdict helper for test-crashhunt-survive. Args: <canvas.json> <map.json>

Both scenarios must end SURVIVED, and the record must say the scenario page
actually RAN (the DONE marker note) -- a survival verdict over a page that
never loaded is a green nothing, which is the qmp_site.py lesson restated:
the harness must prove the measurement happened before any verdict counts.
"""

import json
import sys

fails = []
for name, path in (("canvas", sys.argv[1]), ("map", sys.argv[2])):
    rec = json.load(open(path))
    if rec.get("verdict") != "SURVIVED":
        fails.append("%s: verdict %r (%s)" % (name, rec.get("verdict"), rec.get("why")))
    if "DONE" not in (rec.get("scenario_note") or ""):
        fails.append("%s: scenario_note %r has no DONE -- the page never finished, "
                     "so 'SURVIVED' would not mean 'survived the stress'"
                     % (name, rec.get("scenario_note")))

if fails:
    for f in fails:
        print("FAIL: " + f)
    sys.exit(1)
print("survive ratchet: canvas and map both ran to DONE and SURVIVED")
sys.exit(0)
