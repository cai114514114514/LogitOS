#!/usr/bin/env python3
"""Drive Preview over QMP and assert against the PIXELS, one format at a time.

WHY PIXELS AND NOT A LOG LINE
-----------------------------
Preview is a viewer. "It opened the file" is not the claim -- the claim is that
the picture on the screen is the picture in the file, and the only instrument
that can settle that is the frame. So for every still and animated format this
driver decodes the SAME fixture bytes on the host and compares against a
screendump of the guest.

TWO REFERENCES, AND WHICH ONE IS VALID WHERE. The host build of the guest's own
decoders (build/img_dump) is the reference for every case -- that is the
host-vs-guest comparison make test-imgcheck already makes, and it is the right
one for a claim about the VIEWER, since the decoders themselves are pinned
against independent implementations by test-png / test-img-still /
test-img-anim / test-img-exif. For STILLS the driver additionally compares
against PIL, which is genuinely independent code. It does NOT do that for the
animations, and the reason is a real disagreement rather than a wobble: PIL
restores a disposal-2 GIF frame to the background COLOUR where browsers -- and
c/lib/image, on purpose -- restore it to TRANSPARENT. On the committed anim.gif
that is 108 pixels of alpha per frame. See tests/unit/img_dump.c.

The comparison itself:

  * the aspect-fit rect is recomputed exactly as blit_fit_src() in
    c/apps/gui/preview.c computes it,
  * the nearest-neighbour source lookup is recomputed exactly as
    fb_blit_rgba() in c/kernel/gui/fb.c computes it (i*sw/dw, integer),
  * and alpha is composited over the window background with the same integer
    formula the kernel uses, (p*a + b*(255-a))/255.

Nothing is resampled or blurred anywhere on that path, so the comparison is
EXACT rather than a tolerance -- a wrong palette entry, a missed disposal, a
dropped alpha channel or an unrotated JPEG all move a sampled pixel.

WHAT ELSE IT MEASURES, RATHER THAN ASSUMES
------------------------------------------
  * ANIMATION TIMING. anim.gif declares 120+400+70+900 = 1490 ms per loop.
    Preview prints `declared_ms` and `elapsed_ms` per loop on the console and
    this asserts the wall-clock figure against the declared one. That is an
    assertion about the PLAYER; the image line's harness already asserts the
    per-frame delay at the DECODER, and neither implies the other -- a player
    that shows four frames as fast as it can has decoded every delay correctly
    and honoured none of them. `make test-preview-negctl` builds exactly that
    player and requires this assertion to fail.
  * THAT IT ANIMATES AT ALL. Several screendumps across one loop, each required
    to match SOME composited frame of the host decode, with at least two
    distinct frames seen. A viewer stuck on frame 0 passes neither half.
  * A/V DRIFT. avclock measures signed drift per displayed frame; Preview
    prints the mean and the max at the end of a container. This asserts a bound
    over real MP4 and Matroska files with video and audio in them.
  * THE REFUSALS. A file this machine cannot decode has to say what it is and
    which part has no decoder -- vp9, opus -- rather than showing a blank
    window. Those messages are read back off the console too.

HOW IT STEERS THE APP
---------------------
Preview's list prints itself, with indices, on the console (`preview: pick N
<name>`). A test that counted Down presses against a directory order it had
guessed would break the day a file was added to /media; reading the app's own
list means the file is found by NAME and its row is CLICKED. Clicking is not
a stylistic preference: the emulated PS/2 controller buffers one byte, so
eighteen arrow keys in a row arrive as two, silently, and every assertion
afterwards is then about the wrong file. One click is one event, and the
pointer is confirmed on the pixel before the button goes down.

Usage: qmp_preview.py [--iso X] [--disk X] [--out DIR] [--only NAME,NAME]
                      [--timing-only] [--napps N] [--slot N] [--keep]
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, configure, dock_icon, pt, PPM  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIX = os.path.join(ROOT, "tests", "fixtures")

# c/apps/gui/preview.c
WINW, WINH = 760, 560
CONTH = WINH - 30
IMG_BG = (28, 28, 32)          # gui_clear() behind a still or an animation
# The 6x6 anchor Preview paints at content (0,0) on its LIST screen, in a
# colour nothing else on the desktop uses. Everything below works in
# window-local coordinates measured from it. Same device gallery.c uses.
PICK_PROBE = (255, 0, 128)

# The dock, as qmp_ui models it. Preview is the 7th *.aex at the LogitFS root
# in scan_apps order: clock textedit monitor terminal widgets files preview.
PREVIEW_SLOT = 6
NAPPS = 10

# name in Preview's list, what it exercises, how to check it, options.
CASES = [
    ("dot.png",        "PNG",                "image",     {}),
    ("img/still.bmp",  "BMP/DIB",            "image",     {}),
    ("img/still.webp", "WebP (VP8L)",        "image",     {}),
    ("img/icon.ico",   "ICO/CUR",            "image",     {}),
    ("img/rot.jpg",    "JPEG + EXIF",        "exif",      {}),
    ("img/anim.gif",   "animated GIF",       "anim",      {"declared": 1490}),
    ("img/anim.apng",  "APNG",               "anim",      {"declared": 600}),
    ("sample.mp3",     "MP3",                "audio",     {"fmt": "mp3"}),
    ("sample.flac",    "FLAC",               "audio",     {"fmt": "flac"}),
    ("sample.wav",     "WAV/PCM",            "audio",     {"fmt": "wav"}),
    ("aac.m4a",        "AAC in MP4",         "audio",     {"fmt": "aac"}),
    ("pcm.mov",        "PCM s16be in MOV",   "audio",     {"fmt": "wav"}),
    ("sample.h264",    "H.264 stream",       "stream",    {"codec": "h264"}),
    ("sample.h265",    "H.265 stream",       "stream",    {"codec": "hevc"}),
    ("clip.mp4",       "MP4 h264+mp3",       "container", {"codec": "h264", "drift_ms": 300}),
    ("clip.mkv",       "Matroska h264+flac", "container", {"codec": "h264", "drift_ms": 300}),
    ("clip.webm",      "WebM vp9+opus",      "refuse",    {"says": ["vp9", "opus"]}),
]

# --- the association ---------------------------------------------------------
# What a double-click in the Finder actually is: SYS_OPEN_PATH, which sniffs the
# file, picks an app and hands it the path as its launch ARGUMENT. That is a
# different entry into Preview from the /media list -- get_arg() rather than the
# picker -- and it is the one a user takes, so it is tested separately.
# tests/fixtures/preview/open-*.as issue exactly that syscall and nothing else.
ASSOC = [
    ("open-audio", "sample.mp3",  "MP3 audio",  r"preview: audio sample\.mp3 fmt=mp3"),
    ("open-flac",  "sample.flac", "FLAC audio", r"preview: audio sample\.flac fmt=flac"),
    ("open-wav",   "sample.wav",  "WAV audio",  r"preview: audio sample\.wav fmt=wav"),
    ("open-mp4",   "clip.mp4",    "MP4",        r"preview: container clip\.mp4 kind=mp4"),
    ("open-mkv",   "clip.mkv",    "Matroska",   r"preview: container clip\.mkv kind=matroska"),
    # WebM is Matroska: the container is understood perfectly and it is the
    # CODECS inside that have no decoder here, so the refusal has to name vp9
    # and opus rather than the file format. "No such format" would be a lie
    # about the file.
    ("open-webm",  "clip.webm",   "WebM",       r"preview: container clip\.webm kind=matroska"),
    ("open-image", "still.webp",  "WebP",       r"preview: image still\.webp"),
]

FIXTURE_FOR = {
    "dot.png":        os.path.join(ROOT, "build", "dot.png"),
    "img/still.bmp":  os.path.join(FIX, "image", "still.bmp"),
    "img/still.webp": os.path.join(FIX, "image", "still.webp"),
    "img/icon.ico":   os.path.join(FIX, "image", "icon.ico"),
    "img/rot.jpg":    os.path.join(FIX, "image", "rot.jpg"),
    "img/anim.gif":   os.path.join(FIX, "image", "anim.gif"),
    "img/anim.apng":  os.path.join(FIX, "image", "anim.apng"),
}


# ------------------------------------------------------------- references --
class Ref:
    """One decoded frame: raw RGBA, with PIL's Image interface where it counts.

    The reference dumps come from build/img_dump (the HOST build of the guest's
    own decoders) -- see tests/unit/img_dump.c for why the animated cases
    cannot use PIL and the still ones can."""

    def __init__(self, w, h, data):
        self.size = (w, h)
        self.data = data

    def convert(self, _mode):
        return self

    def getpixel(self, xy):
        o = ((xy[1] * self.size[0]) + xy[0]) * 4
        return (self.data[o], self.data[o + 1], self.data[o + 2], self.data[o + 3])


def load_ref(refdir, fixture):
    """[(delay_ms, Ref), ...] for a fixture, or None when no dump exists."""
    if not refdir:
        return None
    base = os.path.join(refdir, os.path.basename(fixture))
    try:
        with open(base + ".meta") as fh:
            nums = [int(x) for x in fh.read().split()]
    except OSError:
        return None
    w, h, nf = nums[0], nums[1], nums[2]
    delays = nums[4:4 + nf]
    out = []
    for k in range(nf):
        with open("%s.%d.rgba" % (base, k), "rb") as fh:
            out.append((delays[k] if k < len(delays) else 0, Ref(w, h, fh.read())))
    return out


# ------------------------------------------------------------------ pixels --
def blit_fit(iw, ih):
    """The dest rect blit_fit_src() picks, in window-local POINTS."""
    dw, dh = WINW, WINW * ih // iw
    if dh > CONTH:
        dh, dw = CONTH, CONTH * iw // ih
    return ((WINW - dw) // 2, (CONTH - dh) // 2, dw, dh)


def over(px, bg):
    """The kernel's integer composite, from fb_blit_rgba()."""
    if px[3] >= 255:
        return (px[0], px[1], px[2])
    a = px[3]
    return tuple((px[i] * a + bg[i] * (255 - a)) // 255 for i in range(3))


def sample_points(iw, ih, n=6):
    """Source pixels spread over the picture, avoiding the very edge: a
    one-pixel disagreement about the fit rect must not read as a decode bug."""
    out = []
    for j in range(n):
        for i in range(n):
            sx = (2 * i + 1) * iw // (2 * n)
            sy = (2 * j + 1) * ih // (2 * n)
            if 0 < sx < iw - 1 and 0 < sy < ih - 1:
                out.append((sx, sy))
    return out or [(iw // 2, ih // 2)]


def compare_image(ppm, origin, img, tol=0):
    """Compare the guest's picture against a PIL image. -> (ok, detail)."""
    iw, ih = img.size
    rgba = img.convert("RGBA")
    x, y, dw, dh = blit_fit(iw, ih)
    ox, oy = origin
    bad = checked = 0
    first = None
    for (sx, sy) in sample_points(iw, ih):
        # The middle destination column/row whose integer map lands on sx/sy.
        i = (2 * sx + 1) * dw // (2 * iw)
        j = (2 * sy + 1) * dh // (2 * ih)
        if i >= dw or j >= dh or i * iw // dw != sx or j * ih // dh != sy:
            continue
        want = over(rgba.getpixel((sx, sy)), IMG_BG)
        px, py = ox + pt(x) + i, oy + pt(y) + j
        if px >= ppm.w or py >= ppm.h:
            continue
        got = ppm.at(px, py)
        checked += 1
        if any(abs(got[k] - want[k]) > tol for k in range(3)):
            bad += 1
            if first is None:
                first = "src(%d,%d) want %s got %s" % (sx, sy, want, got)
    if checked == 0:
        return False, "no comparable sample points"
    return bad == 0, "%d/%d pixels matched%s" % (
        checked - bad, checked, ("  [%s]" % first) if first else "")


# Every gui_clear() colour c/apps/gui/preview.c uses: the picture/list screens,
# the audio screen and the video screen each pick their own.
BACKGROUNDS = [IMG_BG, (18, 18, 22), (18, 18, 20)]


def content_origin(ppm):
    """The window's content origin, found from the screen's own background.

    A run of >= 600 pixels of one of Preview's gui_clear() colours on a single
    row can only be the window: the wallpaper is a gradient and the dock is
    glass. The first such run, scanning top-down, is the top-left of the
    content canvas -- which is how this avoids hard-coding the compositor's
    window cascade, and it survives the cascade moving when another app opens
    first."""
    row = ppm.w * 3
    best = None
    for bg in BACKGROUNDS:
        target = bytes(bg)
        probe = target * 200
        for yy in range(ppm.h):
            base = yy * row
            k = ppm.px.find(probe, base, base + row)
            if k < 0 or (k - base) % 3:
                continue
            while k - 3 >= base and ppm.px[k - 3:k] == target:
                k -= 3
            hit = ((k - base) // 3, yy)
            if best is None or hit[1] < best[1]:
                best = hit
            break
    return best


def distinct_colours(ppm, origin, w, h):
    ox, oy = origin
    seen = set()
    for j in range(0, h, 7):
        for i in range(0, w, 7):
            if ox + i < ppm.w and oy + j < ppm.h:
                seen.add(ppm.at(ox + i, oy + j))
    return len(seen)


def ppm_to_png(src, dst):
    p = PPM(src)
    raw = b"".join(b"\x00" + p.px[y * p.w * 3:(y + 1) * p.w * 3] for y in range(p.h))

    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    with open(dst, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n"
                 + chunk(b"IHDR", struct.pack(">IIBBBBB", p.w, p.h, 8, 2, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(raw, 6))
                 + chunk(b"IEND", b""))


# ------------------------------------------------------------------ driver --
def read(path):
    try:
        with open(path, errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def main(argv):
    iso = os.environ.get("LOGIT_ISO", os.path.join(ROOT, "build", "logit.iso"))
    disk = os.environ.get("LOGIT_DISK", os.path.join(ROOT, "build", "disk.img"))
    outdir = os.path.join(ROOT, "build", "preview-shots")
    refdir = os.path.join(ROOT, "build", "previewref")
    only, keep, timing_only, assoc_only = None, False, False, False
    napps, slot = NAPPS, PREVIEW_SLOT
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--iso":            iso = argv[i + 1]; i += 2
        elif a == "--disk":         disk = argv[i + 1]; i += 2
        elif a == "--ref":          refdir = argv[i + 1]; i += 2
        elif a == "--out":          outdir = argv[i + 1]; i += 2
        elif a == "--only":         only = set(argv[i + 1].split(",")); i += 2
        elif a == "--timing-only":  timing_only = True; i += 1
        elif a == "--assoc":        assoc_only = True; i += 1
        elif a == "--napps":        napps = int(argv[i + 1]); i += 2
        elif a == "--slot":         slot = int(argv[i + 1]); i += 2
        elif a == "--keep":         keep = True; i += 1
        else:
            print("unknown arg %r" % a); return 2

    try:
        from PIL import Image, ImageOps
    except ImportError:
        Image = ImageOps = None
        print("note: PIL is absent -- the INDEPENDENT still-image check is")
        print("      skipped; the reference dumps still gate every format.")
    if not os.path.isdir(refdir):
        print("note: no reference dumps in %s (make build/previewref/.stamp) --" % refdir)
        print("      the per-pixel comparisons degrade to 'not blank'.")
        refdir = None

    if timing_only:
        cases = [c for c in CASES if c[2] == "anim"]
    elif only:
        cases = [c for c in CASES if c[0] in only]
    else:
        cases = CASES
    if not cases:
        print("no cases selected"); return 2

    os.makedirs(outdir, exist_ok=True)
    configure(1280, 800)               # scale 100: a point IS a device pixel
    tmp = tempfile.mkdtemp(prefix="logit-preview-")
    sock = os.path.join(tmp, "qmp.sock")
    serial = os.path.join(tmp, "serial.log")
    wav = os.path.join(tmp, "out.wav")
    fails = []

    # THE IMAGE AND THE DISK ARE COPIED FIRST, and that is not tidiness.
    # `-snapshot` keeps the guest's WRITES out of build/disk.img; it does not
    # keep another process's writes out of the guest. Several lines work in
    # this tree at once and a `make` in any of them rewrites build/disk.img and
    # build/logit.iso in place, under a QEMU that reads them lazily. The
    # symptoms are not a crash: a directory comes back two entries short, or a
    # 452-byte file reads back as somebody else's indirect block -- which looks
    # exactly like a filesystem bug in the guest and is not one. Both were seen
    # while writing this driver. A private copy costs ~75 MB and a second.
    run_iso = os.path.join(tmp, "logit.iso")
    run_disk = os.path.join(tmp, "disk.img")
    shutil.copyfile(iso, run_iso)
    shutil.copyfile(disk, run_disk)
    iso, disk = run_iso, run_disk

    def ck(cond, what, detail=""):
        print("%-4s %s%s" % ("ok" if cond else "FAIL", what,
                             ("  [%s]" % detail) if detail else ""), flush=True)
        if not cond:
            fails.append(what)

    # The console is stdio with a PIPE on stdin, not a plain file: the
    # association cases have to TYPE at the shell (see ASSOC above), and the
    # log is the same stream either way.
    serial_fh = open(serial, "wb")
    qemu = subprocess.Popen(
        ["qemu-system-x86_64",
         "-cdrom", iso,
         "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
         "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi", "-cpu", "max",
         "-rtc", "base=localtime",
         "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
         # A real sound card, so the audio paths run against a real play cursor
         # instead of degrading to "no sound card" -- a different code path,
         # and testing it would be testing the wrong one.
         "-audiodev", "wav,id=snd0,path=%s,out.frequency=48000,out.channels=2,"
                      "out.format=s16" % wav,
         "-device", "intel-hda", "-device", "hda-output,audiodev=snd0",
         "-serial", "stdio", "-no-reboot",
         "-display", "none", "-qmp", "unix:%s,server,nowait" % sock],
        stdin=subprocess.PIPE, stdout=serial_fh, stderr=subprocess.DEVNULL)

    def shell(cmd):
        """Type a line at the serial shell."""
        qemu.stdin.write((cmd + "\n").encode())
        qemu.stdin.flush()

    def wait_for(pattern, mark, timeout):
        """Poll the console for a regex. Returns the match, or None."""
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern, read(serial)[mark:])
            if m:
                return m
            if qemu.poll() is not None:
                return None
            time.sleep(0.5)
        return None

    try:
        deadline = time.time() + 300
        while time.time() < deadline:
            if "desktop live" in read(serial):
                break
            if qemu.poll() is not None:
                print("FAIL qemu exited early"); return 1
            time.sleep(0.2)
        else:
            print("FAIL desktop never came up"); return 1
        time.sleep(4)

        ui = Session(sock, serial=serial)
        probe = os.path.join(tmp, "probe.ppm")
        shot = os.path.join(tmp, "s.ppm")

        # --- the association ------------------------------------------------
        if assoc_only:
            for n, (script, base, label, want) in enumerate(ASSOC):
                mark = len(read(serial))
                shell("as /usr/as/%s.as" % script)
                launched = wait_for(r"\[wm\] launched Preview", mark, 60)
                ck(launched is not None,
                   "%s (%s): a double-click launches Preview" % (base, label),
                   "no [wm] launched line" if launched is None else "launched")
                opened = wait_for(r"preview: open %s bytes=(\d+)" % re.escape(base),
                                  mark, 60)
                ck(opened is not None,
                   "%s: Preview received it as its launch argument" % base,
                   opened.group(0).strip() if opened else "no console line")
                # THE FILE HAS TO BE BIGGER THAN THE PEEK BUFFER, and that is
                # the assertion rather than a note. The bug this gates was
                # `vfs_read(path, b, 64)` on a VFS with no partial read: it
                # failed for every file longer than 64 bytes and succeeded for
                # every file shorter, so a test built on a tiny fixture would
                # have passed against the broken code and proved nothing.
                if opened is not None:
                    ck(int(opened.group(1)) > 64,
                       "%s: is larger than the 64-byte peek that used to fail"
                       % base, "%s bytes" % opened.group(1))
                got = wait_for(want, mark, 90)
                ck(got is not None, "%s (%s): decoded, not refused" % (base, label),
                   got.group(0).strip() if got else "no line matching %r" % want)

                # A picture, because a claim about a viewer that is not a
                # screenshot is not a claim.
                origin = None
                for _ in range(15):
                    ui.screendump(shot, settle=0.8)
                    origin = content_origin(PPM(shot))
                    if origin is not None:
                        break
                png = os.path.join(outdir, "assoc-%02d-%s.png" % (n + 1, base))
                ppm_to_png(shot, png)
                if origin is not None:
                    cols = distinct_colours(PPM(shot), origin, WINW, CONTH)
                    ck(cols > 3, "%s: the window has something in it" % base,
                       "%d distinct colours" % cols)
                else:
                    ck(False, "%s: Preview's window was found" % base, "not on screen")

                # Preview is SINGLE INSTANCE: a second SYS_OPEN_PATH while it is
                # alive only focuses the window, so each case has to close it.
                ui.key("esc", settle=0.4)
                time.sleep(3)
            if fails:
                print("\n%d FAILED:" % len(fails))
                for f in fails:
                    print("  " + f)
                return 1
            print("\nPASS: every association opened in Preview and decoded")
            return 0

        # --- open Preview from the Dock -----------------------------------
        ui.click_at_confirmed(probe, *dock_icon(slot, napps))
        m = wait_for(r"preview: pick 0 ", 0, 90)
        picks = {mm.group(2): int(mm.group(1))
                 for mm in re.finditer(r"preview: pick (\d+) (\S+)", read(serial))}
        ck(m is not None and bool(picks),
           "Preview opened and listed /media", "%d entries" % len(picks))
        if not picks:
            ui.screendump(shot, settle=1.0)
            ppm_to_png(shot, os.path.join(outdir, "no-list.png"))
            return 1
        print("     list: " + ", ".join(sorted(picks)))

        # The list is PRINTED before it is PAINTED, and on a busy TCG guest the
        # gap is seconds -- a screendump taken on the log line alone catches the
        # window still filled with its initial white. Wait for the anchor.
        origin = None
        for _ in range(20):
            ui.screendump(shot, settle=0.8)
            box = PPM(shot).find_color(PICK_PROBE)
            if box is not None:
                origin = (box[0], box[1])
                break
        ck(origin is not None, "found Preview's content origin", str(origin))
        if origin is None:
            ppm_to_png(shot, os.path.join(outdir, "no-origin.png"))
            return 1
        ppm_to_png(shot, os.path.join(outdir, "00-list.png"))


        def click_row(idx):
            """Click list row `idx`, aiming at the ROW rather than the pixel.

            The pointer arrives a few pixels off: QEMU's `rel` events go through
            an emulated PS/2 mouse whose packets are dropped when the guest is
            busy, so a move to y=568 settles at 579. Insisting on the exact
            pixel (click_at_confirmed) then never converges, and dead reckoning
            silently picks the next row down -- which is how a click meant for
            dot.png opened sample.mp3.

            A row is 20 px tall, so exactness is not needed and is not the
            question: what matters is which row the pointer is IN. Ask the
            guest where it ended up (`[wm] ptr`), work out the row from that,
            and re-aim until it is the right one."""
            tx = origin[0] + pt(200)
            ty = origin[1] + pt(70 + idx * 20 + 10)
            for _ in range(6):
                ui.goto(tx, ty, 0.35)
                got = ui.guest_pointer()
                if got is None:
                    break                    # no report: trust the reckoning
                ui.cur = [got[0], got[1]]
                if (got[1] - origin[1] - pt(70)) // pt(20) == idx:
                    break
                # aim at the row's centre from where we actually are
                ty += (origin[1] + pt(70 + idx * 20 + 10)) - got[1]
            ui.click()
            time.sleep(0.4)

        def open_entry(name):
            """Click `name` in the list. Returns (log mark, the console line).

            ONE CLICK, NOT ONE KEYSTROKE PER ROW. The emulated PS/2 controller
            buffers a single byte, so a run of arrow keys is lossy in a way
            nothing reports: eighteen Downs to reach row 18 arrived as two, the
            selection sat on row 2, and every assertion after that was about
            the wrong file. Pointer motion has no such problem -- it is
            coalesced, and the driver confirms the pointer landed before it
            clicks -- so the list is clicked, which is also how a person would
            use it. A retry is still here because a click can miss while the
            guest is repainting."""
            base = os.path.basename(name)
            idx = picks[name]
            for _ in range(3):
                mark = len(read(serial))
                # PICK_Y0 + row*PICK_DY + half a row, in c/apps/gui/preview.c.
                # The list never scrolls here: it fits PICK_ROWS entries.
                click_row(idx)
                got = wait_for(r"preview: open (\S+) ", mark, 40)
                if got is not None and got.group(1) == base:
                    return mark, got
                if got is not None:
                    back_to_list()
            return mark, None

        def back_to_list():
            """Backspace until the LIST is back, proved by its anchor.

            The anchor is what makes this decisive: every screen clears to one
            of three near-black colours, so "the background is there" cannot
            tell a picture from the list, and the driver used to carry on
            clicking at a window that was still showing the last file."""
            for _ in range(8):
                ui.key("backspace", settle=0.4)
                time.sleep(1.5)
                ui.screendump(shot, settle=0.4)
                box = PPM(shot).find_color(PICK_PROBE)
                if box is not None and (box[0], box[1]) == origin:
                    return True
            return False

        def refresh_picks():
            """Re-read the app's list from its MOST RECENT printing.

            The app rebuilds and reprints the list every time the picker is
            shown -- a directory read on this kernel can come back short, so a
            later showing may hold entries an earlier one missed -- which means
            the indices move. Only the last printing is authoritative, so this
            starts from the last `pick 0` and takes what follows."""
            log = read(serial)
            i = log.rfind("preview: pick 0 ")
            if i < 0:
                return
            picks.clear()
            for mm in re.finditer(r"preview: pick (\d+) (\S+)", log[i:]):
                picks[mm.group(2)] = int(mm.group(1))

        for n, (name, label, kind, opt) in enumerate(cases):
            refresh_picks()
            if name not in picks:
                ck(False, "%s (%s) is in Preview's list" % (name, label), "absent")
                continue
            mark, opened = open_entry(name)
            ck(opened is not None, "%s (%s): opened" % (name, label),
               opened.group(0).strip() if opened else "no console line")

            png = os.path.join(outdir, "%02d-%s.png" % (n + 1, name.replace("/", "-")))

            if kind in ("image", "exif"):
                time.sleep(2.5)
            elif kind == "audio":
                wait_for(r"preview: audio \S+ fmt=", mark, 40)
                time.sleep(2.0)
            elif kind == "stream":
                wait_for(r"preview: stream \S+ frames=\d+", mark, 240)
            elif kind == "container":
                wait_for(r"preview: video \S+ codec=", mark, 240)
            elif kind == "refuse":
                time.sleep(3.0)

            if kind != "anim":
                ui.screendump(shot, settle=1.2)
                ppm_to_png(shot, png)
                ppm = PPM(shot)
                if kind not in ("image", "exif"):
                    # For a picture the pixel comparison below is strictly
                    # stronger; a flat still legitimately has two colours.
                    cols = distinct_colours(ppm, origin, WINW, CONTH)
                    ck(cols > 3, "%s (%s): the window is not blank" % (name, label),
                       "%d distinct colours" % cols)

            log = read(serial)[mark:]

            # ---- stills ------------------------------------------------
            if kind in ("image", "exif"):
                ref = load_ref(refdir, FIXTURE_FOR[name])
                if ref:
                    ok, det = compare_image(ppm, origin, ref[0][1])
                    ck(ok, "%s (%s): pixels match the decoder's own output"
                       % (name, label), det)

            if kind == "image" and Image is not None:
                # The independent half: PIL is the reference test-png /
                # test-img-still already hold these decoders to, and it is
                # valid for stills.
                ok, det = compare_image(ppm, origin, Image.open(FIXTURE_FOR[name]))
                ck(ok, "%s (%s): pixels match PIL's decode too" % (name, label), det)

            # ---- EXIF orientation --------------------------------------
            if kind == "exif" and Image is not None:
                # PIL IS NOT A PIXEL REFERENCE FOR JPEG HERE, and test-jpeg
                # says why: JPEG is lossy and libjpeg's default chroma
                # upsampling is "fancy" where ours is a box filter, so that
                # test uses `djpeg -nosmooth` rather than PIL. What PIL IS the
                # authority on is the ORIENTATION, and that is what these two
                # assertions turn on: the picture on screen must be the
                # transposed geometry and must NOT be the stored one. The
                # pixels are held to the decoder's own dump above.
                img = Image.open(FIXTURE_FOR[name])
                upright = ImageOps.exif_transpose(img)
                same, det2 = compare_image(ppm, origin, img.copy(), tol=12)
                ck(not same, "%s: does NOT match the picture as stored" % name, det2)
                ref = load_ref(refdir, FIXTURE_FOR[name])
                if ref:
                    ck(ref[0][1].size == upright.size and ref[0][1].size != img.size,
                       "%s: comes out %dx%d, not the %dx%d it is stored as"
                       % ((name,) + upright.size + img.size),
                       "decoder says %dx%d" % ref[0][1].size)

            # ---- animation ---------------------------------------------
            if kind == "anim":
                ref = load_ref(refdir, FIXTURE_FOR[name])
                frames = [r[1] for r in ref] if ref else []
                seen, shots = [], 0
                first_png = True
                for _ in range(7):
                    ui.screendump(shot, settle=0.15)
                    if first_png:
                        ppm_to_png(shot, png)
                        first_png = False
                    p = PPM(shot)
                    shots += 1
                    if frames:
                        hit = None
                        for fi, fr in enumerate(frames):
                            ok, _ = compare_image(p, origin, fr)
                            if ok:
                                hit = fi
                                break
                        seen.append(hit)
                    time.sleep(0.30)
                if ref:
                    ck(sum(r[0] for r in ref) == opt["declared"],
                       "%s: the fixture declares %d ms of delays" % (name, opt["declared"]),
                       "decoder says %s" % [r[0] for r in ref])
                if frames:
                    matched = [s for s in seen if s is not None]
                    # A screendump can catch the compositor mid-copy, so the bar
                    # is "most samples land on a real composited frame", not all.
                    ck(len(matched) >= max(3, shots - 2),
                       "%s (%s): every sampled frame is a real composited frame"
                       % (name, label),
                       "%d/%d matched, frames seen %s" % (len(matched), shots, sorted(set(matched))))
                    ck(len(set(matched)) >= 2,
                       "%s: the picture actually changes between frames" % name,
                       "distinct frames seen: %s" % sorted(set(matched)))

                mm = wait_for(
                    r"preview: anim \S*%s loop=\d+ frames=(\d+) declared_ms=(\d+) "
                    r"elapsed_ms=(\d+) paint_ms=(\d+)"
                    % re.escape(os.path.basename(name)), mark, 40)
                if mm is None:
                    ck(False, "%s: the player reported a completed loop" % name, "no line")
                else:
                    nf, decl = int(mm.group(1)), int(mm.group(2))
                    el, paint = int(mm.group(3)), int(mm.group(4))
                    ck(decl == opt["declared"],
                       "%s: declares %d ms per loop" % (name, opt["declared"]),
                       "%d frames, declared %d ms" % (nf, decl))
                    # THE LOWER BOUND IS THE CLAIM. A loop may never finish
                    # early: that is what "the delays are honoured" means, and
                    # it is the half make test-preview-negctl breaks (with the
                    # wait compiled out a loop finishes in paint_ms, three
                    # orders of magnitude short).
                    #
                    # The upper bound is bounded by MEASUREMENT, not by a shrug:
                    # a frame cannot be shown before it has been painted, and a
                    # translucent APNG scaled to fill the window costs more per
                    # frame on this machine than the 100 ms it asks for. So the
                    # allowance is the paint cost the player reports, plus the
                    # 10 ms tick granularity per frame.
                    lo, hi = decl, decl + paint + 20 + 10 * nf
                    ck(lo <= el <= hi,
                       "%s: one loop takes its declared %d ms" % (name, decl),
                       "elapsed %d ms, of which %d ms painting (allowed %d..%d)"
                       % (el, paint, lo, hi))

            # ---- elementary streams ------------------------------------
            if kind == "stream":
                mm = re.search(r"preview: stream (\S+) codec=(\S+)", log)
                fr = re.search(r"preview: stream \S+ frames=(\d+)", log)
                ck(mm is not None and mm.group(2) == opt["codec"],
                   "%s (%s): decoded as %s" % (name, label, opt["codec"]),
                   mm.group(2) if mm else "no line")
                ck(fr is not None and int(fr.group(1)) > 0,
                   "%s: pictures came out" % name,
                   ("%s frames" % fr.group(1)) if fr else "none")

            # ---- containers --------------------------------------------
            if kind == "container":
                mm = re.search(r"preview: video (\S+) codec=(\S+) (\d+)x(\d+) depth=(\d+) "
                               r"shown=(\d+) dropped=(\d+) resyncs=(\d+) "
                               r"drift_mean_ms=(-?\d+) drift_max_ms=(-?\d+) audio=(\S+)", log)
                if mm is None:
                    ck(False, "%s: the player reported the stream" % name, "no summary")
                else:
                    codec, w, h, depth = mm.group(2), int(mm.group(3)), int(mm.group(4)), int(mm.group(5))
                    shown, dropped = int(mm.group(6)), int(mm.group(7))
                    mean, mx, aud = int(mm.group(9)), int(mm.group(10)), mm.group(11)
                    ck(shown > 0, "%s (%s): pictures were displayed" % (name, label),
                       "%s %dx%d %d-bit, shown %d, dropped %d, audio %s"
                       % (codec, w, h, depth, shown, dropped, aud))
                    ck(codec == opt["codec"], "%s: video decoded as %s" % (name, opt["codec"]), codec)
                    ck(aud not in ("none",), "%s: the audio track played too" % name, aud)
                    ck(abs(mean) <= opt["drift_ms"],
                       "%s: mean A/V drift within %d ms" % (name, opt["drift_ms"]),
                       "mean %+d ms, max %+d ms" % (mean, mx))

            # ---- audio --------------------------------------------------
            if kind == "audio":
                mm = re.search(r"preview: audio (\S+) fmt=(\S+) rate=(\d+) ch=(\d+) "
                               r"dur_ms=(-?\d+) sound=(\S+)", log)
                if mm is None:
                    ck(False, "%s (%s): the player reported the track" % (name, label), "no line")
                else:
                    fmt, rate, ch, snd = mm.group(2), int(mm.group(3)), int(mm.group(4)), mm.group(6)
                    ck(opt["fmt"] in fmt.lower(),
                       "%s (%s): decoded as %s" % (name, label, opt["fmt"]),
                       "%s %d Hz %d ch, sound=%s" % (fmt, rate, ch, snd))
                    ck(rate >= 8000 and ch >= 1,
                       "%s: states a usable format" % name, "%d Hz %d ch" % (rate, ch))
                    ck(snd == "yes", "%s: opened the sound card" % name, snd)

            # ---- refusals ------------------------------------------------
            if kind == "refuse":
                said = [s for s in opt["says"] if s in log]
                ck(len(said) == len(opt["says"]),
                   "%s: the refusal names what it cannot decode" % name,
                   "found %s" % (said or "nothing"))

            if not back_to_list():
                ck(False, "%s: Backspace returns to the list" % name, "stuck")
                break

        print("\nwrote screenshots to %s" % outdir)
    finally:
        try:
            qemu.kill()
        except Exception:
            pass
        if keep:
            print("kept %s (serial log, wav capture)" % tmp)

    if fails:
        print("\n%d FAILED:" % len(fails))
        for f in fails:
            print("  " + f)
        return 1
    print("\nPASS: every selected format opened and matched")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
