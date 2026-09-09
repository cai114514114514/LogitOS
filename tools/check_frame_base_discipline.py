#!/usr/bin/env python3
"""ONE JAR, ONE DOOR, applied to c/apps/browser/bfetch.h's g_base.

The property (proved by tests/unit/frame_fetch_test.c against the fake
fetcher): a second browsing context (js_frame.c, not yet built) must resolve
its own references with an EXPLICIT base (bfetch_resolve(base, ref, ...) then
bfetch_sync(abs, ...), or bfetch_start_from(base, ref)) and must NEVER call
bfetch_set_base() -- that call belongs to the top-level page alone
(js_page.c), because it rewrites the ONE global base every relative
reference with no explicit base of its own resolves against. A frame that
called it would retarget the page's own subsequent fetches at the frame's
origin, silently, which is the failure tests/unit/frame_fetch_test.c's
-DFRAME_FETCH_NO_DISCIPLINE build demonstrates in the other direction (page
base leaking into the frame).

This script is the STATIC half of that gate: a grep, not a build, because a
call to bfetch_set_base() compiles fine and links fine -- nothing about it
is a build error, which is exactly why CLAUDE.md rule 5 says the control has
to be able to be watched failing rather than trusted to review.

USAGE:
    check_frame_base_discipline.py           check real frame source files
    check_frame_base_discipline.py --selftest    prove the grep itself fires,
                                                   against a synthetic fixture
                                                   this script writes and
                                                   deletes -- so a change that
                                                   silently breaks the pattern
                                                   (e.g. a regex typo) is
                                                   caught by CI rather than by
                                                   nobody, which is the exact
                                                   shape of the "stranded
                                                   control" trap CLAUDE.md
                                                   names for tests/*-negctl.
"""
import glob, os, re, sys, tempfile

# js_frame.c does not exist yet (2026-08-30) -- this glob is what makes the
# check apply the day it lands with NO edit to this file. "frame" not
# "frames": js_worker.c's sibling is meant to be one file, per the brief.
FRAME_GLOB = "c/apps/browser/js_frame*.c"

CALL_RE = re.compile(r"\bbfetch_set_base\s*\(")


def scan(path):
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return []
    hits = []
    for i, line in enumerate(text.splitlines(), 1):
        if CALL_RE.search(line):
            hits.append((i, line.strip()))
    return hits


def check_real():
    files = sorted(glob.glob(FRAME_GLOB))
    if not files:
        print("check-frame-base-discipline: SKIPPED -- no c/apps/browser/js_frame*.c "
              "yet. This line goes red the day that file exists and calls "
              "bfetch_set_base(); until then there is nothing to check, and "
              "this message (not a silent pass) is the record of that.")
        return 0
    bad = 0
    for f in files:
        hits = scan(f)
        for lineno, line in hits:
            bad += 1
            print(f"check-frame-base-discipline: VIOLATION {f}:{lineno}: {line}")
            print("  a frame document must resolve its own references with an "
                  "explicit base (bfetch_resolve(base, ref, ...) then "
                  "bfetch_sync(abs, ...)) and must never call bfetch_set_base() "
                  "-- see tools/check_frame_base_discipline.py's header.")
    if bad:
        return 1
    print(f"check-frame-base-discipline: ok -- {len(files)} file(s) checked, "
          "no bfetch_set_base() call found")
    return 0


def selftest():
    """Prove the grep is not vacuous: write a fixture that DOES call
    bfetch_set_base(), confirm scan() flags it, then a clean fixture and
    confirm scan() does not. Both must hold or this script is a control
    that cannot be watched failing."""
    with tempfile.TemporaryDirectory() as d:
        bad_path = os.path.join(d, "js_frame_bad.c")
        good_path = os.path.join(d, "js_frame_good.c")
        with open(bad_path, "w") as f:
            f.write(
                "/* synthetic violation fixture */\n"
                "static void frame_nav(const char *doc_url) {\n"
                "    bfetch_set_base(doc_url); /* WRONG: retargets the page */\n"
                "}\n"
            )
        with open(good_path, "w") as f:
            f.write(
                "/* synthetic clean fixture */\n"
                "static int frame_fetch(const char *base, const char *ref) {\n"
                "    char abs[512];\n"
                "    if (bfetch_resolve(base, ref, abs, sizeof abs) != 0) return -1;\n"
                "    unsigned char *out; int outlen;\n"
                "    return bfetch_sync(abs, &out, &outlen);\n"
                "}\n"
            )
        bad_hits = scan(bad_path)
        good_hits = scan(good_path)
        ok = True
        if not bad_hits:
            print("check-frame-base-discipline: SELFTEST FAILED -- the violation "
                  "fixture calls bfetch_set_base() and the grep found nothing. "
                  "The pattern is broken and the real check would pass silently "
                  "on real code that has the same bug.")
            ok = False
        else:
            print(f"check-frame-base-discipline: selftest ok -- violation fixture "
                  f"correctly flagged at line {bad_hits[0][0]}")
        if good_hits:
            print("check-frame-base-discipline: SELFTEST FAILED -- the clean "
                  "fixture (explicit-base discipline, no bfetch_set_base call) "
                  "was flagged. The pattern is over-matching.")
            ok = False
        else:
            print("check-frame-base-discipline: selftest ok -- clean fixture "
                  "correctly passed")
        return 0 if ok else 1


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    sys.exit(check_real())
