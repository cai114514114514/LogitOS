# image -- decode a picture file into pixels an app can put on screen.
#
#   import image
#   img = image.decode("/media/dot.png")     # raises a SENTENCE on failure
#   print(img.w, img.h, img.format)
#   gui.blit_image(img, x, y, w, h)          # lib/gui.as does the drawing
#
# WHY THIS IS A LIBRARY AND NOT FOUR LINES IN EVERY APP. The decode itself IS
# four lines -- fill a logit_imgreq, call SYS_IMG_DECODE, read w/h back. What is
# not four lines is everything around it, and each of the following was found by
# writing the first AetherScript image viewer (/usr/as/examples/asview.as)
# rather than reasoned about in advance:
#
#   * THE KERNEL ANSWERS -1 AND NOTHING ELSE. c/kernel/exec/syscall.c's
#     SYS_IMG_DECODE case has eight distinct `r->rax = -1` returns -- no such
#     file, unreadable, zero length, no decoder for these bytes, a decoder that
#     failed, and w*h*4 larger than the buffer the caller brought -- and from
#     ring 3 they are one value. An app that prints "could not open the file"
#     has told the user nothing; an app that prints nothing shows an empty
#     window, which is worse. So the reason is RECONSTRUCTED here out of facts
#     the caller can still establish: stat, and the file's own first bytes.
#
#   * THE CALLER MUST GUESS THE BUFFER SIZE BEFORE IT KNOWS THE IMAGE SIZE.
#     `max` is an input and `w`,`h` are outputs of the same call, so there is
#     no way to ask first. decode() therefore retries on a doubling budget, and
#     when a bigger buffer succeeds it says nothing -- the app never learns the
#     first attempt happened. That retry is also the only reason "too large"
#     and "the decoder refused it" can be told apart at all.
#
#   * A DENIAL MUST NAME THE CAPABILITY. Under M28 this call needs CAP_FS
#     (syscall.c classes SYS_IMG_DECODE as CAP_FS) and, because every wrapper
#     in lib/abi.as marshals through addr(), CAP_RAW as well. Reached without
#     them the failure is either a kernel -1 indistinguishable from a missing
#     file, or an addr() refusal raised from a stack frame the app never wrote.
#     Both are checked HERE, before the syscall, so the sentence the app shows
#     names the capability that was missing.
#
# WHAT THIS FILE DOES NOT DO: it does not rescale. SYS_GUI_BLIT already
# rescales nearest-neighbour into whatever destination rect it is given
# (fb_blit_rgba in c/kernel/gui/fb.c), so scaling an image to fit is arithmetic
# on four integers, not a pixel loop -- fit() below is that arithmetic. A
# resampler written in AetherScript would be a second, slower, worse copy of
# something the compositor already does on the way to the screen.
#
# COST, stated because it is not free: a successful decode() reads the file
# TWICE -- once here to sniff its magic number, once inside the kernel to
# decode it. The sniff is what lets a non-image be refused by name before a
# multi-megabyte buffer is allocated, and LogitFS's read is all-or-nothing (see
# SNIFF_MAX), so there is no "read the first 16 bytes" to spend instead.

from abi import Imgreq, Stat, img_decode, stat, file_read

# The first buffer decode() tries, and the largest it will grow to. 5 MiB holds
# the desktop's own 1280x800 wallpaper (4,096,000 bytes of RGBA) on the first
# attempt, which is the largest image that ships on the disk; 20 MiB is the
# ceiling, and it is set by the ROOM AVAILABLE rather than by taste -- /bin/as
# runs on mini-libc, whose arena is 24 MiB (ARENA_SIZE, c/apps/libc/src/
# malloc.c), and the VM's own heap lives in there too. An image needing more
# than 20 MiB of RGBA (about 2236x2236) is reported as too large, by name,
# instead of taking the process down.
BUDGET0 = 5 * 1024 * 1024
BUDGET_MAX = 20 * 1024 * 1024

# How much of a file this module will read back to look at its magic number.
# LogitFS's read is ALL-OR-NOTHING -- c/fs/logitfs.c inode_read() returns -1
# when the file is longer than the buffer, there is no partial read and no
# offset -- so "look at the first 16 bytes" costs a buffer as large as the
# whole file. Fine for a picture, wrong for a video, so it is bounded: past
# this size the format is reported as unknown-because-unsniffable, and the
# messages say which of the two it is.
SNIFF_MAX = 4 * 1024 * 1024

UNSNIFFED = "?"

# The formats c/lib/image registers INSIDE THE KERNEL, which is not the same
# list the browser has. img_init() (c/lib/image/img.c) registers png/gif/jpeg/
# bmp/ico/webp unconditionally and svg only through a weak hook the kernel
# build leaves empty -- so an .svg reaching SYS_IMG_DECODE is refused exactly
# like an unknown format, and "SVG, which this kernel cannot decode" is a more
# useful sentence than "not an image".
def magic(b, n):
    if n >= 8 and b[0] == 0x89 and b[1] == 0x50 and b[2] == 0x4E and b[3] == 0x47:
        return "PNG"
    if n >= 6 and b[0] == 0x47 and b[1] == 0x49 and b[2] == 0x46:
        return "GIF"
    if n >= 3 and b[0] == 0xFF and b[1] == 0xD8 and b[2] == 0xFF:
        return "JPEG"
    if n >= 2 and b[0] == 0x42 and b[1] == 0x4D:
        return "BMP"
    if n >= 4 and b[0] == 0x00 and b[1] == 0x00 and b[2] == 0x01 and b[3] == 0x00:
        return "ICO"
    if n >= 12 and b[0] == 0x52 and b[1] == 0x49 and b[2] == 0x46 and b[3] == 0x46:
        if b[8] == 0x57 and b[9] == 0x45 and b[10] == 0x42 and b[11] == 0x50:
            return "WebP"
    # Not a magic number: SVG is XML and may begin with a comment, a BOM or an
    # <?xml?>. A '<' in byte 0 is as far as an honest sniff goes, and it still
    # says something better than "unknown".
    if n >= 1 and b[0] == 0x3C:
        return "SVG/XML"
    return ""


def hexbytes(b, n, k):
    d = "0123456789ABCDEF"
    out = ""
    i = 0
    if k > n:
        k = n
    while i < k:
        out = out + d[(b[i] >> 4) & 15] + d[b[i] & 15] + " "
        i = i + 1
    return out


class Image:
    # path and format ride along so an app can label what it is showing without
    # keeping a second set of variables in step with the pixels.
    def init(self, path, format, w, h, rgba):
        self.path = path
        self.format = format
        self.w = w
        self.h = h
        self.rgba = rgba              # a buffer(), w*h*4 bytes, RGBA8888

    def pixels(self):
        return self.w * self.h

    # One pixel as 0xRRGGBBAA. Present because a test that wants to know the
    # decode is RIGHT, not merely non-empty, needs a way to name a pixel, and
    # reaching in with an index at the call site would put the stride
    # arithmetic in every caller.
    def at(self, x, y):
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            raise f"image.at({x},{y}): outside a {self.w}x{self.h} image"
        i = (y * self.w + x) * 4
        r = self.rgba[i]
        g = self.rgba[i + 1]
        bl = self.rgba[i + 2]
        a = self.rgba[i + 3]
        return (r << 24) | (g << 16) | (bl << 8) | a


# stat_of(path) -> a Stat layout, or nil. The third argument is the caller's
# OWN struct size, which is the point of the len/version pair at the front of
# struct logit_stat -- see the note beside `call stat` in
# include/abi/logit_calls.abi.
def stat_of(path):
    st = Stat()
    if stat(path, st, len(st)) != 0:
        return nil
    return st


def is_dir(st):
    return (st.mode & LST_IFMT) == LST_IFDIR


# The capability check, done BEFORE the syscall so a refusal can name what is
# missing. Returns "" when the call may proceed, or the sentence to show.
#
# It is a check and not a try/except because both failures it prevents are
# illegible: without CAP_FS the kernel refuses SYS_IMG_DECODE with the same -1
# it uses for a missing file, and without CAP_RAW the addr() inside the
# generated wrapper raises a message about addr() from a frame the application
# never wrote.
def refusal(path):
    c = caps()
    b = c.bits()
    if (b & CAP_FS_READ) == 0:
        return (f"{path}: refused -- this process does not hold CAP_FS_READ, "
                + "and reading an image file needs it (M28)")
    if (b & CAP_RAW) == 0:
        return (f"{path}: refused -- this process does not hold CAP_RAW, which "
                + "every lib/abi.as wrapper needs to hand a struct pointer to the kernel")
    # The PATH half of the grant. c.scope(p) is the only probe the language
    # exposes for "is p inside what I hold" -- it calls as_caps_permit_path()
    # and raises rather than answering false. Note what this does and does NOT
    # prove: the prefix is enforced by the LANGUAGE (as_port.c's open(), and
    # this line), not by the kernel, so a script reaching SYS_IMG_DECODE
    # through a bare syscall() bypasses it entirely. That is a real hole; it is
    # written down in this unit's gap list rather than hidden here.
    try:
        c.scope(path)
    except e:
        return (f"{path}: refused -- outside this process's capability scope "
                + f"({c.path()})")
    return ""


# probe(path) -> [format, size], having established that the file exists, is a
# file, and is not empty. RAISES the sentence for each of those failures, so
# the caller does not have to re-derive them.
def probe(path):
    st = stat_of(path)
    if st == nil:
        raise f"{path}: no such file (stat refused it)"
    if is_dir(st):
        raise f"{path}: is a directory, not an image"
    if st.size == 0:
        raise f"{path}: the file is empty (0 bytes)"
    if st.size > SNIFF_MAX:
        return [UNSNIFFED, st.size]
    b = buffer(st.size)
    n = file_read(path, b, st.size)
    if n < 0:
        raise f"{path}: {st.size} bytes on disk, but the read was refused"
    fmt = magic(b, n)
    if fmt == "":
        raise (f"{path}: not an image -- the first bytes are "
               + hexbytes(b, n, 4)
               + "and match no format this kernel decodes (PNG GIF JPEG BMP ICO WebP)")
    if fmt == "SVG/XML":
        raise (f"{path}: looks like SVG/XML, which the KERNEL decoder does not "
               + "handle -- c/lib/image/svg.c is not linked into ring 0")
    return [fmt, st.size]


# decode(path) -> an Image, or RAISES a sentence naming the path and the reason.
#
# It raises rather than returning nil because nil is the shape that produces
# the empty window this library exists to prevent: a caller that forgets one
# `if` draws nothing and says nothing, whereas an uncaught raise at least puts
# the reason on the console. An app that wants to keep going catches it -- and
# asview.as draws the caught sentence on the window.
def decode(path):
    r = refusal(path)
    if r != "":
        raise r
    pr = probe(path)
    fmt = pr[0]
    size = pr[1]

    req = Imgreq()
    budget = BUDGET0
    while budget <= BUDGET_MAX:
        b = buffer(budget)
        req.path = addr(path)
        req.rgba = addr(b)
        req.max = budget
        req.w = 0
        req.h = 0
        if img_decode(req) == 0:
            # The kernel wrote w/h back into the same struct. Trust them only
            # as far as they agree with the buffer it filled: a w*h*4 past the
            # budget would let at() index outside the buffer and raise
            # somewhere unrelated to the cause.
            if req.w <= 0 or req.h <= 0 or req.w * req.h * 4 > budget:
                raise (f"{path}: the kernel reported {req.w}x{req.h}, which does "
                       + f"not fit the {budget}-byte buffer it was given")
            return Image(path, fmt, req.w, req.h, b)
        # Drop the failed buffer before asking for a bigger one. On a 24 MiB
        # arena, holding the 5 MiB attempt while allocating the 10 MiB one is
        # the difference between growing and failing to.
        b = nil
        gc()
        budget = budget * 2

    if fmt == UNSNIFFED:
        raise (f"{path}: {size} bytes -- too large to read back for a format check "
               + "(LogitFS has no partial read), and SYS_IMG_DECODE refused it")
    # Known magic, and even BUDGET_MAX was not enough: the two remaining
    # readings are "more pixels than that buffer holds" and "the decoder
    # rejected the data". Both are named; neither is asserted over the other,
    # because the syscall returns no reason code and inventing one would be
    # worse than saying so.
    raise (f"{path}: {fmt}, {size} bytes -- it decodes to more than "
           + f"{BUDGET_MAX} bytes of pixels, or the {fmt} decoder rejected it "
           + "(SYS_IMG_DECODE answers -1 with no reason code)")


# fit(iw, ih, bx, by, bw, bh) -> [x, y, w, h]: the largest rect with the
# image's aspect ratio that fits in the box, centred in it.
#
# Integer-only and comparison-first: `iw * bh <= bw * ih` is `iw/ih <= bw/bh`
# with no division, so nothing is rounded before the decision is made. The one
# division left produces a pixel count and truncates -- so the result is never
# one pixel WIDER than the box, which is the direction that would clip.
def fit(iw, ih, bx, by, bw, bh):
    if iw <= 0 or ih <= 0:
        raise f"image.fit: a {iw}x{ih} image has no aspect ratio"
    if bw <= 0 or bh <= 0:
        return [bx, by, 0, 0]
    # w and h are introduced HERE and not in the branches. AetherScript blocks
    # are real scopes and a first assignment inside one declares a BLOCK-local:
    # writing `w = ...` in each arm of the if/else compiles, runs, and then
    # fails at the line below with `undefined variable 'w'`. Assignment to a
    # name that already exists in an enclosing scope is an ordinary assignment,
    # which is what makes this shape work and the other one not.
    w = bw
    h = bh
    if iw * bh <= bw * ih:
        h = bh
        w = iw * bh / ih
    else:
        w = bw
        h = ih * bw / iw
    if w < 1:
        w = 1
    if h < 1:
        h = 1
    return [bx + (bw - w) / 2, by + (bh - h) / 2, w, h]


# centre(iw, ih, bx, by, bw, bh) -> [x, y, w, h] at the image's OWN size, with
# no scaling. The other half of a viewer's fit/actual toggle. An image larger
# than the box keeps its size and is clipped by the window -- that is what
# "actual size" means, and quietly shrinking it would make the toggle a no-op
# on exactly the images it exists for.
def centre(iw, ih, bx, by, bw, bh):
    return [bx + (bw - iw) / 2, by + (bh - ih) / 2, iw, ih]
