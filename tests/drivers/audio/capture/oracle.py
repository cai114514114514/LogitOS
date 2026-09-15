"""Compare ADC files against every external PCM sample supplied to QEMU.

The match may start after an initial hardware period, but once recording starts
no missing, reordered or substituted sample is tolerated. Backend enable events
bound the search to the corresponding open/close interval, preventing a stale
recording from satisfying the next open.
"""
import argparse
import array
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import tempfile
import wave


def pcm_wav(path):
    data = path.read_bytes()
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("invalid WAV header")
    if struct.unpack_from("<I", data, 4)[0] + 8 != len(data):
        raise ValueError("unfinished/truncated WAV")
    with wave.open(str(path)) as reader:
        if (reader.getnchannels(), reader.getsampwidth(), reader.getcomptype()) != (2, 2, "NONE"):
            raise ValueError("capture is not stereo s16 PCM")
        rate = reader.getframerate()
        frames = reader.getnframes()
        pcm = reader.readframes(frames)
    if len(pcm) != frames * 4:
        raise ValueError("incomplete PCM frames")
    return rate, pcm


def write_wav(path, rate, pcm):
    with wave.open(str(path), "wb") as writer:
        writer.setnchannels(2)
        writer.setsampwidth(2)
        writer.setframerate(rate)
        writer.writeframes(pcm)


def samples(pcm):
    if len(pcm) % 4:
        raise ValueError("partial stereo frame")
    values = array.array("h", pcm)
    if sys.byteorder != "little":
        values.byteswap()
    return values


def intervals(text, direction):
    opened = None
    result = []
    for order, line in enumerate(text.splitlines()):
        match = re.match(rf"BACKEND_{direction} SetEnabled bytes=(\d+) \(\d+, (true|false)\)", line)
        if not match:
            continue
        position, enabled = int(match[1]), match[2] == "true"
        if enabled:
            if opened is not None:
                raise ValueError("duplicate input/output enable without stop")
            opened = (position, order)
        elif opened is not None:
            result.append((opened[0], position, opened[1], order))
            opened = None
    if opened is not None:
        raise ValueError("input/output has no confirmed stop")
    return result


def playback(pcm, rate):
    values = samples(pcm)
    left, right = values[::2], values[1::2]
    start, end = 0, len(left)
    while start < end and abs(left[start]) < 200:
        start += 1
    while end > start and abs(left[end - 1]) < 200:
        end -= 1
    left, right = left[start:end], right[start:end]
    errors = []
    if abs(len(left) - rate * 3) > 192:
        errors.append("duplex playback did not retain its three-second length")
    if len(left) < rate:
        return {"passed": False, "errors": errors + ["playback is silent or too short"]}
    if min(left) > -28000 or max(left) < 28000:
        errors.append("playback amplitude is wrong")
    if any(abs(a + b) > 3 for a, b in zip(left[8:-8], right[8:-8])):
        errors.append("playback channel polarity is wrong")
    crossings, bad = [], 0
    for index in range(9, len(left) - 8):
        previous, current = left[index - 1:index + 1]
        if previous <= 0 < current:
            crossings.append(index - 1 - previous / (current - previous))
        if abs(previous) < 26000 and abs(current) < 26000:
            bad += abs(current - previous - 256 * 48000 / rate) > 3
    if bad:
        errors.append("playback dropped/reordered/repeated sample steps")
    if len(crossings) < 590 or any(abs(b - a - rate / 200) > 0.05
                                  for a, b in zip(crossings, crossings[1:])):
        errors.append("playback frequency/phase continuity changed during capture")
    return {"passed": not errors, "errors": errors, "signal_frames": len(left),
            "zero_crossings": len(crossings), "bad_steps": bad}


def inspect(directory, rate, expected_volume=None, output_rate=None):
    if output_rate is None:
        output_rate = rate
    errors, rounds = [], []
    try:
        source = (directory / "source.pcm").read_bytes()
        injected = (directory / "injected.pcm").read_bytes()
        log = (directory / "backend.log").read_text()
        input_intervals = intervals(log, "IN")
        output_intervals = intervals(log, "OUT")
        if len(input_intervals) != 2:
            raise ValueError("expected two actual ADC enable/disable intervals")
        if expected_volume is not None:
            volume_line = f"false, [0x{expected_volume:02x}, 0x{expected_volume:02x}]"
            if not any("BACKEND_IN SetVolume" in line and volume_line in line
                       for line in log.splitlines()):
                errors.append("expected QEMU input gain was not requested")
        # Prove that the backend sent the provided source, with only the card's
        # explicit D-Bus gain applied. This prevents a fabricated injected trace
        # from agreeing with a fabricated/stale guest file by construction.
        original = samples(source[:len(injected)])
        actual = samples(injected)
        gain = 255 if expected_volume is None else expected_volume
        if len(original) != len(actual) or any(int(a * gain / 255) != b
                                              for a, b in zip(original, actual)):
            errors.append("injected samples differ from the source and declared gain")
        for index, (begin, stop, _, _) in enumerate(input_intervals, 1):
            captured_rate, pcm = pcm_wav(directory / f"capture{index}.wav")
            if captured_rate != rate or len(pcm) != rate * 4:
                errors.append(f"round {index}: expected exactly one native-rate second")
            values = samples(pcm)
            if not values or max(map(abs, values), default=0) < 1000:
                errors.append(f"round {index}: capture is silent")
            position = injected.find(pcm, begin, stop)
            if not pcm or position < 0 or position % 4:
                errors.append(f"round {index}: complete stereo sample trajectory does not match its injection interval")
            if position >= begin + 32768:
                errors.append(f"round {index}: initial loss exceeds one hardware ring")
            rounds.append({"round": index, "frames": len(pcm) // 4,
                           "match_byte_offset": position, "input_interval": [begin, stop],
                           "sha256": hashlib.sha256(pcm).hexdigest()})
        second = input_intervals[1]
        if not any(start_order < second[2] < second[3] < stop_order
                   for _, _, start_order, stop_order in output_intervals):
            errors.append("second ADC open/close did not occur wholly inside active playback")
        output_check = playback((directory / "playback.pcm").read_bytes(), output_rate)
        errors.extend(output_check["errors"])
    except (OSError, ValueError, wave.Error) as error:
        errors.append(str(error))
        output_check = None
    return {"passed": not errors, "errors": errors, "rounds": rounds,
            "playback": output_check}


def self_test():
    results = []
    with tempfile.TemporaryDirectory(prefix="adc-oracle-") as temporary:
        directory = Path(temporary)
        # The last profile uses independent ADC/DAC clocks, as ES1370 capture
        # with an HDA output does. A wrong-output-rate control must fail even
        # when both WAVs and every injected input sample are otherwise correct.
        for rate, gain, output_rate in ((48000, 136, 48000), (48662, None, 48662),
                                       (48000, None, 48000), (48662, None, 48000)):
            state, original = 0x9f31a257, array.array("h")
            for _ in range(rate * 4):
                state = (state * 1664525 + 1013904223) & 0xffffffff
                original.append(((state >> 16) % 1601 - 800) * 15)
            transmitted = array.array("h", (int(value * (gain or 255) / 255) for value in original))
            if sys.byteorder != "little":
                original.byteswap()
                transmitted.byteswap()
            input_bytes = transmitted.tobytes()
            (directory / "source.pcm").write_bytes(original.tobytes())
            (directory / "injected.pcm").write_bytes(input_bytes)
            output_values = array.array("h")
            for index in range(output_rate * 3):
                position = index * 48000 / output_rate
                lower, fraction = int(position), position % 1
                a, b = (lower % 240) * 256 - 30720, ((lower + 1) % 240) * 256 - 30720
                value = round(a + (b - a) * fraction)
                output_values.extend((value, -value))
            if sys.byteorder != "little":
                output_values.byteswap()
            playback_bytes = output_values.tobytes()
            for case in ("positive", "silence", "swapped", "reordered", "repeated",
                         "short", "wrong-rate", "stale-reopen", "no-stop", "not-duplex",
                         "playback-corrupt", "injected-corrupt", "wrong-output-rate"):
                first, second = input_bytes[:rate * 4], input_bytes[rate * 4:]
                if case == "silence":
                    second = bytes(len(second))
                elif case == "swapped":
                    second = b"".join(second[i + 2:i + 4] + second[i:i + 2] for i in range(0, len(second), 4))
                elif case == "reordered":
                    second = second[4096:8192] + second[:4096] + second[8192:]
                elif case == "repeated":
                    second = second[:4096] + second[:4096] + second[8192:]
                elif case == "short":
                    second = second[:-4096]
                elif case == "stale-reopen":
                    second = first
                write_wav(directory / "capture1.wav", rate, first)
                write_wav(directory / "capture2.wav", rate + (case == "wrong-rate"), second)
                log = (f"BACKEND_IN SetVolume bytes=0 (1, false, [0x{(gain or 255):02x}, 0x{(gain or 255):02x}])\n"
                       "BACKEND_IN SetEnabled bytes=0 (1, true)\n"
                       f"BACKEND_IN SetEnabled bytes={rate * 4} (1, false)\n"
                       "BACKEND_OUT SetEnabled bytes=0 (2, true)\n"
                       f"BACKEND_IN SetEnabled bytes={rate * 4} (1, true)\n"
                       f"BACKEND_IN SetEnabled bytes={rate * 8} (1, false)\n"
                       f"BACKEND_OUT SetEnabled bytes={output_rate * 12} (2, false)\n")
                if case == "no-stop":
                    log = log.replace(f"BACKEND_IN SetEnabled bytes={rate * 8} (1, false)\n", "")
                if case == "not-duplex":
                    log = "\n".join(line for line in log.splitlines() if "BACKEND_OUT" not in line)
                (directory / "backend.log").write_text(log)
                (directory / "playback.pcm").write_bytes(bytes(len(playback_bytes)) if case == "playback-corrupt" else playback_bytes)
                (directory / "injected.pcm").write_bytes(bytes(len(input_bytes)) if case == "injected-corrupt" else input_bytes)
                checked_output_rate = output_rate
                if case == "wrong-output-rate":
                    checked_output_rate = 48662 if output_rate == 48000 else 48000
                observation = inspect(directory, rate, gain, checked_output_rate)
                correct = observation["passed"] == (case == "positive")
                results.append({"case": f"{rate}-gain{gain or 255}-out{output_rate}-{case}", "control_ok": correct,
                                "expected_pass": case == "positive", "observation": observation})
    return {"passed": all(result["control_ok"] for result in results), "cases": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--directory", type=Path)
    parser.add_argument("--rate", type=int, default=48000)
    parser.add_argument("--volume", type=int)
    parser.add_argument("--output-rate", type=int)
    args = parser.parse_args()
    result = self_test() if args.self_test else inspect(args.directory, args.rate, args.volume, args.output_rate)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
