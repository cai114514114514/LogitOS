#!/usr/bin/env python3
"""Verdict helper for test-crashhunt-spin. Args: <spin.json> <spin.serial.txt>

Asserts, in order of how load-bearing they are:
  1. the watchdog BIT: `[js] watchdog: script exceeded its CPU slice` on the
     serial, with the wall-time rail named -- the 45 s constant (js_page.c:240
     JS_SLICE_MS_DEFAULT) doing its job on a 120 s spin.
  2. the browser SURVIVED: the driver's verdict ladder says SURVIVED (the
     about:text ping answered).
  3. the page survived: CH-SPIN-ALIVE-AFTER -- a timer registered BEFORE the
     bite kept firing after it (three ticks), which is the difference between
     "the entry was aborted" and "the page was".
  4. THE RATCHET: CH-SPIN-COMPLETED must NOT appear. That marker is printed
     only if the 120 s spin ran to the end unbitten, i.e. if the watchdog was
     disabled, removed, or sized past 120 s. Its presence is a green-looking
     regression of the owner's "long scripts die silently" protection, and
     this gate names it.
"""

import json
import sys

jpath, spath = sys.argv[1], sys.argv[2]
rec = json.load(open(jpath))
serial = open(spath, errors="replace").read()

fails = []

if "[js] watchdog: script exceeded its CPU slice (wall time) -- interrupted" not in serial:
    fails.append("the watchdog never bit the 120 s spin -- no "
                 "'[js] watchdog: script exceeded its CPU slice (wall time)' line")
if rec.get("verdict") != "SURVIVED":
    fails.append("verdict was %r, not SURVIVED -- the browser did not survive "
                 "the bite: %s" % (rec.get("verdict"), rec.get("why")))
if "CH-SPIN-ALIVE-AFTER" not in serial:
    fails.append("the page's pre-registered timer never fired after the bite "
                 "(no CH-SPIN-ALIVE-AFTER) -- the abort was more than the entry")
if "CH-SPIN-COMPLETED" in serial:
    fails.append("RATCHET: the 120 s spin ran to the end UNBITTEN "
                 "(CH-SPIN-COMPLETED present) -- the watchdog is gone or "
                 "sized past 120 s")

if fails:
    for f in fails:
        print("FAIL: " + f)
    sys.exit(1)

bitten = [ln for ln in serial.splitlines() if "since_begin_ms" in ln]
print("spin: watchdog bit a 120 s spin at %s; browser SURVIVED; page alive after"
      % (bitten[0].split("since_begin_ms=")[1].split()[0] if bitten else "?"))
sys.exit(0)
