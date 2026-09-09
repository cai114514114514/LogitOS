#!/usr/bin/env python3
"""check_replay -- assert what the two zaiblank fixes own, on the JSON the
specimen replay driver wrote. Lives under tests/fixtures/zaiblank/ because
that directory is this package's own list (the frameworks package put its
guest driver in fixtures for the same reason); the alternative was heredoc
python inside tests/zaiblank.mk, whose tab-and-dollar escaping has eaten
recipes before (see the cssweb.mk '#' lesson recorded 2026-08-30).

    check_replay.py <replay.json> --positive   # the real build must pass
    check_replay.py <replay.json> --negctl     # the BC-less build must be red

WHAT EACH SIDE ASSERTS, and why these signals and not "the site works":

  positive   1. no BroadcastChannel-shaped exception anywhere on the serial
                (the entry module used to reject on exactly that at
                evaluation; one statement, whole page blank);
             2. the page's /api fetches REACHED the replay server (they are
                recorded in subresource_404). Before the fetch fix they
                rejected "timed out" without the server ever being consulted
                -- the loop-blocked-time defect. The 404s themselves are
                correct: the replay is offline by design, and the page's own
                "Session not available, rendering as guest" branch is the
                honest degradation the fixture exercises.
  negctl     The BroadcastChannel rejection must COME BACK in a build with
             the class compiled out. A control whose red cannot be told from
             a broken harness is no control: harness verdicts (HARNESS,
             FETCH-FAIL) fail BOTH sides loudly instead.
"""

import json
import sys


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ("--positive", "--negctl"):
        print("usage: check_replay.py <replay.json> --positive|--negctl")
        return 2
    rec = json.load(open(sys.argv[1]))
    exc = "\n".join(rec.get("exceptions") or [])
    miss = rec.get("subresource_404") or []
    name = "test-zaiblank-guest"

    if rec.get("verdict") in ("HARNESS", "FETCH-FAIL"):
        print("%s: FAILED -- harness verdict %s (%s); nothing was measured"
              % (name, rec.get("verdict"), rec.get("why")))
        return 1

    if sys.argv[2] == "--negctl":
        if "BroadcastChannel" not in exc:
            print("%s-negctl: FAILED -- the BC-less build did NOT reproduce "
                  "the BroadcastChannel death; the positive gate is measuring "
                  "something else" % name)
            return 1
        print("%s-negctl: red as designed -- the module rejects on "
              "BroadcastChannel again with the feature compiled out" % name)
        return 0

    bad = []
    if "BroadcastChannel" in exc:
        bad.append("the entry module still dies on BroadcastChannel")
    api = [m for m in miss if "/api/" in m]
    if len(api) < 2:
        bad.append("the /api fetches never reached the replay server "
                   "(saw %d of >=2): %s" % (len(api), api))
    if bad:
        print("%s: FAILED --" % name)
        for b in bad:
            print("  " + b)
        return 1
    paths = ", ".join(sorted(set(m.split("?")[0] for m in api)))
    print("%s: PASS -- no BroadcastChannel death; %d /api request(s) served (%s)"
          % (name, len(api), paths))
    return 0


if __name__ == "__main__":
    sys.exit(main())
