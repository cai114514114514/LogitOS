#!/usr/bin/env python3
"""Native image library operations and independent guest pixel references."""

import argparse
import hashlib
import io
import json
from fractions import Fraction
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from as_managed_test import ROOT, RUNTIME, emit_ir, invoke, sanitized
from as_abi_test import probe_runtime

LIBRARY = ROOT / "fsroot/as/lib"
FIXTURE = ROOT / "tests/fixtures/astyped/image-library/main.as"

DECODE_SOURCE = '''# aether: 3.0
import std.image as image
def main() -> None:
    for path in ["/ok", "/retry", "/huge"]:
        picture = image.decode(("prefix" + path + "suffix").slice(6, 6 + len(path)))
        assert picture.w == 1 and picture.h == 1
        assert picture.at(0, 0) == 0x010203FF
        assert picture.format == ("?" if path == "/huge" else "PNG")
    assert image.stat_of("/missing") is None
    assert image.refusal("") != ""
    assert image.refusal("/bad" + chr(0)) != ""
    failures = 0
    for path in ["/missing", "/directory", "/empty", "/readfail", "/unknown", "/svg", "/reject", "/bad-dim"]:
        try:
            image.decode(path)
        except Error as error:
            assert path in error.message
            if path == "/bad-dim":
                assert "kernel reported" in error.message
            failures += 1
    assert failures == 8
    print("native image decode transport ok")
'''

DECODE_PROBE = '''#include "abi/logit_abi.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

int64_t abi_probe(int64_t number, uint64_t a, uint64_t b, uint64_t c)
{
    const char *path = (const char *)(uintptr_t)a;
    if (number == SYS_STAT) {
        if (!strcmp(path, "/missing")) return -1;
        struct logit_stat *record = (struct logit_stat *)(uintptr_t)b;
        assert(c == sizeof *record);
        record->mode = !strcmp(path, "/directory") ? LST_IFDIR : LST_IFREG;
        record->size = !strcmp(path, "/empty") ? 0 : !strcmp(path, "/huge") ? 5000000 : 8;
        return 0;
    }
    if (number == SYS_READ_FILE) {
        if (!strcmp(path, "/readfail")) return -1;
        assert(c == 8);
        unsigned char data[] = {137, 80, 78, 71, 13, 10, 26, 10};
        if (!strcmp(path, "/unknown")) data[0] = 0;
        if (!strcmp(path, "/svg")) data[0] = '<';
        memcpy((void *)(uintptr_t)b, data, 8);
        return 8;
    }
    assert(number == SYS_IMG_DECODE);
    struct logit_imgreq *request = (struct logit_imgreq *)(uintptr_t)a;
    path = request->path;
    assert(!strcmp(path, "/ok") || !strcmp(path, "/retry") || !strcmp(path, "/huge") ||
           !strcmp(path, "/reject") || !strcmp(path, "/bad-dim"));
    assert(request->max == 5242880 || request->max == 10485760 || request->max == 20971520);
    if (!strcmp(path, "/reject")) return -1;
    if (!strcmp(path, "/retry") && request->max < 10485760) return -1;
    request->w = !strcmp(path, "/bad-dim") ? 2147483647 : 1;
    request->h = !strcmp(path, "/bad-dim") ? 2147483647 : 1;
    request->rgba[0] = 1;
    request->rgba[1] = 2;
    request->rgba[2] = 3;
    request->rgba[3] = 255;
    return 0;
}
'''


def decode_runtime(work):
    runtime = probe_runtime(work, DECODE_PROBE)
    heap = runtime / "heap.c"
    original = heap.read_text()
    anchor = "void *at_gc_allocate(size_t bytes, AtScan scan)\n{"
    assert original.count(anchor) == 1
    heap.write_text(original.replace(anchor, anchor + "\n    at_gc_collect();"))
    return runtime


def guest_images(work):
    # Match the independent codec gates, including JPEG's box chroma sampling.
    # Pillow's default smooth sampling previously produced false JPEG failures
    # after all five lossless formats had passed in the guest.
    from PIL import Image, ImageOps
    jpeg_gate = (ROOT / "tests/unit/jpeg_test.c").read_text()
    max_difference = int(re.search(r"#define MAX_TOL\s+(\d+)", jpeg_gate)[1])
    mean_difference = Fraction(re.search(r"#define MEAN_TOL\s+([\d.]+)", jpeg_gate)[1])
    djpeg = shutil.which("djpeg")
    if djpeg is None:
        raise RuntimeError("JPEG guest reference requires djpeg (libjpeg-turbo)")
    files = []
    records = []
    lines = ["# aether: 3.0", "import std.image as image", "def main() -> None:"]
    formats = {"still.bmp": "BMP", "still.webp": "WebP", "icon.ico": "ICO",
               "anim.gif": "GIF", "anim.apng": "PNG", "rot.jpg": "JPEG"}
    for name, format in formats.items():
        source = ROOT / "tests/fixtures/image" / name
        destination = "/state/" + name
        with Image.open(source) as reference:
            reference.seek(0)
            if format == "JPEG":
                decoded = subprocess.run(
                    [djpeg, "-nosmooth", "-dct", "int", "-pnm", str(source)],
                    capture_output=True, check=True,
                ).stdout
                # djpeg emits unrotated PPM. Preserve the original EXIF tag so
                # the same orientation is applied to the independent pixels.
                reference_pixels = Image.open(io.BytesIO(decoded))
                reference_pixels.getexif()[274] = reference.getexif().get(274, 1)
                rgba = ImageOps.exif_transpose(reference_pixels).convert("RGBA")
            else:
                rgba = ImageOps.exif_transpose(reference).convert("RGBA")
            width, height = rgba.size
            pixels = rgba.tobytes()
        oracle = work / (name + ".rgba")
        oracle.write_bytes(pixels)
        files.extend([(source, destination), (oracle, destination + ".rgba")])
        records.append({"path": destination, "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                        "oracle_sha256": hashlib.sha256(pixels).hexdigest(), "width": width, "height": height,
                        "oracle": "djpeg -nosmooth -dct int" if format == "JPEG" else "Pillow first frame"})
        lines.extend([
            f'    picture = image.decode("prefix{destination}suffix".slice(6, {6 + len(destination)}))',
            f'    assert picture.format == "{format}"',
            f"    assert picture.w == {width} and picture.h == {height}",
            f'    expected = file_read("{destination}.rgba")',
            "    gc_collect()",
            "    total_difference = 0",
            "    for index in range(len(expected)):",
            "        difference = picture.rgba[index] - expected[index]",
            f"        assert difference >= {-max_difference if format == 'JPEG' else 0} and difference <= {max_difference if format == 'JPEG' else 0}",
        ])
        if format == "JPEG":
            lines.extend([
                "        if index % 4 == 3:",
                "            assert difference == 0",
                "        else:",
                "            total_difference += -difference if difference < 0 else difference",
                f"    assert total_difference * {mean_difference.denominator} <= {width * height * 3 * mean_difference.numerator}",
            ])
    lines.append('    print("native image guest pixels ok")')
    source = work / "image-pixels.as"
    source.write_text("\n".join(lines) + "\n")
    return source, files, records


def exercise(compiler):
    with tempfile.TemporaryDirectory(prefix="as-native-image-") as temporary:
        work = Path(temporary)
        ir = work / "main.ll"
        emit_ir(compiler, FIXTURE, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, RUNTIME, work / "operations", optimization)
            assert result.returncode == 0 and result.stdout == "native image operations ok\n", result
        source = work / "decode.as"
        source.write_text(DECODE_SOURCE)
        runtime = decode_runtime(work)
        emit_ir(compiler, source, ir)
        for optimization in ("-O0", "-O2"):
            result = sanitized(ir, runtime, work / "decode", optimization)
            assert result.returncode == 0 and result.stdout == "native image decode transport ok\n", result
    print("PASS native image: formats, pixels, geometry, decode retries/errors and forced GC, O0/O2")


def negative_controls(compiler):
    with tempfile.TemporaryDirectory(prefix="as-image-controls-") as temporary:
        work = Path(temporary)
        library = work / "lib"
        shutil.copytree(LIBRARY, library)
        module = library / "image.as"
        original = module.read_text()
        source = work / "decode.as"
        source.write_text(DECODE_SOURCE)
        runtime = decode_runtime(work)
        cases = [
            (FIXTURE, RUNTIME, "red << 24", "blue << 24", "channels"),
            (source, runtime, "Bytes(absolute + chr(0))", 'Bytes(absolute + "suffix" + chr(0))', "path-copy"),
            (source, runtime, "if width <= 0 or height <= 0 or width > budget / 4 / height:", "if false:", "dimensions"),
        ]
        for entry, transport, anchor, replacement, label in cases:
            ir = work / "main.ll"
            module.write_text(original)
            result = invoke([compiler, "build", entry, "--stdlib", library, "--emit-llvm", "-o", ir])
            assert result.returncode == 0, result
            result = sanitized(ir, transport, work / "positive", "-O0")
            assert result.returncode == 0, result
            assert original.count(anchor) == 1
            module.write_text(original.replace(anchor, replacement))
            result = invoke([compiler, "build", entry, "--stdlib", library, "--emit-llvm", "-o", ir])
            assert result.returncode == 0, result
            result = sanitized(ir, transport, work / "negative", "-O0")
            assert result.returncode != 0 and "assert" in result.stderr.lower(), result
            print("PASS image control:", label, "observed failing")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    (negative_controls if args.negative_control else exercise)(args.compiler.resolve())
