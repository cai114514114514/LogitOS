# aether: 3.0
import std.image as image


def main() -> None:
    expected = parse_int(args()[1])
    assert caps().bits() == expected
    path = "/state/still.bmp"
    reason = image.refusal(path)
    allowed = (expected & CAP_FS_READ) != 0 and (expected & CAP_RAW) != 0
    if allowed:
        assert reason == ""
        picture = image.decode(path)
        assert picture.w == 40 and picture.h == 28
        # A sibling beginning with the same characters is outside /state.
        assert "scope" in image.refusal("/state-outside/fixture")
        path = "/state-outside/fixture"
    elif (expected & CAP_FS_READ) == 0:
        assert "CAP_FS_READ" in reason
    else:
        assert "CAP_RAW" in reason
    denied = false
    try:
        image.decode(path)
    except PermissionError:
        denied = true
    assert denied
    print("native image grants ok")
