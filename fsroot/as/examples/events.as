# events.as -- an app that says out loud what the window manager sent it.
#
# The event ABI grew mouse-up, mouse-move, wheel, a button id and modifier
# flags. Every one of those is a claim about a path that runs from a PS/2 packet
# through an IRQ, a deferred queue, window routing and a coalescing ring before
# an app ever sees it, and none of it is visible from inside the kernel. So this
# opens a window and prints each event as a line on the serial console:
#
#   EV <type> <a> <b> <mods> <button> <wheel>
#
# tests/qmp/qmp_input.py drives real input at it over QEMU's input layer and
# checks the lines. Quits on 'q' or the window's close button.
#
# The window is deliberately large (900x600): it lands under the cursor's boot
# position (screen centre) at the third cascade slot, so the harness does not
# have to walk a relative pointer to a target it cannot see.

import gui

gui.create("Events", 900, 600)
gui.clear(0x1E1E28)
gui.text(16, 16, 0x9AA0B0, "events.as -- every event goes to the serial console")
gui.flush()
print("EVENTS-READY")

n = 0
running = true
while running:
    ev = gui.poll()
    while ev != nil:
        if ev.type == EV_CLOSE:
            running = false
        elif ev.type == EV_KEY and (ev.a == 113 or ev.a == 27):
            running = false
        else:
            print("EV", ev.type, ev.a, ev.b, ev.mods, ev.button, ev.wheel)
            n = n + 1
        ev = gui.poll()
    gui.yield_()

print("EVENTS-DONE", n)
