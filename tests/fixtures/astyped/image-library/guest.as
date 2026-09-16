# aether: 3.0
import std.image as image
from std.sys import chdir


def check_relative() -> None:
    assert chdir("/state") == 0
    picture = image.decode("./still.bmp")
    assert picture.w == 40 and picture.h == 28
    record = image.stat_of("/state")
    if record is None:
        raise AssertionError("missing state directory")
    assert image.is_dir(record)


def check_pressure() -> None:
    # This guest uses mini-libc's default 24 MiB arena. Keep one valid image
    # live while the invalid input reaches its 20 MiB retry; the failure must
    # preserve both MemoryError and the input path, not blame the decoder.
    picture = image.decode("still.bmp")
    refused = false
    try:
        image.decode("short-image")
    except MemoryError as error:
        assert "short-image" in error.message and "allocate" in error.message
        refused = true
    assert refused and picture.w == 40


def main() -> None:
    # End the first image owner's scope before exercising the 20 MiB retry.
    # Its 5 MiB capacity plus that retry exceeds the guest's 24 MiB arena.
    check_relative()
    failures = 0
    # These are ordinary missing/empty/unsupported inputs, never a fabricated
    # successful decode. The short PNG has only a signature and must be refused.
    for path in ["missing-image", "/state", "empty-image", "plain-image", "vector-image", "short-image"]:
        try:
            image.decode(path)
        except Error as error:
            assert path in error.message
            failures += 1
    assert failures == 6
    check_pressure()
    print("native image guest errors ok")
