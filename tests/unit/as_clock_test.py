#!/usr/bin/env python3
"""Validate the shipped clock example using its independent RTC measurement.

Native guest runners and the standalone clock harness share this oracle. It
checks measured values, not a success marker or the host's elapsed wall time.
Private source mutations run the same RTC loop but break monotonic readings;
they never change kernel timers or the production example.
"""

import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "fsroot/as/examples/monotonic.as"
PATTERN = re.compile(r"MONO (\d+) (\d+) (-?\d+) (\d+) (\d+)\nMONO-STEP (\d+) (\d+)\n")


class ClockFailure(AssertionError):
    def __init__(self, reason, output):
        self.reason = reason
        super().__init__(f"{reason}: {output!r}")


def check_clock(output):
    match = PATTERN.fullmatch(output)
    if match is None:
        raise ClockFailure("missing or malformed measurement", output)
    start, end, delta, wall, spins, step_start, step_end = map(int, match.groups())
    if delta != end - start:
        raise ClockFailure("inconsistent delta", output)
    if wall < 4 or spins <= 100:
        raise ClockFailure("no independent interval", output)
    if end <= start:
        raise ClockFailure("clock stopped", output)

    # Preserve the original CMOS quantization window plus 25% TCG slack.
    # This is not a performance measurement. A tick/ms unit error still lies
    # far outside these bounds; none of the numbers come from host timing.
    low = (wall - 1) * 1000 * 3 // 4
    high = (wall + 1) * 1000 * 5 // 4
    if not low <= delta <= high:
        raise ClockFailure("wrong clock rate", output)
    if (step_start, step_end) != (start % 10, end % 10):
        raise ClockFailure("inconsistent step", output)
    if step_start != 0 or step_end != 0:
        raise ClockFailure("clock is not 10 ms aligned", output)
    return {"start_ms": start, "end_ms": end, "delta_ms": delta,
            "wall_seconds": wall, "spins": spins, "allowed_ms": [low, high]}


def expect_rejection(output, reason):
    try:
        check_clock(output)
    except ClockFailure as error:
        if error.reason != reason:
            raise AssertionError(f"Wrong control failure: {error}") from error
        return {"rejected": reason, "output": output}
    raise AssertionError(f"Clock oracle accepted {reason}: {output!r}")


def guest_controls(directory):
    """Buildable private copies; only the two monotonic readings change."""
    original = SOURCE.read_text()
    controls = {}
    for name, expression in (("clock-dead", "0"), ("clock-ticks", "monotonic_ms() / 10")):
        text = original
        for variable in ("t0", "t1"):
            anchor = f"    {variable} = monotonic_ms()\n"
            assert text.count(anchor) == 1, "Clock mutation anchor changed"
            text = text.replace(anchor, f"    {variable} = {expression}\n")
        source = directory / (name + ".as")
        source.write_text(text)
        controls[name] = source
    return controls


def check_guest_clock(name, output):
    if name.startswith("clock-dead-"):
        return expect_rejection(output, "clock stopped")
    if name.startswith("clock-ticks-"):
        return expect_rejection(output, "wrong clock rate")
    if name == "monotonic" or name.startswith("example-monotonic-"):
        return check_clock(output)
    raise AssertionError(f"No measurement oracle registered for {name}")


def negative_controls():
    good = "MONO 1000 5000 4000 4 1000\nMONO-STEP 0 0\n"
    assert check_clock(good)["delta_ms"] == 4000
    cases = (
        ("", "missing or malformed measurement"),
        (good + good, "missing or malformed measurement"),
        (good.replace("5000 4000", "5000 3990"), "inconsistent delta"),
        (good.replace("4 1000", "0 1000"), "no independent interval"),
        (good.replace("4 1000", "4 0"), "no independent interval"),
        ("MONO 0 0 0 4 1000\nMONO-STEP 0 0\n", "clock stopped"),
        ("MONO 100 500 400 4 1000\nMONO-STEP 0 0\n", "wrong clock rate"),
        (good.replace("STEP 0 0", "STEP 1 0"), "inconsistent step"),
        ("MONO 1001 5001 4000 4 1000\nMONO-STEP 1 1\n", "clock is not 10 ms aligned"),
    )
    for output, reason in cases:
        expect_rejection(output, reason)
    print(f"PASS clock oracle: {len(cases)} observed refusals, independent RTC bounds")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    if args.negative_control:
        negative_controls()
    elif args.output:
        result = check_clock(args.output.read_text())
        print(f"PASS native clock: {result['delta_ms']} ms over {result['wall_seconds']} RTC seconds")
    else:
        parser.error("provide a captured output file or --negative-control")
