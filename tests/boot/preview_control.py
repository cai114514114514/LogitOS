#!/usr/bin/env python3
"""Build and observe the real player's missing-delay negative control."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
from make_disk_recipe import disk_recipe

p = argparse.ArgumentParser()
p.add_argument("--build", required=True)
p.add_argument("--pack", action="store_true")
a = p.parse_args()
b = Path(a.build)
if a.pack:
    recipe = subprocess.check_output(["make", "--no-print-directory", "-n", "-W", "tools/mkfs.py",
                                      "BUILD=" + str(b), str(b / "disk.img")], text=True)
    _, files = disk_recipe(recipe)
    old = str(b / "preview.aex") + ":preview.aex"
    if files.count(old) != 1:
        sys.exit("FAIL: production recipe does not contain exactly one Preview")
    files[files.index(old)] = str(b / "previewneg.aex") + ":preview.aex"
    subprocess.run([sys.executable, "tools/mkfs.py", str(b / "disk-prevneg.img"), *files], check=True)
else:
    log = b / "preview-negctl.log"
    with log.open("w") as out:
        result = subprocess.run([sys.executable, "-u", "tests/qmp/qmp_preview.py",
            "--iso", str(b / "logit.iso"), "--disk", str(b / "disk-prevneg.img"),
            "--ref", str(b / "previewref"), "--timing-only",
            "--out", str(b / "preview-shots-negctl")], stdout=out, stderr=subprocess.STDOUT)
    text = log.read_text()
    # The old shell recipe accepted ANY nonzero exit, including a QMP mouse
    # failure before Preview opened. Require the measured missing-delay failure.
    early = re.findall(r"FAIL.*one loop takes its declared (\d+) ms.*elapsed (\d+) ms", text)
    if result.returncode and any(int(elapsed) < int(declared) for declared, elapsed in early):
        print("PASS negative control: the guest observed an animation finishing before its deadline")
        for line in text.splitlines():
            if "one loop takes" in line:
                print(line)
    else:
        print(text)
        sys.exit("FAIL: missing-delay control did not produce the expected early-loop failure")
