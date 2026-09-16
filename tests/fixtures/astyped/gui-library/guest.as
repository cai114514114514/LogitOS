# aether: 3.0
import std.gui as gui


def main() -> None:
    assert gui.create("Native A3 GUI", 320, 200) == 0
    gui.win_min(200, 120)
    gui.clear(0x101010)
    gui.rect(20, 40, 40, 30, 0xF12A73)
    pixels = buffer(16)
    for index in range(4):
        pixels[index * 4] = 0x21
        pixels[index * 4 + 1] = 0xDC
        pixels[index * 4 + 2] = 0xA9
        pixels[index * 4 + 3] = 255
    gui.blit(80, 40, 24, 20, pixels, 2, 2)
    gui.text_px(20, 90, 16, 1, 0xFFFFFF, "A3 native pixels")
    gui.flush()
    print("native GUI painted")
    while true:
        event = gui.poll()
        if event is not None:
            if i64(event.type) == EV_KEY and event.a == 113:
                break
        gui.yield_()
    print("native GUI key ok")
