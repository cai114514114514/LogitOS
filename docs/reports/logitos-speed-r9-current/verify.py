#!/usr/bin/env python3
"""Fail closed when the frozen R9 responsiveness evidence no longer agrees."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent
EVIDENCE = ROOT / "evidence"
RELEASE = ROOT / "release-tested"
EXPECTED_HASHES = {
    "logit.iso": "aa35f02fac7f5a93b13868398201c37a82519dc48b75e37a8e6f52c6e1217810",
    "disk.img": "a3dd855fe6713b07e0f59788bf57fd11d68da0e54cdfc09597f82ac3b184bdf3",
    "browser.aex": "04e9f18dd8cebd4b3da58afc41e6cae30f1ec46fcfca65676397a5af4a9188a0",
}
EXPECTED_BROWSER = {
    "lean": (1660, 560, -66.27),
    "style": (2040, 750, -63.24),
    "script": (2220, 730, -67.12),
    "deepseek": (7190, 750, -89.57),
    "wikipedia": (19520, 5110, -73.82),
}


def load(relative: str):
    return json.loads((EVIDENCE / relative).read_text())


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit("FAIL: " + message)


for name, expected in EXPECTED_HASHES.items():
    digest = hashlib.sha256((RELEASE / name).read_bytes()).hexdigest()
    require(digest == expected, f"{name} hash {digest} != {expected}")

browser = load("browser-comparison-current.json")
for name, (baseline, current, change) in EXPECTED_BROWSER.items():
    case = browser["cases"][name]
    require(case["baseline"]["median_total_ms"] == baseline,
            f"{name} baseline median drifted")
    require(case["current_r9"]["median_total_ms"] == current,
            f"{name} R9 median drifted")
    require(case["change_percent"] == change, f"{name} change drifted")
    samples = case["current_r9"]["samples"]
    require(len(samples) == 3 and all(s.get("case_complete") for s in samples),
            f"{name} does not have three complete samples")

invalid = load("browser-invalid-interleaved/results.json")
require(len(invalid) == 15 and any("total_ms" not in item for item in invalid),
        "interleaved-log negative capture no longer contains a rejected sample")

interactions = load("interactions-current/result.json")
require(interactions.get("passed") is True and interactions["artifacts"].get("unchanged") is True,
        "interaction gate or artifact binding failed")
checks = {item["case"]: item for item in interactions["checks"]}
require(checks["close_during_held_stylesheet"]["wm_gone_host_bound_ms"] <= 400,
        "close during stylesheet exceeded 400 ms")
require(checks["close_during_busy_javascript"]["wm_gone_host_bound_ms"] <= 500,
        "close during JavaScript exceeded 500 ms")
for case, pixels in (("wheel_during_held_stylesheet", 120),
                     ("wheel_during_held_initial_script", 120),
                     ("wheel_during_held_image", 160)):
    require(checks[case]["moved_during_load_px"] >= pixels,
            f"{case} did not visibly scroll during load")

animation = load("animation-comparison-current.json")
require(animation["baseline"]["frames"] == animation["candidate"]["frames"] == 48,
        "animation frame count drifted")
require(animation["candidate"]["elapsed_ms"] == 1340 and
        animation["candidate"]["max_gap_ms"] == 40,
        "animation candidate timing drifted")
require(animation["baseline"]["geometry_same"] and animation["candidate"]["geometry_same"],
        "animation geometry parity failed")

network = load("network-comparison-current.json")
page = network["page_net_ms"]
require((page["control_median"], page["candidate_median"],
         page["paired_median_delta"], page["faster_pairs"]) == (590, 519, -92, 6),
        "seven-pair network result drifted")

webaccel = load("webaccel-comparison-current.json")
current = webaccel["current_r9"]
require((current["first_ms"], current["second_ms"]) == (3180, 2430),
        "repeat-visit timing drifted")
require(current["second_visit_cache"] == {"requests": 14, "dials": 0, "hits": 14},
        "repeat-visit cache category failed")
require(current["artifacts"].get("unchanged") is True,
        "repeat-visit artifacts changed during measurement")

invalid_launch = load("webaccel-invalid-launch/result.json")
pre_binding = load("webaccel-pre-artifact-binding/result.json")
require(not invalid_launch.get("visits"), "invalid dock click unexpectedly became evidence")
require("artifacts" not in pre_binding,
        "pre-binding measurement unexpectedly acquired artifact identity")

print("PASS: frozen R9 hashes, 15 browser samples, interaction, animation, network, and cache gates")
