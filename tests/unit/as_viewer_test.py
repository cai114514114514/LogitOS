#!/usr/bin/env python3
"""Native asview acceptance: independent pixels, real input and closed output.

The same helpers are consumed by cross-compiled and Make-packaged guest gates.
No readiness assertion depends on serial text: four-core kernel logging can
split an application line. Scanout proves readiness; after closing the app we
check its complete output and actual exit code from the guest filesystem.
"""

import argparse
import hashlib
from pathlib import Path
import shlex
import subprocess
import tempfile
import time

from PIL import Image, ImageChops
from as_capability_test import kernel_grants

ROOT = Path(__file__).resolve().parents[2]
BOX = (8, 30, 544, 348)
CANVASES = ((42, 42, 52), (229, 229, 234))
DOT = (255, 0, 229)


def guest_viewer_assets(work):
    files = []
    for name, width, height, destination in (
        ("viewer-dot", 60, 40, "/media/viewer/first.png"),
        ("viewer-large", 800, 600, "/media/large/image.png"),
    ):
        path = work / (name + ".png")
        subprocess.run(["python3", str(ROOT / "tests/unit/dot_gen.py"), str(path),
                        str(width), str(height)], check=True, capture_output=True, timeout=30)
        files.append((path, destination))
    files.append((ROOT / "tests/fixtures/image/still.bmp", "/media/viewer/second.bmp"))
    bad = work / "viewer-not-image.txt"
    bad.write_text("ordinary text is not a decoded image\n")
    files.append((bad, "/media/not-image.txt"))
    # The prebuilt-disk QMP adapter keeps its original cross-subsystem fixtures:
    # terminal PNG, image decoder BMP/WebP and a real H.264 non-image. Package
    # them too so that entrypoint can be exercised on this same VM-free disk.
    files.extend([
        (work / "viewer-dot.png", "/media/dot.png"),
        (ROOT / "tests/fixtures/image/still.bmp", "/media/img/still.bmp"),
        (ROOT / "tests/fixtures/image/still.webp", "/media/img/still.webp"),
        (ROOT / "tests/fixtures/video/sample.h264", "/media/sample.h264"),
    ])
    # A real narrowed kernel grant must still allow this native executable to
    # open a GUI. Derive the bits from kernel headers rather than copying masks.
    grants = kernel_grants(work)
    mask = grants["fs"][0] | grants["raw"][0] | grants["gui"][0]
    records = {destination: {"host": str(source),
                             "sha256": hashlib.sha256(source.read_bytes()).hexdigest()}
               for source, destination in files}
    return files, {"files": records, "scope_mask": mask}


def color_mask(frame, color):
    difference = ImageChops.difference(frame, Image.new("RGB", frame.size, color))
    red, green, blue = difference.split()
    maximum = ImageChops.lighter(ImageChops.lighter(red, green), blue)
    return maximum.point(lambda value: 255 if value == 0 else 0)


def canvas_origin(frame):
    for color in CANVASES:
        mask = color_mask(frame, color)
        box = mask.getbbox()
        if box and box[2] - box[0] == BOX[2] and box[3] - box[1] >= BOX[3]:
            # Antialiased header text can contain the exact canvas gray. It
            # expanded the first observed bbox upward by 9 pixels. Locate the
            # actual canvas using both continuous side strips, not every gray
            # pixel in the window and not a guessed desktop cascade position.
            top = box[3] - BOX[3]
            left = mask.crop((box[0], top, box[0] + 1, box[3]))
            right = mask.crop((box[2] - 1, top, box[2], box[3]))
            if left.getextrema() == (255, 255) and right.getextrema() == (255, 255):
                return (box[0] - BOX[0], top - BOX[1])
    raise AssertionError("viewer canvas is not visible at the expected size")


def check_picture(frame, source, actual=False, origin=None):
    origin = origin or canvas_origin(frame)
    with Image.open(source) as decoded:
        pixels = decoded.convert("RGB")
    width, height = pixels.size
    if actual:
        dw, dh = width, height
    elif width * BOX[3] <= BOX[2] * height:
        dw, dh = max(1, width * BOX[3] // height), BOX[3]
    else:
        dw, dh = BOX[2], max(1, height * BOX[2] // width)
    x = BOX[0] + (BOX[2] - dw) // 2
    y = BOX[1] + (BOX[3] - dh) // 2
    left, top = max(x, BOX[0]), max(y, BOX[1])
    right, bottom = min(x + dw, BOX[0] + BOX[2]), min(y + dh, BOX[1] + BOX[3])
    # Match the specified integer nearest-neighbor sampling, not Pillow's
    # center-of-pixel resize convention, which differs at fractional scales.
    expected = bytearray()
    for row in range(top, bottom):
        sy = (row - y) * height // dh
        for column in range(left, right):
            sx = (column - x) * width // dw
            expected.extend(pixels.getpixel((sx, sy)))
    visible = (origin[0] + left, origin[1] + top, origin[0] + right, origin[1] + bottom)
    if frame.crop(visible).tobytes() != expected:
        raise AssertionError("viewer scanout differs from independently decoded source pixels")
    if pixels.getextrema() == ((255, 255), (0, 0), (229, 229)):
        count = color_mask(frame, DOT).histogram()[255]
        if count != (right - left) * (bottom - top):
            raise AssertionError("viewer painted outside its clipping box")
    return {"origin": origin, "rectangle": [x, y, dw, dh], "visible": visible}


def check_refusal(frame):
    origin = canvas_origin(frame)
    if color_mask(frame, DOT).getbbox() is not None:
        raise AssertionError("refused image pixels remain visible")
    x, y = origin[0] + BOX[0], origin[1] + BOX[1]
    colors = frame.crop((x, y, x + BOX[2], y + BOX[3])).getcolors(BOX[2] * BOX[3])
    if colors is None or len(colors) < 12:
        raise AssertionError("refusal has no visible text in its canvas")
    return {"origin": origin, "distinct_colors": len(colors)}


def check_usage(frame):
    heading = color_mask(frame, (192, 57, 43)).getbbox()
    if heading is None or heading[2] - heading[0] < 30:
        raise AssertionError("no-file window has no visible error heading")
    legend = frame.crop((heading[0], heading[1] + 25, heading[0] + 400, heading[3] + 45))
    if color_mask(legend, (110, 110, 115)).histogram()[255] < 20:
        raise AssertionError("no-file window has no visible usage text")
    return {"heading": heading}


def wait_frame(guest, output, name, validator):
    deadline = time.monotonic() + 40
    while True:
        guest.screenshot()
        with Image.open(output / "desktop.ppm") as screenshot:
            frame = screenshot.convert("RGB")
        try:
            evidence = validator(frame)
        except AssertionError:
            if time.monotonic() >= deadline:
                frame.save(output / (name + "-failure.png"))
                raise
            time.sleep(0.2)
            continue
        path = output / (name + ".png")
        frame.save(path)
        return {"screenshot": str(path), **evidence}


def run_guest_viewer(guest, executable, program, output, assets):
    """Exercise one real artifact; no test-only mode or auto-quit in asview."""
    records = []
    stem = program["name"]
    files = assets["files"]

    def launch(label, arguments, scoped=False):
        name = stem + "-" + label
        command = ["/bin/native-capture"]
        if scoped:
            command += ["--caps", str(assets["scope_mask"]), "/usr/as"]
        command += [executable, *arguments]
        destination = "/state/" + name + ".txt"
        status = "/state/" + name + "-exit.txt"
        guest.serial.sendall((shlex.join(command) + " > " + destination +
                              "\n/bin/echo $? > " + status + "\n").encode())
        return name, destination, status

    def close(session, markers, key="q", origin=None):
        if key is None:
            # The real WM traffic light sends EV_CLOSE. Content coordinates
            # come from scanout, not a predicted cascade/window number.
            guest.click(origin[0] + 16, origin[1] - 15)
        else:
            guest.key(key)
        name, destination, status = session
        code = guest.capture("/bin/cat " + status, timeout=30)
        text = guest.capture("/bin/cat " + destination, timeout=30)
        assert code.strip() == "0", (name, code, text)
        for marker in markers:
            assert marker in text, (name, marker, text)
        assert text.endswith("asview: bye\n"), (name, text)
        records.append({"name": name, "output": text, "exit_code": 0})

    first = files["/media/viewer/first.png"]["host"]
    second = files["/media/viewer/second.bmp"]["host"]
    session = launch("navigation", ["/media/viewer/first.png"])
    frame = wait_frame(guest, output, session[0] + "-fit", lambda f: check_picture(f, first))
    records.append(frame)
    origin = frame["origin"]
    for key, label, image, actual in (
        ("a", "actual", first, True),
        ("spc", "toggle", first, False),
        ("n", "next", second, False),
        ("p", "previous", first, False),
    ):
        guest.key(key)
        records.append(wait_frame(guest, output, session[0] + "-" + label,
                                 lambda f: check_picture(f, image, actual, origin)))
    close(session, ["asview: image PNG 60x40", "asview: image BMP",
                    "asview: mode 1:1", "asview: ready"])

    large = files["/media/large/image.png"]["host"]
    session = launch("clipping", ["/media/large/image.png"])
    frame = wait_frame(guest, output, session[0] + "-fit", lambda f: check_picture(f, large))
    guest.key("a")
    records.append(wait_frame(guest, output, session[0] + "-actual",
                             lambda f: check_picture(f, large, True, frame["origin"])))
    guest.key("f")
    records.append(wait_frame(guest, output, session[0] + "-restore", lambda f: check_picture(f, large)))
    close(session, ["asview: image PNG 800x600", "asview: mode 1:1"], key="esc")

    for label, path, reason, scoped in (
        ("unsupported", "/media/not-image.txt", "not an image", False),
        ("missing", "/media/missing.png", "no such file", False),
        ("scope", "/media/viewer/first.png", "capability scope", True),
    ):
        session = launch(label, [path], scoped)
        frame = wait_frame(guest, output, session[0], check_refusal)
        records.append(frame)
        close(session, ["asview: error", path, reason, "asview: ready"],
              key=None if scoped else "q", origin=frame["origin"])
    session = launch("usage", [])
    records.append(wait_frame(guest, output, session[0], check_usage))
    close(session, ["asview: error no file given", "asview: ready"])
    program["viewer"] = records
    return 0


def negative_controls():
    # Build scanout planes with an independently known solid image. Prove that
    # a blank viewer, wrong destination size and header overdraw are rejected.
    with tempfile.TemporaryDirectory(prefix="as-viewer-oracle-") as temporary:
        source = Path(temporary) / "dot.png"
        Image.new("RGB", (60, 40), DOT).save(source)
        good = Image.new("RGB", (560, 400), CANVASES[0])
        good.paste(DOT, (19, 30, 541, 378))
        check_picture(good, source, origin=(0, 0))
        bad_frames = [Image.new("RGB", good.size, CANVASES[0]), good.copy(), good.copy()]
        bad_frames[1].paste(CANVASES[0], (19, 30, 541, 31))
        bad_frames[2].paste(DOT, (19, 0, 541, 1))
        for frame in bad_frames:
            try:
                check_picture(frame, source, origin=(0, 0))
            except AssertionError as error:
                print("PASS viewer pixel control:", error)
            else:
                raise AssertionError("viewer pixel oracle accepted a broken frame")
        blank = Image.new("RGB", (560, 400), (0, 0, 0))
        blank.paste(CANVASES[0], (8, 30, 552, 378))
        try:
            check_refusal(blank)
        except AssertionError as error:
            assert "no visible text" in str(error), error
            print("PASS viewer refusal control:", error)
        else:
            raise AssertionError("blank refusal passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--negative-control", action="store_true", required=True)
    parser.parse_args()
    negative_controls()
