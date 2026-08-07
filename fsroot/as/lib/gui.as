# gui -- windowed Logit apps in pure AetherScript (M23.5).
#   import gui
#   gui.create("My App", 400, 300)
#   gui.rect(10, 10, 100, 50, 0x3478F6)
#   gui.text(20, 30, 0xFFFFFF, "hello")
#   gui.flush()
#   ev = gui.poll()          # nil, or an Event
# An Event carries .type .a .b .mods .button .wheel:
#   .type    EV_KEY / EV_MOUSE (a button went down) / EV_MOUSE_R (right down) /
#            EV_MOUSE_UP / EV_MOUSE_MOVE / EV_WHEEL / EV_CLOSE / EV_THEME
#   .a .b    EV_KEY: the character or KEY_* code. Pointer events: window-local x,y
#   .mods    EV_MOD_SHIFT | EV_MOD_CTRL | EV_MOD_ALT held when it happened
#   .button  EV_BTN_LEFT / _RIGHT / _MIDDLE on a press or release, else EV_BTN_NONE
#   .wheel   EV_WHEEL only: notches, positive = scrolled down
# EV_MOUSE_MOVE is COALESCED by the kernel -- consecutive samples collapse to the
# newest one, so a script that repaints slowly gets the pointer's current
# position rather than a backlog. Clicks and wheel notches are never merged.
# The kernel adopts the script's process as a window owner on create(); closing
# the window delivers EV_CLOSE and the script exits like any app.
#
# There is no bit-twiddling left in this file. `(x << 16) | y` is a calling
# convention, and a convention written out by hand at each caller is one nothing
# checks -- it lived here, in a logit_abi.h comment, and in the kernel's
# unpacking: three copies of one rule. It is stated once now, in
# include/abi/logit_calls.abi, which generates both the packing these wrappers
# call (`abi`) and the kernel's unpack macros (include/abi/logit_pack.h).

from abi import Event, gui_create, gui_clear, gui_rect, gui_rrect, gui_text, gui_text_mono
from abi import gui_icon, gui_glass, gui_clip, gui_flush, gui_poll_event, ui_dark_query, sys_yield

# One event struct, reused: the kernel fills it on each poll and the caller is
# expected to handle the event before polling again. Its field offsets come from
# include/abi/logit_abi.h, so a kernel that changes struct logit_event breaks the
# build rather than this reading the wrong words.
_ev = Event()

def create(title, w, h):
    return gui_create(title, w, h)

def clear(color):
    return gui_clear(color)

def rect(x, y, w, h, color):
    return gui_rect(x, y, w, h, color)

# A rounded rect, like a web border-radius. radius is 8 bits (the kernel reads
# it as such), which the header comment never said out loud.
def rrect(x, y, w, h, radius, color):
    return gui_rrect(x, y, w, h, radius, color)

# Restrict drawing to a rectangle of this window's surface; (0,0,0,0) clears it.
def clip(x, y, w, h):
    return gui_clip(x, y, w, h)

def text(x, y, color, s):
    return gui_text(x, y, color, s)

def text_mono(x, y, cell, color, s):
    return gui_text_mono(x, y, cell, color, s)

def icon(x, y, id, px, color):
    return gui_icon(x, y, id, px, color)

# radius and an RGBA tint, each its own argument now. It used to take one
# pre-packed 64-bit `spec`, which meant the packing rule leaked out of this
# module and into every caller.
def glass(x, y, w, h, radius, tr, tg, tb, ta):
    return gui_glass(x, y, w, h, radius, tr, tg, tb, ta)

def flush():
    return gui_flush()

def dark():
    return ui_dark_query()

def poll():
    if gui_poll_event(_ev) != 1:
        return nil
    return _ev

def yield_():
    return sys_yield()
