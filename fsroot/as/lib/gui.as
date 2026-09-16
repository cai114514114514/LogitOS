# aether: 3.0
# gui -- windowed Logit apps in pure AetherScript (M23.5).
#   import gui
#   gui.create("My App", 400, 300)
#   gui.rect(10, 10, 100, 50, 0x3478F6)
#   gui.text(20, 30, 0xFFFFFF, "hello")
#   img = gui.picture("/media/dot.png")      # decode (lib/image.as does the work)
#   gui.blit_image(img, 20, 50, img.w, img.h)
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

from abi import Event, Blit, gui_create, gui_clear, gui_rect, gui_rrect, gui_text, gui_text_mono
from abi import gui_icon, gui_glass, gui_clip, gui_flush, gui_poll_event, ui_dark_query, sys_yield
from abi import Run, gui_blit, gui_text_run, gui_win_min, text_measure
import image
from image import Image

# One event struct, reused: the kernel fills it on each poll and the caller is
# expected to handle the event before polling again. Its field offsets come from
# include/abi/logit_abi.h, so a kernel that changes struct logit_event breaks the
# build rather than this reading the wrong words.
_ev: Event = Event()

def create(title: str, w: i64, h: i64) -> i64:
    return gui_create(title, w, h)

def clear(color: i64) -> i64:
    return gui_clear(color)

def rect(x: i64, y: i64, w: i64, h: i64, color: i64) -> i64:
    return gui_rect(x, y, w, h, color)

# A rounded rect, like a web border-radius. radius is 8 bits (the kernel reads
# it as such), which the header comment never said out loud.
def rrect(x: i64, y: i64, w: i64, h: i64, radius: i64, color: i64) -> i64:
    return gui_rrect(x, y, w, h, radius, color)

# Restrict drawing to a rectangle of this window's surface; (0,0,0,0) clears it.
def clip(x: i64, y: i64, w: i64, h: i64) -> i64:
    return gui_clip(x, y, w, h)

def text(x: i64, y: i64, color: i64, s: str) -> i64:
    return gui_text(x, y, color, s)

def text_mono(x: i64, y: i64, cell: i64, color: i64, s: str) -> i64:
    return gui_text_mono(x, y, cell, color, s)

def icon(x: i64, y: i64, id: i64, px: i64, color: i64) -> i64:
    return gui_icon(x, y, id, px, color)

# radius and an RGBA tint, each its own argument now. It used to take one
# pre-packed 64-bit `spec`, which meant the packing rule leaked out of this
# module and into every caller.
def glass(x: i64, y: i64, w: i64, h: i64, radius: i64, tr: i64, tg: i64, tb: i64, ta: i64) -> i64:
    return gui_glass(x, y, w, h, radius, tr, tg, tb, ta)

# ---- pixels ------------------------------------------------------------------
# Everything above this line draws a SHAPE the compositor knows about. blit()
# is the one call that puts arbitrary pixels on a window, and it is how an
# image, a decoded video frame or a rasterizer's output reaches the screen.
#
# THE DESTINATION RECT SCALES AND THE SOURCE DOES NOT. (w,h) is where the
# pixels land; (sw,sh) is how many there are. The kernel's fb_blit_rgba()
# rescales nearest-neighbour between them, so "fit this picture to the window"
# costs four integers and no pixel loop at all -- see image.fit(). Passing
# w,h == sw,sh is the 1:1 case and is not special-cased anywhere.
#
# `rgba` is a buffer() of sw*sh*4 bytes, RGBA8888. Its ADDRESS is what the
# kernel reads, so the buffer must stay reachable from the caller across this
# call; it does, because the caller is holding it in the variable it passed.
#
# One Blit struct, reused, for the same reason _ev is: the kernel copies it out
# of user memory before returning (wm.c memcpy's the whole struct), so nothing
# survives the call that a second struct would have protected.
_bl: Blit = Blit()

def blit[B: ByteStorage](x: i64, y: i64, w: i64, h: i64, rgba: B, sw: i64, sh: i64) -> i64:
    # A2 trusted dimensions and let the kernel dereference a raw address. A3
    # validates the complete source extent before crossing that boundary.
    # Divide first: sw * sh * 4 could itself overflow on an invalid request.
    if sw <= 0 or sh <= 0 or sw > len(rgba) / 4 / sh:
        raise ValueError("gui.blit: source dimensions exceed RGBA storage")
    _bl.x = i32(x)
    _bl.y = i32(y)
    _bl.w = i32(w)
    _bl.h = i32(h)
    _bl.sw = i32(sw)
    _bl.sh = i32(sh)
    unsafe:
        _bl.rgba = addr(rgba)
    # The source parameter owns the bytes until the synchronous call returns.
    # A pointer field in a layout record is never a native GC root.
    return gui_blit(_bl)

# The call an application actually wants: put THIS decoded image in THIS rect.
# It exists so that the two numbers a caller must not get wrong -- the source
# width and height, which have to be the ones the decode reported and not the
# ones on screen -- come from the image itself rather than from two more
# variables at the call site.
def blit_image(img: Image, x: i64, y: i64, w: i64, h: i64) -> i64:
    return blit(x, y, w, h, img.rgba, img.w, img.h)

# decode a file to an Image. A one-line re-export of image.decode() and
# deliberately not a second implementation: an app that has already imported
# gui to get a window should not have to learn a second module name to put a
# picture in it, and a copy of the decode here would be the "fourth path"
# problem in miniature. lib/image.as is where the work and the reasoning are.
def picture(path: str) -> Image:
    return image.decode(path)

# ---- measuring ---------------------------------------------------------------
# The width in POINTS that text() would use for this string at this size. The
# only way a script can lay anything out around text without hardcoding a
# per-character width, which is wrong for every proportional font and wrong by
# a factor of two for CJK. `mono` picks the monospace face, matching
# text_mono(); the answer is a width only -- the kernel returns no height, so a
# caller wanting a line height still has to use its own leading.
def measure(s: str, px: i64, mono: i64) -> i64:
    return text_measure(s, len(s), px, mono)

# The size text() ACTUALLY DRAWS AT, in points. text() takes no size argument --
# SYS_GUI_TEXT hardcodes fb_ui_px(), which is TEXT_UI_PX (c/kernel/gui/text.h)
# times the display's backing scale, and the kernel answers measure() in points
# too, so at any scale the number to measure with is this one. Without it
# `measure(s, 16, 0)` is a 16 someone copied out of a header, and it is wrong
# the moment the two stop agreeing.
UI_PX: i64 = 16

# Text at a size of the caller's choosing, which text() cannot do: SYS_GUI_TEXT
# has no size argument, so an app wanting a heading has to go through the run
# struct. Same reused-struct rule as _ev and _bl -- the kernel copies it out
# before returning.
_run: Run = Run()

def text_px(x: i64, y: i64, px: i64, mono: i64, color: i64, s: str) -> i64:
    _run.x = i32(x)
    _run.y = i32(y)
    _run.px = i32(px)
    _run.mono = i32(mono)
    _run.color = u32(color)
    _run.len = i32(len(s))
    unsafe:
        _run.s = addr(s)
    # This ABI is length-delimited, so interior UTF-8 views need no NUL copy.
    # Keep s as a parameter owner across the call; _run.s cannot retain it.
    return gui_text_run(_run)

# The smallest CONTENT size the window manager will let the user drag this
# window to, in points. Worth setting from any app whose layout has a floor:
# without it a window can be resized to a few points across and every
# subsequent rect is clipped to nothing, which looks like the app crashed.
def win_min(w: i64, h: i64) -> i64:
    return gui_win_min(w, h)

def flush() -> i64:
    return gui_flush()

def dark() -> i64:
    return ui_dark_query()

def poll() -> Optional[Event]:
    if gui_poll_event(_ev) != 1:
        return None
    return _ev

def yield_() -> i64:
    return sys_yield()
