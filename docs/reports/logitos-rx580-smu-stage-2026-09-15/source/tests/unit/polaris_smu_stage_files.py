#!/usr/bin/env python3
"""Real-file RAM staging oracle. No GPU is modeled or accessed here."""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

EXPECTED = (
    "e27b3ec976f87e32883b9190e653b20d27f9e77da8dd2d77eeb10c35ee9f1e61",
    "23dd73eb1316d40a681841b4d3d116c5cae6d8317cebe8f5e1f9e5dec6de5c87",
)


def validate(image, files, base):
    # These literal sizes/offsets are independent of stage_info and its header:
    # fixed linux-firmware 1522c78... input pair, 12436 bytes each, 4K pages.
    if len(image) != 36864 or struct.unpack_from("<II", image) != (1, 2):
        raise ValueError("header-layout")
    for i, offset in enumerate((4096, 20480)):
        entry = struct.unpack_from("<HHIIIIIHH", image, 8 + i * 28)
        expected = (i + 1, 58, (base + offset) >> 32,
                    (base + offset) & 0xffffffff, 0, 0, 12436, 0, 0)
        if entry != expected:
            raise ValueError("entry-layout")
        if image[offset:offset + 12436] != files[i][256:]:
            raise ValueError("payload-mismatch")
        if any(image[offset + 12436:offset + 16384]):
            raise ValueError("payload-padding")
    if any(image[64:4096]):
        raise ValueError("toc-padding")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--tool", required=True)
    p.add_argument("--negative-tool", required=True)
    p.add_argument("--firmware-dir", required=True)
    p.add_argument("--out", required=True)
    args = p.parse_args()
    directory, out = Path(args.firmware_dir).resolve(), Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    inputs = [directory / "polaris10_sdma.bin", directory / "polaris10_sdma1.bin"]
    files = [f.read_bytes() for f in inputs]
    hashes = [hashlib.sha256(f).hexdigest() for f in files]
    if tuple(hashes) != EXPECTED:
        raise ValueError("expected pinned upstream firmware pair; this gate does not fetch it")
    base = 0x120000000

    def run(tool, destination, address, sources=inputs, expect=0):
        result = subprocess.run([str(Path(tool).resolve()), *map(str, sources),
            str(destination), hex(address)], capture_output=True, text=True)
        if result.returncode != expect:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout + result.stderr

    # Same oracle must catch a kernel implementation mutation on real payloads.
    negative = out / "corrupted-stage.bin"
    run(args.negative_tool, negative, base)
    try:
        validate(negative.read_bytes(), files, base)
    except ValueError as exc:
        if str(exc) != "payload-mismatch":
            raise
    else:
        raise AssertionError("corrupted payload escaped the oracle")
    destination = out / "sdma-partial-stage.bin"
    log = run(args.tool, destination, base)
    data = destination.read_bytes()
    validate(data, files, base)
    rebased = out / "rebased-stage.bin"
    run(args.tool, rebased, 0x340000000)
    second = rebased.read_bytes()
    validate(second, files, 0x340000000)
    assert second != data and second[4096:] == data[4096:]
    # Reject a truncated v1.1 header without truncating an existing output.
    truncated = out / "truncated-firmware.bin"
    truncated.write_bytes(files[0][:48])
    log += run(args.tool, destination, base, [truncated, inputs[1]], expect=1)
    assert destination.read_bytes() == data
    (out / "tool.log").write_text(log)
    result = dict(passed=True, validation="host-ram-only", firmware_sha256=hashes,
        stage_sha256=hashlib.sha256(data).hexdigest(), stage_bytes=len(data),
        present_mask="0x6", missing_mask="0x478", proposed_gpu_base=hex(base),
        negative_control="payload-mismatch", truncated_file_preserves_output=True,
        rebasing_changes_only_toc=True, gpu_mapped=False, uploaded=False,
        firmware_loaded=False, physical_rx580_verified=False)
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("POLARIS_SMU_STAGE_FILES: PASS exact payloads/TOC/padding, corruption rejected; GPU not accessed")


if __name__ == "__main__":
    main()
