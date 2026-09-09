#!/usr/bin/env python3
# Verdict check for `make test-zaiblank-guest-negctl` (tests/zaiblank.mk).
# Extracted verbatim from the recipe heredoc for the same reason as
# zaiblank_guest_check.py (see its header).
import json, sys
rec = json.load(open(sys.argv[1]))
exc = "\n".join(rec.get("exceptions") or [])
if rec.get("verdict") in ("HARNESS", "FETCH-FAIL"):
    print("test-zaiblank-guest-negctl: FAILED -- harness verdict %s (%s)"
          % (rec.get("verdict"), rec.get("why"))); sys.exit(1)
if "BroadcastChannel" not in exc:
    print("test-zaiblank-guest-negctl: FAILED -- the BC-less build did NOT reproduce the BroadcastChannel death; the positive gate is measuring something else"); sys.exit(1)
print("test-zaiblank-guest-negctl: red as designed -- the module rejects on BroadcastChannel again with the feature compiled out")
