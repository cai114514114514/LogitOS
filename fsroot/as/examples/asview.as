# aether: 3.0
# asview -- an image viewer for LogitOS, written in AetherScript.
#
#   /usr/as/bin/asview.aex /media/dot.png
#
#   q / Esc     quit                 f   fit to window
#   space       toggle fit/actual    a   actual size (1:1)
#   n / p       next / previous image in the same directory
#
# WHAT THIS IS FOR. c/apps/gui/preview.c is 1,381 lines of C and its call
# profile is almost pure glue -- gui_text x10, gui_rect x9, gui_flush x9,
# gui_clear x8, gui_blit x2, img_decode x3 -- because every heavy thing it does
# (decoding, rasterizing, compositing) already lives behind a syscall or in a
# library. If that is true, an application of that shape does not need to be
# written in C, and this file is the measurement of that claim rather than an
# argument for it. It opens a file named on the command line, decodes it,
# scales it to the window, answers the keyboard and the close button, and when
# it cannot decode something it says on screen WHICH FILE and WHY.
#
# WHAT IT DOES NOT PROVE. Preview also plays H.264 video and animates GIFs
# frame by frame, and neither is reachable from here: the H.264 decoder is a
# ring-3 C library linked into a binary, AetherScript has no FFI, and calling
# into one is M30 native-codegen work. The boundary is exact and worth stating
# in one line -- ANYTHING BEHIND A SYSCALL IS REACHABLE FROM THIS LANGUAGE
# TODAY, ANYTHING BEHIND A `.o` IS NOT.
#
# CAPABILITIES (M28). This program needs CAP_FS (to stat, read and decode the
# file -- c/kernel/exec/syscall.c classes SYS_IMG_DECODE as CAP_FS), CAP_GUI (to
# own a window) and CAP_RAW (because every lib/abi.as wrapper marshals its
# arguments through addr(), which is CAP_RAW-gated in legacy/builtins.c). Launched
# from the Dock a GUI app holds CAP_ALL; launched from a shell it holds whatever
# the shell granted, and `as --scope /media asview.as /etc/logit.conf` is
# refused BY NAME on the window rather than showing an empty one -- see
# lib/image.as refusal(). A denial is a catchable exception, not a fault, which
# is the whole reason the error path below can draw anything at all.
# A3 correction: this viewer is now an independently linked native program.
# Its decode path still uses the kernel image service, not a video/animation
# decoder. The old VM scope-launch example above is replaced by an actual
# restricted native child in the guest gate; library imports happen at build
# time and never cause the retiring compiler to run inside the guest.

# A3 uses explicit std imports: the sibling teaching examples sys.as and
# strings.as are applications, not the standard-library modules with those
# names. Resolving them implicitly would import the wrong program.
import std.gui as gui
import std.image as image
from std.image import Image
import std.paths as paths
import std.strings as strings
from std.sys import ls

WIN_W = 560
WIN_H = 400
HEADER = 30                 # the info line above the picture
FOOTER = 22                 # the key legend below it
PAD = 8
UI_PX = gui.UI_PX           # the size gui.text() actually draws at

BOX_X = PAD
BOX_Y = HEADER
BOX_W = WIN_W - 2 * PAD
BOX_H = WIN_H - HEADER - FOOTER

# The formats SYS_IMG_DECODE can handle, as file extensions, for choosing what
# n/p will step to. This is a SEPARATE list from lib/image.as's magic-number
# sniff on purpose and the two answer different questions: this one asks "is
# this worth trying" over a directory listing, where opening every file to look
# at its bytes would be absurd; that one asks "what are these bytes", which is
# the only answer that can be trusted. A file whose name lies is simply
# refused by name when it is opened.
EXTS = [".png", ".gif", ".jpg", ".jpeg", ".bmp", ".ico", ".webp", ".apng"]


def lower(s: str) -> str:
    out = ""
    for i in range(len(s)):
        c = ord(s[i])
        out = out + (chr(c + 32) if c >= 65 and c <= 90 else s[i])
    return out


def is_image_name(name: str) -> bool:
    e = lower(paths.extname(name))
    for x in EXTS:
        if e == x:
            return true
    return false


# Every image beside `path`, in the order the filesystem lists them, with the
# starting file's index. NOT SORTED, and that is deliberate: SYS_DIR_NAME
# returns directory order, sorting it here would be this program inventing an
# order the rest of the machine does not use, and Finder shows the same order.
class Siblings:
    files: List[str]
    index: i64

    def init(self, files: List[str], index: i64) -> None:
        self.files = files
        self.index = index


def siblings(path: str) -> Siblings:
    d = paths.dirname(path)
    base = paths.basename(path)
    try:
        names = ls(d)
    except Error:
        # Directory enumeration is optional navigation. Keep the requested
        # path so load() can show a precise file/capability error in a window.
        return Siblings([path], 0)
    out: List[str] = []
    at = -1
    if names is not None:
        for n in names:
            if is_image_name(n):
                if n == base:
                    at = len(out)
                out.append(paths.join(d, n))
    else:
        return Siblings([path], 0)
    if at < 0:
        # The file we were given is not in its own directory listing (it may
        # have an extension EXTS does not know, which is fine -- the decoder is
        # the authority, not the name). Keep it as the current item so n/p
        # still has somewhere to start from.
        out.append(path)
        at = len(out) - 1
    return Siblings(out, at)


# Break `s` into lines no wider than maxw POINTS, measured with the same font
# and size gui.text() will draw them at. A single word longer than the box is
# NOT broken -- it overhangs -- because breaking mid-word in an error message
# containing a path is worse than a line that runs long, and the caller can see
# both.
def wrap(s: str, maxw: i64) -> List[str]:
    out: List[str] = []
    line = ""
    for w in strings.split(s, " "):
        cand = w if line == "" else line + " " + w
        if line != "" and gui.measure(cand, UI_PX, 0) > maxw:
            out.append(line)
            line = w
        else:
            line = cand
    if line != "":
        out.append(line)
    return out


class View:
    path: str
    img: Optional[Image]
    err: str
    fit: bool

    def init(self, path: str) -> None:
        self.path = path
        self.img = None
        self.err = ""
        self.fit = true
        self.load(path)

    def load(self, path: str) -> None:
        self.path = path
        self.img = None
        self.err = ""
        print(f"asview: open {path}")
        try:
            picture = image.decode(path)
            self.img = picture
            print(f"asview: image {picture.format} {picture.w}x{picture.h}")
        except Error as error:
            # The whole reason decode() raises a SENTENCE rather than returning
            # nil: this is the text the user is about to read, and it was
            # written by the code that knows why.
            self.err = error.message
            print(f"asview: error {self.err}")

    # The destination rect for the picture, in window points.
    def rect(self) -> List[i64]:
        picture = self.img
        if picture is None:
            raise ValueError("Cannot compute a rectangle without an image")
        if self.fit:
            return image.fit(picture.w, picture.h, BOX_X, BOX_Y, BOX_W, BOX_H)
        return image.centre(picture.w, picture.h, BOX_X, BOX_Y, BOX_W, BOX_H)

    def draw(self, dark: bool) -> None:
        bg = 0x1E1E28 if dark else 0xF2F2F7
        canvas = 0x2A2A34 if dark else 0xE5E5EA
        ink = 0xE8E8F0 if dark else 0x1C1C1E
        dim = 0x9A9AA6 if dark else 0x6E6E73
        bad = 0xFF6B6B if dark else 0xC0392B

        gui.clear(bg)
        gui.rect(BOX_X, BOX_Y, BOX_W, BOX_H, canvas)

        # Narrow a local owner: a mutable Optional field is not proof that a
        # later field read remains non-null. The image stays rooted throughout
        # formatting and drawing, even when those operations allocate.
        picture = self.img
        if picture is None:
            gui.text(PAD, 8, bad, "Cannot show this image")
            y = BOX_Y + 14
            gui.text(BOX_X + 12, y, ink, paths.basename(self.path))
            y = y + 22
            for line in wrap(self.err, BOX_W - 24):
                gui.text(BOX_X + 12, y, dim, line)
                y = y + 20
                if y > BOX_Y + BOX_H - 20:
                    return
            return

        r = self.rect()
        # CLIP FIRST. At actual size an image larger than the box would
        # otherwise be blitted over the header and the legend -- the kernel
        # clips to the WINDOW, which is not the same rectangle.
        gui.clip(BOX_X, BOX_Y, BOX_W, BOX_H)
        gui.blit_image(picture, r[0], r[1], r[2], r[3])
        gui.clip(0, 0, 0, 0)

        mode = "fit" if self.fit else "1:1"
        gui.text(PAD, 8, ink, paths.basename(self.path))
        info = f"{picture.format} {picture.w}x{picture.h} [{mode}]"
        gui.text(WIN_W - PAD - gui.measure(info, UI_PX, 0), 8, dim, info)
        print(f"asview: mode {mode} rect={r[0]},{r[1]},{r[2]},{r[3]}")
        return


def show_usage() -> None:
    # Still opens a window: a viewer launched with no argument that exits
    # silently is indistinguishable from one that crashed.
    gui.create("asview", WIN_W, 120)
    gui.clear(0xF2F2F7)
    gui.text(PAD, 20, 0xC0392B, "asview: no file given")
    gui.text(PAD, 48, 0x6E6E73, "usage: asview.aex <image-file>")
    gui.flush()
    print("asview: error no file given")
    print("asview: ready")
    # Keep the diagnostic visible until the user dismisses it. Returning
    # immediately destroys this process's window before a person can read it.
    running = true
    while running:
        event = gui.poll()
        if event is not None:
            if i64(event.type) == EV_CLOSE:
                running = false
            elif i64(event.type) == EV_KEY and (event.a == 113 or event.a == 27):
                running = false
        gui.yield_()
    print("asview: bye")


def main() -> None:
    a = args()
    if len(a) < 2:
        show_usage()
        return

    start = a[1]
    sib = siblings(start)
    files = sib.files
    idx = sib.index

    gui.create("asview", WIN_W, WIN_H)
    gui.win_min(320, 240)
    v = View(start)
    dark = gui.dark() == 1
    v.draw(dark)
    gui.flush()
    print("asview: ready")

    running = true
    while running:
        ev = gui.poll()
        dirty = false
        while ev is not None:
            if i64(ev.type) == EV_CLOSE:
                running = false
            elif i64(ev.type) == EV_THEME:
                dark = gui.dark() == 1
                dirty = true
            elif i64(ev.type) == EV_KEY:
                k = ev.a
                if k == 113 or k == 27:                 # q, Esc
                    running = false
                elif k == 102:                          # f -- fit
                    if not v.fit:
                        v.fit = true
                        dirty = true
                elif k == 97:                           # a -- actual size
                    if v.fit:
                        v.fit = false
                        dirty = true
                elif k == 32:                           # space -- toggle
                    v.fit = not v.fit
                    dirty = true
                elif k == 110 and len(files) > 1:       # n -- next
                    idx = (idx + 1) % len(files)
                    v.load(files[idx])
                    dirty = true
                elif k == 112 and len(files) > 1:       # p -- previous
                    idx = (idx + len(files) - 1) % len(files)
                    v.load(files[idx])
                    dirty = true
            ev = gui.poll()

        if dirty and running:
            v.draw(dark)
            gui.flush()
        gui.yield_()

    print("asview: bye")
