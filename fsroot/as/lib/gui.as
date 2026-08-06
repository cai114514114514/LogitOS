# gui -- windowed Logit apps in pure AetherScript (M23.5).
#   import gui
#   gui.create("My App", 400, 300)
#   gui.rect(10, 10, 100, 50, 0x3478F6)
#   gui.text(20, 30, 0xFFFFFF, "hello")
#   gui.flush()
#   ev = gui.poll()          # nil, or an Event with .type .a .b (EV_KEY/EV_MOUSE/EV_CLOSE)
# The kernel adopts the script's process as a window owner on create(); closing
# the window delivers EV_CLOSE and the script exits like any app.

from abi import Event

# One event struct, reused: the kernel fills it on each poll and the caller is
# expected to handle the event before polling again. Its field offsets come from
# include/abi/logit_abi.h, so a kernel that changes struct logit_event breaks the
# build rather than this reading the wrong words.
_ev = Event()

def create(title, w, h):
    return syscall(SYS_GUI_CREATE, addr(title), (w << 16) | h)

def clear(color):
    return syscall(SYS_GUI_CLEAR, color)

def rect(x, y, w, h, color):
    return syscall(SYS_GUI_RECT, (x << 16) | y, (w << 16) | h, color)

def text(x, y, color, s):
    return syscall(SYS_GUI_TEXT, (x << 16) | y, color, addr(s))

def text_mono(x, y, cell, color, s):
    return syscall(SYS_GUI_TEXT_MONO, (x << 16) | y, (cell << 24) | color, addr(s))

def icon(x, y, id, px, color):
    return syscall(SYS_GUI_ICON, (x << 16) | y, (id << 16) | px, color)

def glass(x, y, w, h, spec):
    return syscall(SYS_GUI_GLASS, (x << 16) | y, (w << 16) | h, spec)

def flush():
    return syscall(SYS_GUI_FLUSH)

def dark():
    return syscall(SYS_UI_DARK, -1)

def poll():
    if syscall(SYS_POLL_EVENT, addr(_ev)) != 1:
        return nil
    return _ev

def yield_():
    return syscall(SYS_YIELD)
