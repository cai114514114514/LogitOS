#!/usr/bin/env python3
"""Observe completion lifetime/clicks in real guest scanout and saved source.

No replacement UI or popup instrumentation: rectangles are found by the actual
selection colour, and acceptance is checked against the saved filesystem bytes.
The private disk also exercises the A3 snapshot protocol used by background checks.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import time

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/unit"))
from as_snapshot_test import encode

spec = importlib.util.spec_from_file_location("studio_guest", ROOT / "tests/boot/run-agent.py")
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--base", type=Path, default=Path("build"))
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    build, base, out = args.build.resolve(), args.base.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / "demo.as").write_text("# aether: 3.0\n")
    module = "# aether: 3.0\ndisk: i64 = 7\n"
    project = "# aether: 3.0\nimport m as tools\ndef main() -> None:\n    "
    (out / "m.as").write_text(module)
    (out / "project.as").write_text(project)
    typed = ("# aether: 3.0\nclass Box:\n    value: i64\n"
             "    def read(self) -> i64:\n        return self.value\n"
             "def main() -> None:\n    box = Box(7)\n    ")
    (out / "typed.as").write_text(typed)
    (out / "settings.conf").write_text(
        "app.studio.trace_paint = 1\napp.studio.project = /docs\n"
        "app.studio.tab.0 = /docs/demo.as\napp.studio.active = 0\n")
    (out / "snapshot").write_bytes(encode([
        ("/docs/main.as", "# aether: 3.0\nimport m\ndef main() -> None:\n    print(m.value)\n"),
        ("/docs/m.as", '# aether: 3.0\nvalue: i64 = "中文"\n'),
    ]))
    files = [f"{base}/{name}.aex:/bin/{name}" for name in ("login", "sh", "cat", "echo", "pref")]
    files += [f"{ROOT}/fsroot/fonts/{name}.ttf:/fonts/{name}.ttf"
              for name in ("ui", "mono", "ui-bold", "mono-bold")]
    files += [f"{ROOT}/third_party/fonts/DejaVuSans.ttf:/fonts/text.ttf",
              f"{build}/studio.aex:/studio.aex", f"{build}/as.aex:/bin/as",
              f"{out}/demo.as:/docs/demo.as", f"{out}/settings.conf:/etc/settings.conf",
              f"{out}/m.as:/docs/m.as", f"{out}/project.as:/docs/project.as",
              f"{out}/typed.as:/docs/typed.as",
              f"{out}/snapshot:/state/snapshot"]
    with (out / "mkfs.log").open("w") as log:
        subprocess.run(["python3", "tools/mkfs.py", str(out / "disk.img"), *files],
                       cwd=ROOT, check=True, stdout=log)
    report = {"studio_sha256": hashlib.sha256((build / "studio.aex").read_bytes()).hexdigest(),
              "compiler_sha256": hashlib.sha256((build / "as.aex").read_bytes()).hexdigest(),
              "checks": []}
    guest = None

    def check(name, value):
        if not value:
            raise AssertionError(name)
        report["checks"].append(name)
        print("PASS", name, flush=True)

    def shot(name):
        guest.screenshot()
        image = Image.open(out / "desktop.ppm").convert("RGB")
        image.save(out / (name + ".png"))
        return image

    def move(x, y):
        while (x, y) != tuple(guest.pointer):
            dx = max(-100, min(100, x - guest.pointer[0]))
            dy = max(-100, min(100, y - guest.pointer[1]))
            guest.qmp("input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": dx}},
                {"type": "rel", "data": {"axis": "y", "value": dy}}]})
            guest.pointer[0] += dx
            guest.pointer[1] += dy
            time.sleep(.03)

    def click(x, y):
        move(x, y)
        for down in (True, False):
            guest.qmp("input-send-event", {"events": [
                {"type": "btn", "data": {"button": "left", "down": down}}]})
            time.sleep(.1)
        time.sleep(.2)

    try:
        guest = runtime.Guest(base, out / "disk.img", out, "bios", "512M")
        guest.wait(b"LogitOS shell", 180)
        raw = guest.capture("/bin/as check --json --snapshot-stdin /docs/main.as < /state/snapshot")
        diagnostic = json.loads(raw)
        check("guest checks unsaved imported module", guest.last_capture_exit == 1 and
              len(diagnostic["diagnostics"]) == 1 and
              diagnostic["diagnostics"][0]["path"] == "/docs/m.as")
        guest.command("/studio.aex &")
        guest.wait(b"STUDIO_PAINT", 60)
        time.sleep(2)
        pattern = rb"\[wm\] win \d+ frame (\d+) (\d+) (\d+) (\d+) content (\d+) (\d+) pt zoom 0 min 0 Code Studio"
        frames = re.findall(pattern, bytes(guest.log))
        check("actual guest window geometry", bool(frames))
        x, y, width, height, content_width, content_height = map(int, frames[-1])
        y += height - content_height
        check("unscaled guest window", width == content_width)
        guest.key("ctrl", "end")
        guest.type("pri")
        time.sleep(.4)
        early = shot("completion-open")
        area = (x + 250, y + 110, x + 650, y + 350)
        selection = (38, 62, 88)
        pixels = early.load()
        selected = [(px, py) for py in range(area[1], area[3])
                    for px in range(area[0], area[2]) if pixels[px, py] == selection]
        check("completion actually visible", len(selected) > 3000)
        popup_x = min(px for px, _ in selected)
        popup_y = min(py for _, py in selected)
        time.sleep(4)
        late = shot("completion-after-check")
        check("completion survives four seconds and check",
              ImageChops.difference(early.crop(area), late.crop(area)).getbbox() is None)
        # A visible popup is insufficient if the background check was rejected.
        # Require real diagnostic ink below it, against the formerly empty panel.
        problem_pixels = late.crop((x + 208, y + content_height - 145,
                                    x + content_width - 12, y + content_height - 85))
        check("background diagnostic appears in actual panel",
              any(red > 140 and red > green * 1.3 and red > blue * 1.2
                  for red, green, blue in problem_pixels.getdata()))
        click(popup_x + 12, popup_y + 10)
        guest.key("ctrl", "s")
        time.sleep(.7)
        check("mouse accepts actual completion", guest.capture("/bin/cat /docs/demo.as") ==
              "# aether: 3.0\nprint")
        guest.key("ctrl", "a")
        guest.type("pri")
        time.sleep(.4)
        click(x + 800, y + 350)
        time.sleep(2)
        outside = shot("completion-dismissed-outside")
        check("outside click closes and stays closed", selection not in outside.crop(area).getdata())
        guest.key("ctrl", "end")
        guest.key("ctrl", "spc")
        time.sleep(.3)
        reopened = shot("completion-reopened")
        check("manual completion reopens", selection in reopened.crop(area).getdata())
        guest.key("esc")
        time.sleep(2)
        escaped = shot("completion-dismissed-escape")
        check("escape closes and stays closed", selection not in escaped.crop(area).getdata())
        # Edit the imported module through the actual editor, leaving it
        # unsaved. Disk-only completion would suggest `disk`, which cannot
        # satisfy the saved-source assertion below.
        click(x + 50, y + 144)
        guest.key("ctrl", "home")
        guest.key("down")
        guest.key("home")
        for _ in range(4):
            guest.key("shift", "right")
        guest.type("unsaved")
        # QMP acknowledging send-key does not mean Studio consumed that key.
        # Let the final character settle before a mouse event switches tabs.
        time.sleep(1)
        shot("unsaved-module")
        click(x + 50, y + 168)
        time.sleep(.5)
        guest.key("ctrl", "end")
        guest.type("tools.")
        module_area = (x + 208, y + 100, x + content_width - 10, y + 350)
        deadline = time.monotonic() + 12
        selected = []
        while time.monotonic() < deadline:
            candidate_image = shot("module-completion")
            pixels = candidate_image.load()
            selected = [(px, py) for py in range(module_area[1], module_area[3])
                        for px in range(module_area[0], module_area[2])
                        if pixels[px, py] == selection]
            if len(selected) > 3000:
                break
            time.sleep(.2)
        check("native module candidate actually visible", len(selected) > 3000)
        click(min(px for px, _ in selected) + 12, min(py for _, py in selected) + 10)
        guest.key("ctrl", "s")
        time.sleep(.7)
        check("native candidate accepts unsaved imported name",
              guest.capture("/bin/cat /docs/project.as") == project + "tools.unsaved")
        check("completion did not save imported module",
              guest.capture("/bin/cat /docs/m.as") == module)
        # Request another asynchronous result and dismiss immediately. Waiting
        # past child completion proves a late reply cannot resurrect the popup.
        guest.key("ctrl", "spc")
        guest.key("esc")
        time.sleep(3)
        dismissed = shot("native-completion-dismissed")
        check("native reply stays dismissed",
              selection not in dismissed.crop(module_area).getdata())
        click(x + 50, y + 192)
        time.sleep(.5)
        guest.key("ctrl", "end")
        guest.type("box.re")
        deadline = time.monotonic() + 12
        selected = []
        while time.monotonic() < deadline:
            candidate_image = shot("object-completion")
            pixels = candidate_image.load()
            selected = [(px, py) for py in range(module_area[1], module_area[3])
                        for px in range(module_area[0], module_area[2])
                        if pixels[px, py] == selection]
            if len(selected) > 3000:
                break
            time.sleep(.2)
        check("typed method candidate actually visible", len(selected) > 3000)
        click(min(px for px, _ in selected) + 12, min(py for _, py in selected) + 10)
        guest.key("ctrl", "s")
        time.sleep(.7)
        check("typed method accepts and saves actual source",
              guest.capture("/bin/cat /docs/typed.as") == typed + "box.read")
        report["passed"] = True
    except BaseException as error:
        report.update(passed=False, error=str(error))
        if guest:
            shot("failure")
        raise
    finally:
        (out / "result.json").write_text(json.dumps(report, indent=2))
        if guest:
            guest.close()


if __name__ == "__main__":
    main()
