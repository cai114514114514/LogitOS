"""Independent PCM evidence for the unmodified sndtest ramp producer.

The direct-rate cards also use the original frame-index checker. ES1370's
integer clock divider requires resampling, so its independent oracle measures
rising slopes and interpolated zero-crossing spacing instead of demanding an
impossible one-source-frame-per-output-frame mapping.
"""
import contextlib
import importlib.util
import io
import math
import struct
from pathlib import Path

RATES = {"intel-hda": 48000, "AC97": 48000, "ES1370": 48662}
CHECKER = Path(__file__).resolve().parents[3] / "boot" / "audio_check.py"
spec = importlib.util.spec_from_file_location("legacy_audio_check", CHECKER)
legacy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(legacy)


def inspect(path, rate, *, allow_streaming_header=False):
    data = Path(path).read_bytes()
    errors = []
    metrics = {}
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return {"passed": False, "errors": ["invalid RIFF/WAVE header"]}
    streaming_header = (allow_streaming_header and data[4:8] == bytes(4) and
                        data[12:20] == b"fmt \x10\x00\x00\x00" and
                        data[36:44] == b"data" + bytes(4))
    metrics["header_finalized"] = not streaming_header
    if not streaming_header and struct.unpack_from("<I", data, 4)[0] + 8 != len(data):
        return {"passed": False, "errors": ["RIFF length is not finalized"]}
    offset, format_info, pcm = 12, None, None
    while offset + 8 <= len(data):
        kind, length = struct.unpack_from("<4sI", data, offset)
        if streaming_header and offset == 36:
            length = len(data) - 44
        end = offset + 8 + length
        if end > len(data):
            return {"passed": False, "errors": ["truncated WAV chunk"]}
        body = data[offset + 8:end]
        if kind == b"fmt " and len(body) >= 16:
            format_info = struct.unpack_from("<HHIIHH", body)
        elif kind == b"data":
            pcm = body
        offset = end + (length & 1)
    expected_format = (1, 2, rate, rate * 4, 4, 16)
    if format_info != expected_format or pcm is None or len(pcm) % 4:
        return {"passed": False, "errors": ["expected complete stereo s16 PCM at native rate"],
                "format": format_info}
    values = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    left, right = values[::2], values[1::2]
    start, end = legacy.trim_silence(left)
    left, right = left[start:end], right[start:end]
    metrics.update(captured_frames=len(pcm) // 4, signal_frames=len(left), rate=rate)
    # 64 frames allow startup/tail rounding, not a missing 1024-frame period.
    if abs(len(left) - rate) > 64:
        errors.append("one-second signal length differs by more than 64 frames")
    if len(left) < rate // 4:
        return {"passed": False, "errors": errors + ["silent or too short"], "metrics": metrics}
    metrics.update(minimum=min(left), maximum=max(left))
    if min(left) > -28000 or max(left) < 28000:
        errors.append("signal amplitude is missing or attenuated")
    if any(abs(a + b) > 3 for a, b in zip(left[8:-8], right[8:-8])):
        errors.append("left and right are not opposite-polarity samples")
    crossings = []
    bad_slopes = 0
    expected_slope = 256 * 48000 / rate
    for index in range(9, len(left) - 8):
        previous, current = left[index - 1:index + 1]
        if previous <= 0 < current:
            crossings.append(index - 1 - previous / (current - previous))
        # Exclude the sawtooth discontinuity and its interpolation neighbours.
        # Every remaining ascending sample must retain the intended direction
        # and slope; dropped/repeated samples and swapped channels fail here.
        if abs(previous) < 26000 and abs(current) < 26000:
            if abs((current - previous) - expected_slope) > 3:
                bad_slopes += 1
    intervals = [b - a for a, b in zip(crossings, crossings[1:])]
    metrics.update(zero_crossings=len(crossings), bad_slopes=bad_slopes)
    if bad_slopes:
        errors.append("ramp slope/order disagrees with the source clock")
    if not intervals or any(abs(value - rate / 200) > 0.05 for value in intervals):
        errors.append("200 Hz cycle spacing is discontinuous or has wrong speed")
    if intervals:
        metrics["frequency_hz"] = rate / (sum(intervals) / len(intervals))
    if rate == 48000:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            direct_ok = legacy.check_ramp(left, right)
        if not direct_ok:
            errors.append("original exact frame-index checker rejected capture")
        metrics["direct_checker"] = output.getvalue()
    return {"passed": not errors, "errors": errors, "metrics": metrics}


def fixture(path, rate, *, frequency=200, silence=False, swapped=False, shortened=False,
            repeated=False):
    frames = rate // 2 if shortened else rate
    signal = []
    for index in range(frames):
        source_position = index * frequency * 240 / rate
        lower = math.floor(source_position)
        fraction = source_position - lower
        a = (lower % 240) * 256 - 30720
        b = ((lower + 1) % 240) * 256 - 30720
        value = round(a + (b - a) * fraction)
        signal.append(0 if silence else value)
    if repeated:
        signal[2048:3072] = signal[1024:2048]
    if swapped:
        signal = [-value for value in signal]
    pcm = b"".join(struct.pack("<hh", value, -value) for value in signal)
    header = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(pcm), b"WAVE",
                         b"fmt ", 16, 1, 2, rate, rate * 4, 4, 16, b"data", len(pcm))
    Path(path).write_bytes(header + pcm)


def self_test(directory):
    directory.mkdir(parents=True, exist_ok=True)
    results = []
    for rate in sorted(set(RATES.values())):
        for name, options in (("positive", {}), ("silence", {"silence": True}),
                              ("wrong-speed", {"frequency": 180}),
                              ("swapped", {"swapped": True}),
                              ("short", {"shortened": True}),
                              ("repeated-period", {"repeated": True}),
                              ("damaged", {}), ("wrong-rate", {}), ("streaming-positive", {}),
                              ("streaming-damaged", {}), ("streaming-short", {"shortened": True}),
                              ("only-riff-zero", {}), ("only-data-zero", {})):
            path = directory / f"{rate}-{name}.wav"
            fixture(path, rate, **options)
            if name == "damaged":
                path.write_bytes(path.read_bytes()[:-7])
            if name.startswith("streaming-"):
                contents = bytearray(path.read_bytes())
                contents[4:8] = contents[40:44] = bytes(4)
                if name == "streaming-damaged":
                    del contents[-7:]
                path.write_bytes(contents)
            if name in ("only-riff-zero", "only-data-zero"):
                contents = bytearray(path.read_bytes())
                offset = 4 if name == "only-riff-zero" else 40
                contents[offset:offset + 4] = bytes(4)
                path.write_bytes(contents)
            expected_rate = rate + 100 if name == "wrong-rate" else rate
            result = inspect(path, expected_rate, allow_streaming_header=True)
            expected_pass = name in ("positive", "streaming-positive")
            correct = result["passed"] == expected_pass
            results.append({"case": f"{rate}-{name}", "expected_pass": expected_pass,
                            "control_ok": correct, "observation": result})
    return {"passed": all(case["control_ok"] for case in results), "cases": results}
