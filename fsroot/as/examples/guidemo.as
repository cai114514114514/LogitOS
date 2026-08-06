# guidemo -- a REAL windowed Logit app written in AetherScript (M23.5 lib/gui.as).
# A bouncing ball you can quit with q/ESC or the window's close button.
import gui

W = 360
H = 240
gui.create("AS Ball", W, H)

x = 40
y = 40
dx = 3
dy = 2
frames = 0
running = true

while running:
    ev = gui.poll()
    while ev != nil:
        if ev.type == EV_CLOSE:
            running = false
        if ev.type == EV_KEY and (ev.a == 113 or ev.a == 27):
            running = false
        ev = gui.poll()

    x = x + dx
    y = y + dy
    if x < 8 or x > W - 24:
        dx = 0 - dx
    if y < 28 or y > H - 24:
        dy = 0 - dy

    gui.clear(0x1E1E28 if gui.dark() == 1 else 0xF2F2F7)
    gui.text(10, 8, 0x888888, "AetherScript GUI -- q to quit")
    gui.rect(x, y, 16, 16, 0x3478F6)
    gui.flush()
    frames = frames + 1
    if frames == 30:
        print("gui ok")        # serial marker for the boot test
    gui.yield_()
