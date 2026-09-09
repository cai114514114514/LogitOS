#!/usr/bin/env python3
# Verdict check for `make test-zaiblank-guest` (tests/zaiblank.mk). Was an
# inline recipe heredoc until 2026-08-30, but make feeds each recipe line to
# its own shell, so the un-indented body broke parsing of the whole fragment
# ("missing separator") and every make invocation with it. Extracted verbatim.
import json, sys
rec = json.load(open(sys.argv[1]))
exc = "\n".join(rec.get("exceptions") or [])
miss = rec.get("subresource_404") or []
bad = []
if rec.get("verdict") in ("HARNESS", "FETCH-FAIL"):
    bad.append("harness verdict %s (%s)" % (rec.get("verdict"), rec.get("why")))
if "BroadcastChannel" in exc:
    bad.append("the entry module still dies on BroadcastChannel")
api = [m for m in miss if "/api/" in m]
if len(api) < 2:
    bad.append("the /api fetches never reached the replay server (saw %d of >=2): %s" % (len(api), api))
if bad:
    print("test-zaiblank-guest: FAILED --")
    for b in bad: print("  " + b)
    sys.exit(1)
print("test-zaiblank-guest: PASS -- no BroadcastChannel death; %d /api request(s) served (%s)"
      % (len(api), ", ".join(sorted(set(m.split("?")[0] for m in api)))))
