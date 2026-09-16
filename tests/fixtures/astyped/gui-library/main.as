# aether: 3.0
import std.gui as gui
from std.image import Image


def main() -> None:
    # Interior views retain suffix bytes in the owner. The string ABIs copy
    # only the requested title/text; the text-run ABI uses its explicit length.
    label = "prefix中文suffix".slice(6, 12)
    assert gui.create(label, 400, 300) == 9
    assert gui.clear(0x123456) == 9
    assert gui.rect(-3, 7, 23, 19, 0x123456) == 9
    assert gui.rrect(-3, 7, 23, 19, 5, 0x123456) == 9
    assert gui.clip(-3, 7, 23, 19) == 9
    assert gui.text(-3, 7, 0x123456, label) == 9
    assert gui.text_mono(-3, 7, 8, 0x123456, label) == 9
    assert gui.icon(-3, 7, 4, 16, 0x123456) == 9
    assert gui.glass(-3, 7, 23, 19, 5, 1, 2, 3, 4) == 9
    assert gui.measure(label, 20, 1) == 9
    assert gui.measure(label, 20, 3) == 9
    assert gui.text_px(-3, 7, 20, 1, 0x123456, label) == 9
    assert gui.win_min(400, 300) == 9
    assert gui.dark() == 9 and gui.UI_PX == 16
    assert gui.flush() == 9 and gui.yield_() == 9

    data = buffer(8)
    data[0] = 42
    assert gui.blit(-3, 7, 23, 19, data, 2, 1) == 9
    assert gui.blit(-3, 7, 23, 19, Bytes(data), 2, 1) == 9
    picture = Image("local", "RGBA", 2, 1, data)
    assert gui.blit_image(picture, -3, 7, 23, 19) == 9
    assert gui.picture("/ok").at(0, 0) == 0x010203FF

    event = gui.poll()
    if event is None:
        raise AssertionError("expected first event")
    assert event.a == 11
    assert gui.poll() is None
    gc_collect()
    latest = gui.poll()
    if latest is None:
        raise AssertionError("expected second event")
    # The documented event is reused, not a new allocation on every poll.
    assert latest.a == 22 and event.a == 22

    errors = 0
    try:
        gui.blit(0, 0, 1, 1, buffer(3), 1, 1)
    except ValueError:
        errors += 1
    try:
        gui.blit(0, 0, 1, 1, data, 0, 1)
    except ValueError:
        errors += 1
    try:
        gui.blit(2147483648, 0, 1, 1, data, 2, 1)
    except ConversionError:
        errors += 1
    assert errors == 3
    print("native GUI library ok")
