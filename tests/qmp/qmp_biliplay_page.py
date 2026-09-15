#!/usr/bin/env python3
"""Play the FIRST VIDEO on bilibili, on the real machine, over the real wire.

    python3 tests/qmp/qmp_biliplay_page.py <iso> <disk.img>
                                            [--mode live|offline|probe|mirror]
                                            [--specimen URL] [--secs N]
                                            [--control no-video] [--trace-input]

WHY THIS HARNESS EXISTS (and why nothing else in the tree answers it). The
video line proved the pipeline end to end on LOCAL fixtures: <video src> direct
(qmp_video_page layer 1), MSE segmented playback (layer 2), subtitles as pixels.
What none of that proves is the thing the owner asked for -- the real site's
player stack driving OUR media engine. A real player negotiates codecs through
isTypeSupported, resolves its segments through a playurl API, fetches them
cross-origin from a CDN, and appends DASH-shaped fMP4 whose timescales and
profiles came off some encoder we never met. Every one of those steps can fail
in a way no local fixture can reproduce, so this drives the actual page:

  https://www.bilibili.com/video/BV1GJ411x7h7/

THE MEASUREMENT, in the house's counting rules: FRAMES SHOWN and captured PCM.
A screendump cannot tell a stall from a black frame; "painted" is not
"playing". The engine's own counters are printed to the serial console by
js_media.c at 1 Hz ("[media] stats shown=... decoded=... audio=..."), a channel
the page cannot write to, and the PCM is read off QEMU's wav backend -- bytes
on the host's disk, not a clean log line.

--mode offline re-vehicles the SAME assertion on locally-served copies of the
SAME video's init+media segments (fetched once into tests/fixtures/biliplay/):
same engine, same append path, same counting -- no network needed. That is the
ci-boot ratchet; the live gate skips LOUDLY without connectivity and names the
command that would settle it.

AN HONEST REFUSAL IS A RESULT. If the CDN refuses our truthful UA or the site
requires login for playback, the live gate reports exactly that and the offline
vehicle is the one that carries the regression burden.
"""

import os
import re
import sys
import subprocess
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session              # noqa: E402
import qmp_addrbar                      # noqa: E402  caret-derived address-bar geometry

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

MODE = "live"
SPECIMEN = "https://www.bilibili.com/video/BV1GJ411x7h7/"
SECS = 150
CONTROL = ""
TRACE_INPUT = False
_args = sys.argv[3:]
_i = 0
while _i < len(_args):
    if _args[_i] == "--mode" and _i + 1 < len(_args):
        MODE = _args[_i + 1]
    elif _args[_i] == "--specimen" and _i + 1 < len(_args):
        SPECIMEN = _args[_i + 1]
    elif _args[_i] == "--secs" and _i + 1 < len(_args):
        SECS = int(_args[_i + 1])
    elif _args[_i] == "--control" and _i + 1 < len(_args):
        CONTROL = _args[_i + 1]
    elif _args[_i] == "--trace-input":
        TRACE_INPUT = True
    _i += 1

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BILIFX = os.path.join(ROOT, "tests", "fixtures", "biliplay")

tmp = tempfile.mkdtemp(prefix="qmp_biliplay_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
wav_path = os.path.join(tmp, "guest.wav")

served = []


# ---- the offline vehicle: same segments, served next door ----------------
class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        served.append(self.path)
        raw, ctype = None, "application/octet-stream"
        if self.path.startswith("/page"):
            # BaseHTTPRequestHandler's stream accepts bytes only. Returning the
            # Python str writes the headers, raises TypeError in wfile.write,
            # and leaves the guest diagnosing a plausible but false
            # "connection closed mid-message" network failure.
            if MODE == "probe":
                page = probe_page()
            elif MODE == "mirror":
                page = mirror_page()
            else:
                page = offline_page()
            raw, ctype = page.encode("utf-8"), "text/html"
        elif self.path.startswith("/bili/"):
            name = os.path.basename(self.path)
            path = os.path.join(BILIFX, name)
            if os.path.exists(path):
                with open(path, "rb") as fh:
                    raw = fh.read()
                ctype = (".mp4" if "init" in name else "video/iso.segment")
        if raw is None:
            raw, ctype = b"not found\n", "text/plain"
            self.send_response(404)
        else:
            self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except OSError:
            pass

    def log_message(self, *_a):
        pass


def manifest():
    """The segment list the fixtures directory holds, discovered rather than
    hand-copied: init-video.mp4 + init-audio.mp4 + numbered .m4s pairs. A
    hand-typed list is one more place a re-capture can silently desync from."""
    v = ["/bili/init-video.mp4"]
    a = ["/bili/init-audio.mp4"]
    n = 1
    while os.path.exists(os.path.join(BILIFX, "video-%d.m4s" % n)):
        v.append("/bili/video-%d.m4s" % n)
        n += 1
    m = 1
    while os.path.exists(os.path.join(BILIFX, "audio-%d.m4s" % m)):
        a.append("/bili/audio-%d.m4s" % m)
        m += 1
    # Negative control: retain the valid video init segment and the complete
    # audio stream, but withhold every video media segment. A gate wired only
    # to page paint, fetch or audio will still look healthy; the decoder's
    # framesDecoded counter must be the check that makes this run red.
    if CONTROL == "no-video":
        v = v[:1]
    return v, a


def offline_page():
    vq, aq = manifest()
    vtype = _type_line("type-video.txt", 'video/mp4; codecs="avc1.64001E"')
    atype = _type_line("type-audio.txt", 'audio/mp4; codecs="mp4a.40.2"')
    segs = "var VQ=%s, AQ=%s;" % (repr(vq), repr(aq))
    return ("""<!doctype html>
<html><head><title>bili</title><style>
html, body { background:#101014; margin:0; color:#e8e8ee; }
video { display:block; width:640px; height:360px; background:#000; }
</style></head><body>
<video id="v" autoplay></video>
<script>
""" + segs + """
var v = document.getElementById('v');

function stats(tag) {
  var st = (typeof v.__mediaStats === 'function') ? v.__mediaStats() : null;
  if (!st) { console.log(tag + ' NOSTATS'); return null; }
  console.log(tag + ' shown=' + st.framesShown + ' decoded=' + st.framesDecoded +
              ' audio=' + st.audioFrames + ' t=' + v.currentTime.toFixed(2) +
              ' rs=' + v.readyState + ' err=' + (v.error ? v.error.code : 0));
  return st;
}

/* The DASH shape, exactly as a player library drives it: one MediaSource, two
   SourceBuffers, init segment first, media segments appended as the previous
   append reports updateend. Nothing here knows what site the bytes came from. */
var ms = new MediaSource();
v.src = URL.createObjectURL(ms);
console.log('BILI-OFFLINE isTypeSupported avc1=' +
    MediaSource.isTypeSupported('""" + vtype + """') +
    ' aac=' + MediaSource.isTypeSupported('""" + atype + """'));
ms.addEventListener('sourceopen', function () {
  var vb, ab;
  try {
    vb = ms.addSourceBuffer('""" + vtype + """');
    ab = ms.addSourceBuffer('""" + atype + """');
  } catch (e) { console.log('BILI-FAIL addSourceBuffer ' + e); return; }
  function feed(sb, q, name, whenDone) {
    var k = 0;
    function step() {
      if (k >= q.length) { console.log('BILI-FED ' + name + ' ' + k); whenDone(); return; }
      var url = q[k++];
      fetch(url).then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status + ' ' + url);
        return r.arrayBuffer();
      }).then(function (b) {
        sb.addEventListener('updateend', function once() {
          sb.removeEventListener('updateend', once);
          step();
        });
        sb.appendBuffer(b);
      }).catch(function (e) { console.log('BILI-FAIL fetch ' + e); });
    }
    step();
  }
  var left = 2;
  function oneDone() {
    if (--left) return;
    try { ms.endOfStream(); } catch (e) { console.log('BILI-eos ' + e); }
    /* Real player libraries call play() after attaching/buffering. `autoplay`
       is still present so attribute handling remains observable, but relying
       on it alone made this vehicle stop before the media engine: every byte
       appended successfully while currentTime remained exactly zero. */
    /* The acceptance gate captures HDA PCM outside the guest. Keep this
       explicitly audible so a muted element cannot make a healthy decoder
       indistinguishable from an audio pipeline that produced silence. */
    v.muted = false;
    v.volume = 1;
    v.play();
    console.log('BILI-APPENDED buffered=' +
        (v.buffered.length ? v.buffered.end(0).toFixed(2) : 'none') +
        ' duration=' + v.duration);
    var n = 0;
    var id = setInterval(function () {
      n++;
      var st = stats('BILI-TICK' + n);
      if (!st || v.ended || (v.error && v.error.code) || n >= 120) {
        clearInterval(id);
        console.log('BILI-DONE reason=' + (v.ended ? 'ended' :
                     (v.error ? 'error' + v.error.code : 'timeout')));
        stats('BILI-FINAL');
        console.log('BILIPLAY-OFFLINE-DONE');
      }
    }, 500);
  }
  feed(vb, VQ, 'video', oneDone);
  feed(ab, AQ, 'audio', oneDone);
});
</script></body></html>
""")


def probe_page():
    """Run Bilibili's current production player core with the specimen's real
    identity, but let the bootstrap exception reach the serial console.

    The live page deliberately wraps createPlayer()+connect() in an empty
    catch.  That is reasonable production behaviour but makes a missing Web
    Platform primitive indistinguishable from a player that chose to stay
    idle.  This page is diagnostic only: it cannot satisfy the live gate and
    it uses the exact production core URL rather than a forked player.
    """
    return r'''<!doctype html><html><head><title>bili-core-probe</title></head>
<body><div id="bilibili-player" style="width:670px;height:412px"></div>
<div class="danmaku-wrap"></div>
<script src="https://s1.hdslb.com/bfs/static/player/main/core.9b2f4c3c.js"></script>
<script>
console.log('BILI-PROBE nano=' + typeof window.nano);
function h2probe(label, url) {
  fetch(url)
    .then(function (r) { console.log('BILI-PROBE-H2-' + label + ' status=' + r.status); return r.text(); })
    .then(function (t) { console.log('BILI-PROBE-H2-' + label + ' bytes=' + t.length); })
    .catch(function (e) { console.log('BILI-PROBE-H2-' + label + '-FAIL ' + e); });
}
// Three requests started in the same turn reproduce the live player's first
// API burst and, critically, share one negotiated HTTP/2 connection.
h2probe('A', 'https://api.bilibili.com/x/player/online/total?aid=80433022&cid=137649199&bvid=BV1GJ411x7h7');
h2probe('B', 'https://api.bilibili.com/x/player/online/total?aid=80433022&cid=137649199&bvid=BV1GJ411x7h7&probe=2');
h2probe('C', 'https://api.bilibili.com/x/player/online/total?aid=80433022&cid=137649199&bvid=BV1GJ411x7h7&probe=3');
try {
  var setting = {
    element: document.getElementById('bilibili-player'),
    auxiliary: document.querySelector('.danmaku-wrap'),
    aid: 80433022, cid: 137649199, bvid: 'BV1GJ411x7h7', p: 1, t: 0,
    fromDid: null, kind: nano.GroupKind.Ugc,
    featureList: new Set(['blackGap']),
    stats: {spmId:'333.788.0.0',spmIdFrom:'333.788.0.0',trackId:''},
    enableHEVC: true, enableAV1: true, revision: 1
  };
  window.player = nano.createPlayer(setting, {});
  console.log('BILI-PROBE created=' + typeof window.player);
  window.player.connect();
  console.log('BILI-PROBE connected');
} catch (e) {
  console.log('BILI-PROBE-THREW ' + e + '\n' + (e && e.stack ? e.stack : ''));
}
setTimeout(function () { console.log('BILI-PROBE-TIMER player=' + typeof window.player); }, 5000);
</script></body></html>'''


def mirror_page():
    """Serve a captured real page with only its swallowed bootstrap exception
    made observable.  The capture is supplied explicitly so it never becomes
    test evidence and no stale snapshot can be mistaken for the live gate.
    """
    path = os.environ.get("BILIPLAY_MIRROR_HTML", "")
    if not path:
        return "<!doctype html><script>console.log('BILI-MIRROR-NO-HTML')</script>"
    with open(path, "r", encoding="utf-8") as fh:
        page = fh.read()
    old = """          try {
            connectPlayer()
          } catch(e) {}"""
    new = """          console.log('BILI-MIRROR-CONNECT-ENTER')
          try {
            connectPlayer()
            console.log('BILI-MIRROR-CONNECT-RETURN')
          } catch(e) {
            console.log('BILI-MIRROR-CONNECT-THREW ' + e + '\\n' +
              (e && e.stack ? e.stack : ''))
          }"""
    if old not in page:
        return page.replace(
            "<head>",
            "<head><script>console.log('BILI-MIRROR-PATCH-MISS')</script>",
            1)
    page = page.replace(old, new, 1)
    # The captured document is served from the host fixture but its resources
    # and relative URL resolution must remain those of the real specimen.
    page = page.replace("<head>", '<head><base href="%s">' % SPECIMEN, 1)
    return page


# The MIME types the manifest family declares for these very segments,
# recorded at capture time in type-video.txt / type-audio.txt. Reading them
# back keeps the fixture honest: the offline gate asks the browser the SAME
# codec question the live player asked.
def _type_line(name, fallback):
    p = os.path.join(BILIFX, name)
    if os.path.exists(p):
        with open(p, "r") as fh:
            s = fh.read().strip()
        if s:
            return s
    return fallback


srv = None
PORT = 0
if MODE in ("offline", "probe", "mirror"):
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
    PORT = srv.server_port
    threading.Thread(target=srv.serve_forever, daemon=True).start()

PAGE_URL = ("http://10.0.2.2:%d/page.html" % PORT) if MODE in (
    "offline", "probe", "mirror") else SPECIMEN

# A REAL SOUND CARD, wav backend rather than none: the master clock is the
# card's play cursor and the wav file is the only channel that distinguishes
# audio from silence from OUTSIDE the guest (same argument as qmp_video_page).
proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % DISK,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "1024M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-audiodev", "wav,id=snd0,path=%s,out.frequency=48000,out.channels=2,"
                  "out.format=s16" % wav_path,
     "-device", "intel-hda", "-device", "hda-output,audiodev=snd0",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
     "-display", "none", "-no-reboot",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", "file:" + serial_path,
     "-qmp", "unix:%s,server,nowait" % qmp_path],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


_seen_serial = 0


def serial_new():
    """The tail of the serial log not yet printed. Measurement mode streams
    this so the whole chain -- scripts, API calls, URL fetches, the stop -- is
    in the run log as it happens."""
    global _seen_serial
    s = serial()
    if len(s) <= _seen_serial:
        return ""
    out = s[_seen_serial:]
    _seen_serial = len(s)
    return out


def wait_serial(needle, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            print("FAIL: QEMU exited while waiting for " + what)
            return False
        time.sleep(0.25)
    return False


print("== qmp_biliplay_page mode=%s control=%s url=%s ==" %
      (MODE, CONTROL or "none", PAGE_URL))
if not wait_serial("LOGIT_BOOT_OK", 300, "boot"):
    print("FAIL: kernel never printed LOGIT_BOOT_OK")
    print(serial()[-4000:])
    proc.kill()
    sys.exit(1)
if not wait_serial("desktop live", 120, "desktop"):
    print("FAIL: the window manager never brought the desktop up")
    print(serial()[-4000:])
    proc.kill()
    sys.exit(1)
time.sleep(3)

ui = Session(qmp_path, serial=serial_path)
try:
    # The boot path is busy enough that a burst of PS/2 relative packets can
    # be dropped even at 1280x800.  A dead-reckoned Dock click then looks like
    # a slow Browser launch.  Make the guest's own pointer report the
    # precondition, just as the later live-player click already does.
    ui.launch_app("browser", probe=os.path.join(tmp, "browser-launch-pointer.ppm"))
except AssertionError as e:
    print("FAIL: %s" % e)
    proc.kill()
    sys.exit(1)
time.sleep(8)

bar = qmp_addrbar.focus(ui)
for _ in range(70):
    ui.key("backspace", settle=0.02)
ui.typ(PAGE_URL)
qmp_addrbar.typed_echo(ui, bar)
ui.key("ret")

# ---- watch the whole run ---------------------------------------------------
# Measurement: stream the serial tail and take periodic screendumps. The chain
# evidence (which player scripts ran, which URLs were fetched, what the page
# asked and what we answered) is the point of this pass; the verdicts come
# after, from the counters and the wav.
DONE_MARKER = "BILIPLAY-OFFLINE-DONE" if MODE == "offline" else None
end = time.time() + SECS
shots = 0
live_clicked = False
live_click_error = None
live_mounted_clicked = False
live_keyed = False
while time.time() < end and proc.poll() is None:
    time.sleep(5)
    tail = serial_new()
    if tail:
        sys.stdout.write(tail)
        sys.stdout.flush()
    shots += 1
    shot_path = os.path.join(tmp, "shot-%02d.ppm" % shots)
    ui.screendump(shot_path, settle=0.1)
    if (MODE == "live" and not live_clicked and
            "Never Gonna Give You Up" in serial() and
            "[load-complete]" in serial()):
        # Bilibili intentionally leaves an unmuted player behind its poster
        # until it sees a real user gesture.  Drive the same click a person
        # would: through QEMU's PS/2 path, confirmed by the guest's own pointer
        # report, on the visible play triangle at the bottom-left of the
        # 670x412 player at 1280x800.  A centre click is not equivalent here:
        # the current production skin leaves the black poster area inert while
        # paused, so that gesture reached the page but never called play(). The
        # title also exists in Bilibili's early skeleton paint, so wait for the
        # browser's lifecycle-complete marker: a click before player scripts
        # mount is a gesture delivered to the wrong DOM, not a playback test.
        # Merely calling HTMLMediaElement.play() from injected JS would bypass
        # the site player and would not prove that Bilibili can play here.
        try:
            ui.click_at_confirmed(
                os.path.join(tmp, "live-click-pointer.ppm"), 202, 711)
            live_clicked = True
            print("BILIPLAY-LIVE-CLICK point=202,711 confirmed=1")
        except AssertionError as e:
            live_click_error = str(e)
            live_clicked = True
            print("BILIPLAY-LIVE-CLICK-FAIL " + live_click_error)
    # The lifecycle marker can precede the asynchronously mounted production
    # player under TCG. Once the real player reaches its first SourceBuffer
    # request, repeat the same control click once. This is still physical QMP
    # input; no player state, quality preference, API response or media byte is
    # changed by the harness.
    sourcebuffer_attempt = 'addSourceBuffer("audio/mp4;codecs="mp4a.40.' in serial()
    if (MODE == "live" and live_clicked and sourcebuffer_attempt and
            not live_mounted_clicked):
        try:
            if TRACE_INPUT:
                # about:input is an on-demand browser diagnostic that arms 64
                # native hit/focus/key trace points while deliberately
                # preserving the current document and JS realm. Drive it
                # through the address bar so this debug mode does not inject
                # script into, or otherwise impersonate, the public site.
                # The caret-derived locator can only discover a fresh Browser
                # that is already in address editing mode.  This is a loaded
                # page, so use the browser's real Ctrl+L shortcut: it both
                # focuses the bar and selects the complete current URL.  That
                # keeps the diagnostic physical without relying on a caret
                # which intentionally is not on screen before the chord.
                ui.key_mods(("ctrl",), "l", settle=0.2)
                ui.typ("about:input")
                ui.key("ret", settle=0.5)
                print("BILIPLAY-LIVE-INPUT-TRACE armed=1")
            ui.click_at_confirmed(
                os.path.join(tmp, "live-mounted-click-pointer.ppm"), 202, 711)
            live_mounted_clicked = True
            print("BILIPLAY-LIVE-MOUNTED-CLICK point=202,711 confirmed=1")
            # The player has focus now. Space is Bilibili's documented/user
            # playback gesture and crosses QEMU's PS/2 path like a real key;
            # it does not call the media shim or mutate player state from the
            # harness. Keep the click as the focus precondition and send the
            # key only after SourceBuffers prove that the production player,
            # rather than the early page skeleton, is the event consumer.
            ui.key("spc", settle=0.5)
            live_keyed = True
            print("BILIPLAY-LIVE-PLAY-KEY key=space confirmed=1")
        except AssertionError as e:
            live_click_error = str(e)
            live_mounted_clicked = True
            print("BILIPLAY-LIVE-MOUNTED-CLICK-FAIL " + live_click_error)
    # Do not burn the remainder of a long public-site allowance after the
    # substantive gate is already settled. Two rising engine samples prove
    # motion and a positive audio counter proves that decoded PCM reached the
    # guest sound path; QEMU is then closed normally so the host WAV supplies
    # the independent audio assertion below.
    if MODE == "live" and live_keyed:
        stats_lines = [l for l in serial().splitlines()
                       if "[media] stats" in l and "shown=" in l]
        shown_now = [int(re.search(r"shown=(\d+)", l).group(1))
                     for l in stats_lines if re.search(r"shown=(\d+)", l)]
        audio_now = [int(re.search(r"audio=(\d+)", l).group(1))
                     for l in stats_lines if re.search(r"audio=(\d+)", l)]
        if (len(shown_now) >= 2 and
                any(b > a for a, b in zip(shown_now, shown_now[1:])) and
                any(n > 0 for n in audio_now)):
            print("BILIPLAY-LIVE-EVIDENCE-COMPLETE shown=%s audio=%s" %
                  (shown_now[-4:], audio_now[-4:]))
            break
    if DONE_MARKER and DONE_MARKER in serial():
        break

time.sleep(2)
# Close the browser before stopping the machine.  Besides matching a user
# leaving the page, this runs the normal page/transport teardown and prints the
# HTTP/2 session counters.  Killing QEMU first used to hide whether a stalled
# fetch had sent no frames, received no frames, or hit a protocol error.
if proc.poll() is None:
    try:
        ui.key_mods(("meta_l",), "w", settle=0.5)
        time.sleep(3)
        tail = serial_new()
        if tail:
            sys.stdout.write(tail)
            sys.stdout.flush()
    except Exception as e:
        print("BILIPLAY-CLOSE-DIAG-FAIL " + str(e))
proc.terminate()
try:
    proc.wait(timeout=20)
except Exception:
    proc.kill()

# ---- the PCM channel -------------------------------------------------------
def pcm_energy(path):
    if not os.path.exists(path) or os.path.getsize(path) <= 44:
        return 0, 0, 0, None
    with open(path, "rb") as fh:
        raw = fh.read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        return 0, 0, 0, None
    import struct
    body = raw[44:]
    cnt = len(body) // 2
    vals = struct.unpack("<%dh" % cnt, body[:cnt * 2])
    peak, loud, first, last = 0, 0, None, None
    for i, v in enumerate(vals):
        a = v if v >= 0 else -v
        if a > peak:
            peak = a
        if a > 64:
            loud += 1
            if first is None:
                first = i
            last = i
    span = None
    if first is not None:
        span = (first / 2.0 / 48000.0, last / 2.0 / 48000.0)
    return cnt // 2, peak, loud, span


frames, peak, loud, span = pcm_energy(wav_path)
print("\n== audio, as QEMU's card wrote it ==")
print("guest.wav: %d frames (%.2f s), peak %d, %d samples above the floor%s"
      % (frames, frames / 48000.0, peak, loud,
         (" -- audible %.2fs..%.2fs" % span) if span is not None else ""))
print("\nartefacts: %s" % tmp)

# ---- verdicts --------------------------------------------------------------
# In the house counting rules: FRAMES SHOWN (from the engine's own counters,
# on serial, a channel the page cannot write to) and captured PCM (from the
# wav above). "PAINTED" is not "playing"; one frame and a stall is not
# playback. Both channels must move, or the verdict is no.
checks = []


def ck(cond, name):
    checks.append((bool(cond), name))
    print(("ok: " if cond else "FAIL: ") + name)


def fields(marker):
    out = None
    for ln in serial().splitlines():
        i = ln.find(marker)
        if i < 0:
            continue
        d = {}
        for tok in ln[i + len(marker):].split():
            if "=" in tok:
                k, _, val = tok.partition("=")
                d[k] = val
        out = d
    return out


if MODE == "offline":
    final = fields("BILI-FINAL")
    ticks = []
    for ln in serial().splitlines():
        if "BILI-TICK" in ln:
            d = {}
            for tok in ln.split():
                if "=" in tok:
                    k, _, val = tok.partition("=")
                    d[k] = val
            ticks.append(d)
    ck(final is not None, "the offline page reported a final stat line")
    if final:
        shown = int(final.get("shown", "0"))
        decoded = int(final.get("decoded", "0"))
        ck(int(final.get("err", "0")) == 0,
           "no MediaError (err=%s)" % final.get("err"))
        ck(decoded > 0, "the specimen's H.264 decoded (%d pictures)" % decoded)
        ck(shown > 0, "the FIRST FRAME was SHOWN (%d)" % shown)
        seen = [int(d.get("shown", "0")) for d in ticks]
        ck(any(b > a for a, b in zip(seen, seen[1:])),
           "framesShown ROSE between samples -- playback, not one frame and a "
           "stall (over time: %s)" % seen)
        ck(final.get("reason", "") == "ended" or "BILI-DONE reason=ended" in serial(),
           "reached the end of the appended media ('ended')")
    ck(frames > 0, "the sound card received a stream (%d frames)" % frames)
    ck(loud > 1000, "and it was AUDIO, not silence (%d samples above the floor, "
                    "peak %d)" % (loud, peak))
    ok = all(c for c, _ in checks)
    print("\nqmp_biliplay offline: %d checks, %d failures" %
          (len(checks), len(checks) - sum(1 for c, _ in checks if c)))
    print("BILIPLAY-OFFLINE-" + ("OK" if ok else "FAIL"))
    sys.exit(0 if ok else 1)

# Live mode uses the same substantive assertions as offline (shown>0, rising,
# and PCM); reaching `ended` is optional because the public video is long.
media_lines = [l for l in serial().splitlines() if "[media]" in l]
shown_samples = [int(l.split("shown=")[1].split()[0]) for l in media_lines
                 if "shown=" in l]
ck(len(media_lines) > 0,
   "the page's player stack asked this browser a media question (%d [media] "
   "lines)" % len(media_lines))
for l in media_lines[:6]:
    print("     " + l.strip())
ck(live_clicked and live_click_error is None,
   "a confirmed user click reached the LIVE player")
ck(any(n > 0 for n in shown_samples),
   "the FIRST FRAME was shown on the LIVE specimen (engine counters)")
ck(any(b > a for a, b in zip(shown_samples, shown_samples[1:])),
   "framesShown ROSE on the LIVE specimen -- playback, not one frame and a "
   "stall (over time: %s)" % shown_samples)
ck(frames > 0 and loud > 1000, "audio PCM was captured on the LIVE specimen")
ok = all(c for c, _ in checks)
print("\nqmp_biliplay live: %d checks, %d failures" %
      (len(checks), len(checks) - sum(1 for c, _ in checks if c)))
print("BILIPLAY-LIVE-" + ("OK" if ok else "FAIL"))
sys.exit(0 if ok else 1)
