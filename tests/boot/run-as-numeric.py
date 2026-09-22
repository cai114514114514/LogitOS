#!/usr/bin/env python3
"""Check mixed numeric arithmetic through the real Code Studio and /bin/as.

Each invocation creates its own writable disk. Studio output is a pipe, not a
file: compare its actual scanout with a separately run pure-float reference,
then require a different-result control to change the same pixels. CLI captures
add exact bytes and exit codes without substituting for the GUI interaction.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import time

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[2]
SOURCE = "# aether: 3.0\ny = 2\nx = 1.0\nprint(x + y)\n"


def load_runtime():
    spec = importlib.util.spec_from_file_location("numeric_guest", ROOT / "tests/boot/run-agent.py")
    runtime = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runtime)
    return runtime


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def packed_digest(disk, path):
    spec = importlib.util.spec_from_file_location("numeric_disk", ROOT / "tools/license_audit.py")
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    filesystem = reader._LogitFS(disk)
    try:
        inode = filesystem.lookup(path)
        if inode is None:
            raise AssertionError("Private guest disk is missing " + path)
        return hashlib.sha256(filesystem.read_file(inode)).hexdigest()
    finally:
        filesystem.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True, help="Current Studio artifacts")
    parser.add_argument("--base", type=Path, default=Path("build-layout-browser"))
    parser.add_argument("--as", dest="compiler", type=Path, help="Override /bin/as for baseline")
    parser.add_argument("--native", type=Path, help="LLVM-built mixed numeric fixture to execute")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--expect-error", action="store_true", help="Observe the old AS3202 refusal")
    args = parser.parse_args()
    build, base, out = args.build.resolve(), args.base.resolve(), args.out.resolve()
    compiler = args.compiler.resolve() if args.compiler else build / "as.aex"
    native = args.native.resolve() if args.native else None
    out.mkdir(parents=True, exist_ok=False)
    fixtures = {
        "demo.as": SOURCE,
        "reference.as": "# aether: 3.0\nprint(3.0)\n",
        "wrong.as": "# aether: 3.0\nprint(4.0)\n",
    }
    for name, source in fixtures.items():
        (out / name).write_text(source)
    (out / "settings.conf").write_text(
        "ui.dark = 0\napp.studio.trace_paint = 1\napp.studio.project = /docs\n"
    )
    (out / "state.txt").write_text("Private numeric regression disk.\n")
    files = [f"{base}/{name}.aex:/bin/{name}" for name in ("login", "sh", "cat", "echo", "pref")]
    files += [f"{ROOT}/fsroot/fonts/{name}.ttf:/fonts/{name}.ttf"
              for name in ("ui", "mono", "ui-bold", "mono-bold")]
    files += [f"{ROOT}/third_party/fonts/DejaVuSans.ttf:/fonts/text.ttf",
              f"{build}/studio.aex:/studio.aex", f"{compiler}:/bin/as",
              f"{out}/settings.conf:/etc/settings.conf", f"{out}/state.txt:/state/fixture"]
    files += [f"{out}/{name}:/docs/{name}" for name in fixtures]
    if native:
        files.append(f"{native}:/mixed.aex")
    with (out / "mkfs.log").open("w") as log:
        subprocess.run(["python3", "tools/mkfs.py", str(out / "disk.img"), *files],
                       cwd=ROOT, check=True, stdout=log)
    report = {"passed": False, "expect_error": args.expect_error, "checks": [],
              "compiler_sha256": digest(compiler), "studio_sha256": digest(build / "studio.aex"),
              "kernel_sha256": digest(base / "logit.iso"), "source": SOURCE}
    if native:
        report["native_sha256"] = digest(native)
    guest = None

    def check(name, value):
        if not value:
            raise AssertionError(name)
        report["checks"].append(name)
        print("PASS", name, flush=True)

    def shot(name):
        guest.screenshot()
        picture = Image.open(out / "desktop.ppm").convert("RGB")
        picture.save(out / (name + ".png"))
        return picture

    def click(px, py):
        # Large PS/2 relative deltas can be clipped. Move in bounded steps so
        # a passed QMP request really puts the pointer on the toolbar button.
        target = (x + px, y + py)
        while target != tuple(guest.pointer):
            dx = max(-100, min(100, target[0] - guest.pointer[0]))
            dy = max(-100, min(100, target[1] - guest.pointer[1]))
            guest.qmp("input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": dx}},
                {"type": "rel", "data": {"axis": "y", "value": dy}}]})
            guest.pointer[0] += dx
            guest.pointer[1] += dy
            time.sleep(.03)
        for down in (True, False):
            guest.qmp("input-send-event", {"events": [
                {"type": "btn", "data": {"button": "left", "down": down}}]})
            time.sleep(.12)
        time.sleep(.3)

    def open_file(index, name):
        click(36, 20)
        shot(name + "-picker")
        # The chooser is centered in the content area; its title consumes 40
        # pixels. Rows start at y=80 in the dialog's inner coordinate system.
        dialog_height = min(content_height - 180, 450)
        left = (width - 540) // 2
        top = max(12, (content_height - dialog_height - 40) // 3)
        click(left + 180, top + 40 + 80 + index * 28 + 13)
        time.sleep(1.5)
        selected = guest.capture("/bin/pref read app.studio.tab." + str(index))
        # Tabs are opened in sorted fixture order by this gate.
        check(name + " opened through picker", selected.strip() == "/docs/" + name + ".as")
        shot(name + "-opened")

    def button(button_x, name):
        start = len(guest.log)
        click(button_x, 20)
        guest.wait(b"STUDIO_PAINT", 30, start)
        # Event completion and process completion are different. A settled
        # output/status crop must agree over multiple frames before comparison.
        crop = (x + 208, y + content_height - 145,
                x + width - 12, y + content_height - 105)
        previous = None
        same = 0
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            time.sleep(.5)
            picture = shot(name)
            region = picture.crop(crop)
            if previous is not None and ImageChops.difference(previous, region).getbbox() is None:
                same += 1
                if same >= 3:
                    return picture
            else:
                same = 0
            previous = region
        raise AssertionError(name + " did not settle")

    try:
        report["packed_compiler_sha256"] = packed_digest(out / "disk.img", "/bin/as")
        report["packed_studio_sha256"] = packed_digest(out / "disk.img", "/studio.aex")
        check("packed compiler matches selected artifact",
              report["packed_compiler_sha256"] == report["compiler_sha256"])
        check("packed Studio matches selected artifact",
              report["packed_studio_sha256"] == report["studio_sha256"])
        if native:
            report["packed_native_sha256"] = packed_digest(out / "disk.img", "/mixed.aex")
            check("packed native fixture matches selected artifact",
                  report["packed_native_sha256"] == report["native_sha256"])
        guest = load_runtime().Guest(base, out / "disk.img", out, "bios", "512M")
        guest.wait(b"LogitOS shell", 180)
        if native:
            output = guest.capture("/mixed.aex")
            report.update(native_output=output, native_exit=guest.last_capture_exit)
            check("LLVM native fixture passes in guest",
                  output == "mixed numeric ok\n" and guest.last_capture_exit == 0)
        diagnostic = guest.capture("/bin/as check --json /docs/demo.as")
        report["diagnostic"] = json.loads(diagnostic)
        report["check_exit"] = guest.last_capture_exit
        errors = report["diagnostic"]["diagnostics"]
        if args.expect_error:
            check("baseline guest reports AS3202", guest.last_capture_exit == 1 and
                  any(error["code"] == "AS3202" for error in errors))
        else:
            check("guest type check accepts mixed arithmetic", guest.last_capture_exit == 0 and not errors)
            output = guest.capture("/bin/as /docs/demo.as")
            report.update(output=output, run_exit=guest.last_capture_exit)
            check("guest /bin/as prints exactly 3", output == "3\n" and guest.last_capture_exit == 0)
        guest.command("/studio.aex &")
        guest.wait(b"STUDIO_PAINT", 60)
        time.sleep(2)
        pattern = (rb"\[wm\] win \d+ frame (\d+) (\d+) (\d+) (\d+) content "
                   rb"(\d+) (\d+) pt zoom 0 min 0 Code Studio")
        frames = re.findall(pattern, bytes(guest.log))
        check("actual Code Studio geometry reported", bool(frames))
        x, y, width, height, content_width, content_height = map(int, frames[-1])
        y += height - content_height
        check("unscaled Code Studio window", width == content_width)
        report["geometry"] = [x, y, width, content_height]
        open_file(0, "demo")
        checked = button(168, "demo-check")
        # Only the first output lines belong to this assertion. The dock can
        # overlap the lower panel and animate independently of program output.
        panel = (x + 208, y + content_height - 145,
                 x + width - 12, y + content_height - 105)
        error_ink = any(red > 140 and red > green * 1.3 and red > blue * 1.2
                        for red, green, blue in checked.crop(panel).getdata())
        check("Studio Check shows expected diagnostics", error_ink == args.expect_error)
        mixed = button(235, "demo-run")
        if args.expect_error:
            # The paired new-image run below supplies the positive result; do
            # not call a diagnostic screenshot successful numeric execution.
            report["observed_refusal"] = True
        else:
            open_file(1, "reference")
            reference = button(235, "reference-run")
            open_file(2, "wrong")
            wrong = button(235, "wrong-run")
            check("GUI output control changes actual scanout",
                  ImageChops.difference(reference.crop(panel), wrong.crop(panel)).getbbox() is not None)
            check("Studio mixed output equals pure-float 3",
                  ImageChops.difference(reference.crop(panel), mixed.crop(panel)).getbbox() is None)
            # Restore the actual reported program for the review screenshot.
            click(246, 56)
            button(235, "fixed-demo")
        check("source remains exact screenshot program", guest.capture("/bin/cat /docs/demo.as") == SOURCE)
        check("input artifacts remain unchanged", digest(compiler) == report["compiler_sha256"] and
              digest(build / "studio.aex") == report["studio_sha256"] and
              digest(base / "logit.iso") == report["kernel_sha256"] and
              (not native or digest(native) == report["native_sha256"]))
        report["passed"] = True
    except BaseException as error:
        report["error"] = str(error)
        if guest:
            try:
                shot("failure")
            except Exception:
                pass
        raise
    finally:
        if guest:
            guest.close()
            report["guest_stopped"] = guest.process.poll() is not None
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
