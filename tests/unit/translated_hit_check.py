import collections
import pathlib
import re
import subprocess
import sys

run = subprocess.run([sys.argv[1]], capture_output=True, text=True)
pathlib.Path(sys.argv[2]).write_text(run.stdout + run.stderr)
print(run.stdout, end="")
want = []
if sys.argv[3] == "old":
    for case in ["own-px", "ancestor-px", "nested-px", "own-percent",
                 "ancestor-percent", "nested-percent", "page-scroll",
                 "fixed-control", "fixed-parent"]:
        want += [case + ": " + message for message in [
            "visible translated position targets control",
            "old unshifted position does not target control",
            "input/selection geometry shares visible border box",
            "native caret maps visible point to ordinary byte offset",
            "popup anchor follows translated lower edge"]]
    want += [case + ": control blocks underlying link"
             for case in ["nested-percent", "fixed-control"]]
    want += [
        "translated-child-overflow: visible clipped portion targets translated child",
        "translated-child-overflow: old child area has no ghost target",
        "translated-source-text: text selection rectangle matches translated paint",
        "before-repaint-style-change: new style hit works before another paint",
        "inner-offset-plus-page-scroll: native consumer has same composed point"]
actual = re.findall(r"^FAIL: (.+)$", run.stdout, re.M)
assert collections.Counter(actual) == collections.Counter(want), (actual, want)
assert re.search(rf"^translated-hit: 119 checks, {len(want)} failures$", run.stdout, re.M)
assert run.returncode == (1 if want else 0)
