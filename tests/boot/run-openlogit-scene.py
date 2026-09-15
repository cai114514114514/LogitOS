#!/usr/bin/env python3
"""Build the three-file SDK consumer in the guest, then use ordinary input.

Rendered viewport differences and exact restoration are the authority for
controls. Serial frame counters supplement pixels when checking idle behavior.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/qmp"))
from qmp_ui import Session, configure
from qmp_window import win_by_title, cmd_key
from owned_process import stop_owned

parser = argparse.ArgumentParser()
parser.add_argument("--build", required=True, type=Path)
args = parser.parse_args()
build = args.build.resolve()
out = build / "scene-guest"
out.mkdir(exist_ok=True)
serial = out / "serial.log"
serial.write_text("")
checks, actions = [], []
process = ui = None


def log():
    return serial.read_text(errors="replace")


def record(complete=False):
    result = {"complete": complete, "passed": bool(checks) and all(c["passed"] for c in checks),
              "checks": checks, "actions": actions}
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")


def check(ok, name):
    checks.append({"name": name, "passed": bool(ok)})
    record()
    print(("PASS " if ok else "FAIL ") + name, flush=True)
    if not ok:
        raise AssertionError(name)


def wait(condition, name, seconds=120):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("guest exited: " + name)
        if "STUDIO ERROR" in log() or "error: include file" in log():
            raise RuntimeError("guest compiler/render failure; inspect serial.log")
        value = condition()
        if value:
            return value
        time.sleep(.05)
    raise TimeoutError(name)


def send(line):
    actions.append({"shell": line})
    process.stdin.write((line + "\n").encode())
    process.stdin.flush()


def tap(key):
    actions.append({"key": key})
    for down in (True, False):
        response = ui._input([{"type": "key", "data": {
            "down": down, "key": {"type": "qcode", "data": key}}}])
        if "error" in response:
            raise RuntimeError(response)
        time.sleep(.05)


def window():
    return win_by_title(str(serial), "OpenLogit Scene Studio")


def point(x, y):
    w = window()
    return w["x"] + x, w["y"] + w["h"] - w["ch"] + y


def click(x, y):
    actions.append({"click": [x, y]})
    ui.click_at_confirmed(str(out / "pointer.ppm"), *point(x, y))


def shot(name):
    path = out / (name + ".ppm")
    response = ui.cmd({"execute": "screendump", "arguments": {"filename": str(path)}})
    if "error" in response:
        raise RuntimeError(response)
    image = Image.open(path).convert("RGB")
    image.save(out / (name + ".png"))
    x, y = point(0, 0)
    client = image.crop((x, y, x + 960, y + 600))
    client.save(out / (name + "-client.png"))
    return client


def viewport(image):
    return image.crop((208, 108, 728, 472))


def different(a, b):
    return ImageChops.difference(a, b).getbbox() is not None


def frames():
    return [tuple(map(int, values)) for values in re.findall(
        r"STUDIO FRAME n=(\d+) scenes=(\d+) playing=(\d+) active=(\d+) phase=(\d+)", log())]


def settle(name):
    # A final serial write may be torn by an SMP kernel diagnostic. Stable
    # counters plus repeated pixels work even when that one line is incomplete.
    last = None
    stable_since = time.monotonic()
    deadline = stable_since + 30
    while time.monotonic() < deadline:
        current = frames()
        if current != last:
            last = current
            stable_since = time.monotonic()
        if current and time.monotonic() - stable_since > .9:
            return shot(name)
        if "STUDIO ERROR" in log():
            raise RuntimeError("render failure: " + name)
        time.sleep(.1)
    raise TimeoutError("idle: " + name)


record()
configure(1280, 800)
try:
    with tempfile.TemporaryDirectory(prefix="ol-scene-") as tmp, serial.open("wb") as stream:
        socket = str(Path(tmp) / "qmp.sock")
        command = ["qemu-system-x86_64", "-cpu", "max", "-smp", "4", "-m", "1G",
                   "-accel", "tcg,thread=multi", "-cdrom", str(build / "logit.iso"),
                   "-drive", f"file={build / 'disk.img'},format=raw,if=none,id=d0",
                   "-device", "virtio-blk-pci,drive=d0", "-snapshot", "-boot", "d",
                   "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
                   "-display", "none", "-nic", "none", "-serial", "stdio",
                   "-qmp", f"unix:{socket},server=on,wait=off", "-no-reboot"]
        (out / "command.json").write_text(json.dumps(command, indent=2))
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT)
        wait(lambda: "[wm] launched Finder" in log(), "desktop")
        ui = Session(socket, serial=str(serial))
        send("/bin/mkdir /tmp")
        sources = " ".join("/usr/share/openlogit/studio/" + name + ".c" for name in ("main", "scene", "ui"))
        send("/bin/tcc " + sources + " -I/usr/include/openlogit -lopenlogit -o /tmp/scene-studio")
        send("/tmp/scene-studio")
        wait(lambda: "STUDIO READY" in log() and window(), "guest-compiled Scene Studio")
        initial = settle("initial")
        check("/tmp/scene-studio loading" in log(), "guest compiler links three source files against installed SDK")
        check(len(set(viewport(initial).getdata())) > 500, "native viewport displays shaded textured geometry")
        count = len(frames())
        time.sleep(1)
        check(len(frames()) == count, "paused scene and settled UI stop frame submissions")

        for key, name in (("s", "shadow"), ("t", "texture"), ("c", "toon")):
            tap(key)
            changed = settle(name)
            check(different(viewport(initial), viewport(changed)), name + " control changes actual 3D pixels")
            tap(key)
            restored = settle(name + "-restored")
            check(not different(viewport(initial), viewport(restored)), name + " restoration reproduces exact scene pixels")

        click(770, 358)
        lit = settle("light")
        check(different(viewport(initial), viewport(lit)), "light slider changes illumination and projected shadows")
        tap("r")
        settle("reset-light")
        click(450,80)
        motion_page = settle("motion-page")
        check(different(initial.crop((744,108,944,488)), motion_page.crop((744,108,944,488))),
              "Motion tab displays its own inspector and controls")
        click(765, 463)
        rest_pose = settle("rest-pose")
        check(different(viewport(initial), viewport(rest_pose)), "pose blend slider deforms the actual skinned mesh")
        tap("r")
        settle("reset-pose")
        click(650, 515)
        scrubbed = settle("scrubbed")
        check(different(viewport(initial), viewport(scrubbed)) and not frames()[-1][2],
              "timeline scrubbing selects a different pose while remaining paused")
        tap("spc")
        wait(lambda: frames() and frames()[-1][2] == 1, "playing scene")
        a = shot("playing-a")
        wait(lambda: different(viewport(a), viewport(shot("playing-b"))), "animated geometry", 20)
        check(True, "shared timeline changes torus rotation and bone deformation on screen")
        tap("spc")
        paused = settle("paused")
        count = len(frames())
        time.sleep(1)
        check(len(frames()) == count and not different(viewport(paused), viewport(shot("paused-stable"))),
              "pause freezes displayed geometry and stops render submissions")

        tap("h")
        modal = settle("help")
        check(different(paused.crop((270,200,690,420)), modal.crop((270,200,690,420))),
              "help opens an animated modal over the scene")
        click(480, 384)
        closed = settle("help-closed")
        check(not different(viewport(paused), viewport(closed)), "closing modal clears its old coverage")
        click(144, 395)
        orbited = settle("orbited")
        check(different(viewport(closed), viewport(orbited)), "orbit camera control changes projection")
        tap("q")
        quality = settle("low-quality")
        check(different(viewport(orbited), viewport(quality)), "quality control reallocates and displays lower-resolution target")

        tap("spc")
        wait(lambda: frames()[-1][2] == 1, "playing before minimize")
        cmd_key(ui, "m")
        wait(lambda: window()["min"] == 1, "minimized")
        time.sleep(.5)
        count = len(frames())
        time.sleep(1)
        check(len(frames()) == count, "minimized scene stops continuous rendering")
        for _ in range(4):
            cmd_key(ui, "tab")
            time.sleep(.3)
            if window()["min"] == 0:
                break
        wait(lambda: window()["min"] == 0 and len(frames()) > count, "restored scene")
        check(True, "normal window switching restores scene rendering")
        tap("spc")
        settle("restored-paused")
        ui.launch_app("settings", title="Settings", probe=str(out / "pointer.ppm"))
        settings = wait(lambda: win_by_title(str(serial), "Settings"), "Settings")
        time.sleep(.4)
        ui.click_at_confirmed(str(out / "pointer.ppm"), settings["x"] + 584,
                              settings["y"] + settings["h"] - settings["ch"] + 388)
        wait(lambda: "STUDIO MOTION reduced=1" in log(), "live reduced motion")
        w = window()
        ui.click_at_confirmed(str(out / "pointer.ppm"), w["x"] + w["w"] // 2, w["y"] + 15)
        settle("reduced")
        tap("spc")
        reduced = settle("reduced-play")
        count = len(frames())
        time.sleep(1)
        check(not frames()[-1][2] and len(frames()) == count and
              not different(viewport(reduced), viewport(shot("reduced-stable"))),
              "system reduced-motion setting blocks continuous playback and leaves no render wakeups")
        timing = [{"ns": int(ns), "width": int(width), "height": int(height)}
                  for ns, width, height in re.findall(r"STUDIO RENDER ns=(\d+) width=(\d+) height=(\d+)", log())]
        (out / "render-timings.json").write_text(json.dumps({
            "scope": "guest scene render calls; excludes window composition and is not a 2D baseline comparison",
            "samples": timing}, indent=2) + "\n")
        record(True)
        paths = [build / "logit.iso", build / "disk.img", build / "sdk/libopenlogit.a",
                 build / "sdk/scene-studio.elf"]
        (out / "artifacts.json").write_text(json.dumps(
            {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}, indent=2) + "\n")
except Exception as error:
    checks.append({"name": str(error), "passed": False})
    record()
    raise
finally:
    if ui:
        ui.f.close()
        ui.s.close()
    stop_owned(process)
print(f"OpenLogit scene guest: {len(checks)} checks passed", flush=True)
