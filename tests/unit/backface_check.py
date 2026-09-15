"""Require the exact disabled-consumer failures, not an arbitrary red binary."""
import re
import sys
from collections import Counter
from pathlib import Path

text = Path(sys.argv[1]).read_text()
expected = Counter()
ordinary = [
    "face ink matches facing",
    "ordinary descendant shares face visibility",
    "native hit agrees with visible plane",
    "covered link only returns when face is absent",
    "full repaint repeats current facing",
]
for scene in ("x-back", "y-back", "x-quarter", "y-quarter", "child-visible-cannot-reveal-plane"):
    for assertion in ordinary:
        expected[f"FAIL: {scene}: {assertion}"] += 1
    if scene not in ("x-quarter", "y-quarter"):
        expected[f"FAIL: {scene}: face background follows same visibility"] += 1
for scene in ("preserve-parent-turn", "hidden-parent-none-child", "hidden-parent-two-d-child"):
    for assertion in ("nested plane facing matches accumulated transform", "nested hit shares plane ownership"):
        expected[f"FAIL: {scene}: {assertion}"] += 1
for assertion in ("inside clip obeys backface ownership", "clipped full paint still culls turned face"):
    expected[f"FAIL: overflow-clipping: {assertion}"] += 1
observed = Counter(line for line in text.splitlines() if line.startswith("FAIL:"))
assert observed == expected, (observed - expected, expected - observed)
assert re.search(r"^backface: 124 checks, 36 failures$", text, re.M), "wrong negative summary"
print("backface negative: exact 36 consumer failures; all other checks preserved")
