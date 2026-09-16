#!/usr/bin/env python3
"""Verify the A3 GUI facade against native ABI records and call words."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from as_managed_test import ROOT, emit_ir, invoke, sanitized
from as_abi_test import probe_runtime
from as_image_test import DECODE_PROBE

FIXTURE = ROOT / "tests/fixtures/astyped/gui-library/main.as"
LIBRARY = ROOT / "fsroot/as/lib"


def guest_assets():
    # Read the production Make inventory. Missing fonts let rectangles pass
    # while every text draw remained invisible on the first private GUI disk.
    recipe = (
        ".PHONY: aether-gui-assets\naether-gui-assets:\n"
        "\t@printf 'font %s\\n' $(FONTS)\n"
        "\t@printf 'text %s\\n' $(FONT_TEXT)\n"
        "\t@printf 'notice %s\\n' $(RELEASE_NOTICES)\n"
    )
    result = subprocess.run(
        ["make", "--no-print-directory", "-s", "-f", "Makefile", "-f", "-", "aether-gui-assets"],
        input=recipe, cwd=ROOT, capture_output=True, text=True, check=True, timeout=30,
    )
    files = []
    for line in result.stdout.splitlines():
        kind, relative = line.split(" ", 1)
        source = ROOT / relative
        destination = ("/fonts/" + source.name if kind == "font" else
                       "/fonts/text.ttf" if kind == "text" else "/licenses/" + relative)
        files.append((source, destination))
    return files


def guest_frame(path, demo=False):
    """Read actual scanout colors; no window coordinates are assumed."""
    from PIL import Image
    with Image.open(path) as frame:
        rgb = frame.convert("RGB")
        colors = {(52, 120, 246)} if demo else {(241, 42, 115), (33, 220, 169)}
        points = {color: [] for color in colors}
        pixels = rgb.tobytes()
        for offset in range(0, len(pixels), 3):
            index = offset // 3
            color = tuple(pixels[offset:offset + 3])
            if color in points:
                points[color].append((index % rgb.width, index // rgb.width))
        bounds = []
        for color in sorted(colors):
            positions = points[color]
            if not positions:
                raise AssertionError(f"Native GUI color absent: {color}")
            xs, ys = zip(*positions)
            box = (min(xs), min(ys), max(xs) + 1, max(ys) + 1)
            width, height = box[2] - box[0], box[3] - box[1]
            if len(positions) != width * height:
                raise AssertionError(f"Native GUI rectangle is incomplete: {color}, {box}")
            bounds.append({"color": color, "box": box, "pixels": len(positions)})
        if demo:
            width = bounds[0]["box"][2] - bounds[0]["box"][0]
            height = bounds[0]["box"][3] - bounds[0]["box"][1]
            assert width == height and width >= 16, bounds
        else:
            green, pink = (item["box"] for item in bounds)
            scale = (pink[2] - pink[0]) / 40
            assert scale >= 1 and scale.is_integer(), bounds
            assert pink[3] - pink[1] == 30 * scale, bounds
            assert green[2] - green[0] == 24 * scale and green[3] - green[1] == 20 * scale, bounds
            assert green[0] - pink[0] == 60 * scale and green[1] == pink[1], bounds
            text_region = rgb.crop((pink[0], int(pink[1] + 45 * scale),
                                   int(pink[0] + 250 * scale), int(pink[1] + 85 * scale)))
            text_pixels = text_region.tobytes()
            ink = sum(1 for offset in range(0, len(text_pixels), 3)
                      if text_pixels[offset] >= 128
                      and text_pixels[offset] == text_pixels[offset + 1] == text_pixels[offset + 2])
            assert ink > 20, f"Native GUI text has no visible ink: {ink}"
        return bounds


def run_guest_window(guest, command, program, output_directory):
    """Keep the app alive for scanout inspection, then quit using real input."""
    demo = program["name"].startswith("example-guidemo-")
    start = len(guest.log)
    status_path = "/state/gui-exit.txt"
    output_path = "/state/gui-" + program["name"] + ".txt"
    # The shell waits for the foreground app. Its next queued command records
    # that app's status before any diagnostic command can overwrite $?.
    # Serial stdout used to be the readiness signal. Kernel WM diagnostics can
    # split it inside a word (observed "dgamage fui okrom" for "gui ok").
    # Use actual scanout for readiness, then check the complete output file
    # after the process closes it. Open-file metadata is not yet durable on
    # LogitFS, so host reads while the child runs can falsely see zero bytes.
    guest.serial.sendall((command + " > " + output_path +
                          "\n/bin/echo $? > " + status_path + "\n").encode())
    deadline = time.monotonic() + 60
    while True:
        guest.screenshot()
        try:
            bounds = guest_frame(output_directory / "desktop.ppm", demo)
            break
        except AssertionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.2)
    screenshot = output_directory / (program["name"] + ".ppm")
    shutil.copyfile(output_directory / "desktop.ppm", screenshot)
    from PIL import Image
    with Image.open(screenshot) as frame:
        frame.save(screenshot.with_suffix(".png"))
    guest.key("q")
    transcript = bytes(guest.log[start:]).decode(errors="replace")
    status = guest.capture("/bin/cat " + status_path, timeout=30)
    output = guest.capture("/bin/cat " + output_path, timeout=30)
    assert output == program["expected_output"], (program["name"], output)
    program["output"] = output
    program["scanout"] = {"path": str(screenshot), "bounds": bounds}
    program["serial_transcript"] = transcript
    return int(status.strip())

# The kernel ABI header supplies the record layouts and syscall identities.
# Inputs deliberately differ between coordinates, extents and source sizes;
# swapping fields cannot agree accidentally. No GUI implementation is linked.
GUI_PROBE = r'''
int64_t abi_probe(int64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    const uint64_t xy = (uint64_t)(uint16_t)-3 << 16 | 7;
    const uint64_t extent = (uint64_t)23 << 16 | 19;
    const uint64_t window = (uint64_t)400 << 16 | 300;
    const uint64_t color = 0x123456;
    switch (number) {
    case SYS_GUI_CREATE:
        assert(!strcmp((const char *)(uintptr_t)a, "中文") && b == window);
        break;
    case SYS_GUI_CLEAR:
        assert(a == color);
        break;
    case SYS_GUI_RECT:
        assert(a == xy && b == extent && c == color);
        break;
    case SYS_GUI_RRECT:
        assert(a == xy && b == extent && c == (5u << 24 | color));
        break;
    case SYS_GUI_CLIP:
        assert(a == xy && b == extent);
        break;
    case SYS_GUI_TEXT:
        assert(a == xy && b == color && !strcmp((const char *)(uintptr_t)c, "中文"));
        break;
    case SYS_GUI_TEXT_MONO:
        assert(a == xy && b == (8u << 24 | color));
        assert(!strcmp((const char *)(uintptr_t)c, "中文"));
        break;
    case SYS_GUI_ICON:
        assert(a == xy && b == (4u << 16 | 16) && c == color);
        break;
    case SYS_GUI_GLASS:
        assert(a == xy && b == extent && c == ((uint64_t)5 << 32 | 0x01020304));
        break;
    case SYS_TEXT_MEASURE: {
        static int measurements;
        int face = ++measurements == 1 ? LOGIT_FACE_MONO : LOGIT_FACE_MONO | LOGIT_FACE_BOLD;
        /* Decode like the actual kernel branch, independently of the .abi
         * metadata. The old literal 41 repeated its stale one-bit encoding
         * and let a half-size measurement pass the host gate. */
        assert(!strcmp((const char *)(uintptr_t)a, "中文") && b == 6);
        assert((c >> 2) == 20 && (c & 3) == (uint64_t)face);
        break;
    }
    case SYS_GUI_TEXT_RUN: {
        const struct logit_run *run = (const void *)(uintptr_t)a;
        assert(run->x == -3 && run->y == 7 && run->px == 20 && run->mono == 1);
        assert(run->color == color && run->len == 6 && run->bold == 0);
        assert(!memcmp(run->s, "中文", 6));
        break;
    }
    case SYS_GUI_BLIT: {
        const struct logit_blit *blit = (const void *)(uintptr_t)a;
        assert(blit->x == -3 && blit->y == 7 && blit->w == 23 && blit->h == 19);
        assert(blit->sw == 2 && blit->sh == 1 && blit->rgba[0] == 42);
        break;
    }
    case SYS_GUI_WIN_MIN:
        assert(a == window);
        break;
    case SYS_UI_DARK:
        assert(a == UINT64_MAX);
        break;
    case SYS_POLL_EVENT: {
        static int polls;
        struct logit_event *event = (void *)(uintptr_t)a;
        polls++;
        if (polls == 2) return 0;
        event->a = polls == 1 ? 11 : 22;
        return 1;
    }
    case SYS_GUI_FLUSH:
    case SYS_YIELD:
        break;
    default:
        return image_probe(number, a, b, c);
    }
    return 9;
}
'''


def runtime_probe(work):
    image_probe = DECODE_PROBE.replace("int64_t abi_probe(", "int64_t image_probe(")
    runtime = probe_runtime(work, image_probe + GUI_PROBE)
    heap = runtime / "heap.c"
    original = heap.read_text()
    anchor = "void *at_gc_allocate(size_t bytes, AtScan scan)\n{"
    assert original.count(anchor) == 1
    heap.write_text(original.replace(anchor, anchor + "\n    at_gc_collect();"))
    return runtime


def exercise(compiler, negative):
    with tempfile.TemporaryDirectory(prefix="as-native-gui-") as temporary:
        work = Path(temporary)
        runtime = runtime_probe(work)
        ir = work / "main.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, runtime, work / "positive", optimization)
            assert result.returncode == 0 and result.stdout == "native GUI library ok\n", result
        if negative:
            # Actual scanout from the initial private disk: shapes are correct,
            # but fonts were not packaged. A success marker alone passed it.
            try:
                guest_frame(FIXTURE.parent / "no-font.png")
            except AssertionError as error:
                assert "text has no visible ink" in str(error), error
                print("PASS GUI scanout control:", error)
            else:
                raise AssertionError("Fontless guest scanout incorrectly passed")
            library = work / "lib"
            shutil.copytree(LIBRARY, library)
            module = library / "gui.as"
            original = module.read_text()
            for anchor, replacement, label in (
                ("_bl.sw = i32(sw)", "_bl.sw = i32(w)", "source-size"),
                ("_run.len = i32(len(s))", "_run.len = 1", "text-length"),
                ("if sw <= 0 or sh <= 0 or sw > len(rgba) / 4 / sh:", "if false:", "source-bound"),
            ):
                assert original.count(anchor) == 1
                module.write_text(original.replace(anchor, replacement))
                result = invoke([compiler, "build", FIXTURE, "--stdlib", library, "--emit-llvm", "-o", ir])
                assert result.returncode == 0, result
                result = sanitized(ir, runtime, work / "negative", "-O0")
                assert result.returncode != 0 and "assert" in result.stderr.lower(), result
                print("PASS GUI control:", label, "observed failing")
            module.write_text(original)
            abi = library / "abi.as"
            original_abi = abi.read_text()
            anchor = "((px & 0x3FFFFFFF) << 2) | ((mono & 0x3))"
            assert original_abi.count(anchor) == 1
            abi.write_text(original_abi.replace(anchor, "((px & 0x7FFFFFFF) << 1) | ((mono & 1))"))
            result = invoke([compiler, "build", FIXTURE, "--stdlib", library, "--emit-llvm", "-o", ir])
            assert result.returncode == 0, result
            result = sanitized(ir, runtime, work / "old-font-encoding", "-O0")
            assert result.returncode != 0 and "assert" in result.stderr.lower(), result
            print("PASS GUI control: obsolete font packing observed failing")
    print("PASS native GUI: drawing words, owned pixel/text views, events and bounds, O0/O2")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    exercise(args.compiler.resolve(), args.negative_control)
