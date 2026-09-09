#!/usr/bin/env python3
"""Play a video in the REAL browser, on the machine, and count FRAMES SHOWN.

    python3 tests/qmp/qmp_video_page.py <iso> <disk.img> [--layer 1|2|both]

WHAT THIS IS FOR, and why none of the gates that already exist answer it.

  make test-mse       drives the ENGINE (js_media_src.c) on the host, against a
                      stepped clock and a recording blitter. It settles the
                      arithmetic and nothing about the browser: no QuickJS, no
                      DOM, no fetch, no layout, no compositor, no sound card.
  make test-mse-os    runs /bin/msecheck on the machine. Real clock, real card,
                      real mini-libc -- and STILL not the browser: msecheck
                      calls the engine directly and there is no page, no
                      <video> element, no JavaScript and no painter.

So both of them can be green while `<video>` in a page does nothing, which is
exactly the state this line found the tree in. This harness closes that: a page
loaded over HTTP, in browser.aex, drives an ordinary <video> element and the
numbers come back off the element's own counters.

THE MEASUREMENT IS FRAMES SHOWN, NOT PIXELS. A screenshot cannot tell a stall
from a black frame, and it cannot tell a video that played from one that showed
its first picture and froze -- which is the failure this whole line is about.
`video.__mediaStats()` reports framesDecoded / framesShown / framesDropped from
the same counters the host gate asserts on, so the two are commensurable. The
harness additionally requires:

  - framesShown to be RISING at two separate times, seconds apart. One frame and
    a stall satisfies "> 0" and is not playback.
  - the element to reach `ended`, which is only reachable if the master clock
    got all the way to the last picture's presentation time.
  - the WAV file QEMU's audio backend writes to contain real PCM, not silence.
    hda.c's own header is the reason: the DMA engine runs happily into silence,
    every register reads back correct, and that failure looks exactly like
    success from the controller's side. Bytes on the host's disk do not.

TWO LAYERS, REPORTED SEPARATELY, because they fail for different reasons and
"video is broken" is not a work order:

THIS GATE HAS BEEN WATCHED GOING RED, twice, for two different real defects,
which is the evidence a green gate otherwise cannot offer:

  1. `decoded=0 shown=0 audio=0, readyState=1, size=64x48, err=none` -- every
     stage worked (fetch, loader, demuxer, metadata) and the element was simply
     PAUSED, because `v.src = url; v.play();` is synchronous and the loader's
     teardown ran afterwards and reset it. Nothing else in the tree could see
     this: the host gate calls the loader and play() in the other order.
  2. `decoded=30 shown=29 ended=0`, drift -14 ms, readyState 4, no error, held
     there for the full 30 s of sampling -- the sound card's play cursor parks
     ~104 ms short of what it was given once nothing more is written, so the
     last picture was AV_WAIT for ever. A one-frame stall out of a clock
     policy, and invisible to any check that asks "did it play".

Both were found by this harness and both are fixed; the log of either failure
is what "framesShown ROSE" and "'ended' fired" are here to keep out.

  layer 1  <video src="movie.mp4">  -- one whole progressive file. This is the
           floor. It exercises fetch, the loader, the demuxer's content sniff,
           the decoders, the clock, the card and the painter.
  layer 2  MediaSource / SourceBuffer / appendBuffer, init segment then media
           segments in order -- the shape every DASH site uses, bilibili
           included. Same engine, driven the way a player library drives it.
"""

import os
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
LAYER = "both"
for i, a in enumerate(sys.argv[3:]):
    if a == "--layer" and i + 4 <= len(sys.argv) - 1 + 1:
        LAYER = sys.argv[3:][i + 1]

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MEDIA = os.path.join(ROOT, "tests", "fixtures", "media")
MSEFX = os.path.join(ROOT, "tests", "fixtures", "mse")

# The progressive file. h264-mp3.mp4 rather than one of the fMP4 fixtures on
# purpose: a plain moov-at-the-front MP4 with an MP3 track is what the
# non-DASH half of the web actually serves, and its 1152-sample audio frames
# are what caught the staging-buffer truncation in the engine.
PROGRESSIVE = os.path.join(MEDIA, "h264-mp3.mp4")

VIDEO_SEGS = 4
AUDIO_SEGS = 5

PAGE = """<!doctype html>
<html><head><title>video</title><style>
html, body { background: #101014; margin: 0; padding: 0; color: #e8e8ee; }
video { display: block; width: 512px; height: 384px; background: #000000; }
</style></head><body>
<video id="v"></video>
<div id="s">idle</div>
<script>
var v = document.getElementById('v');
var LAYER = %LAYER%;

function stats(tag) {
  var st = (typeof v.__mediaStats === 'function') ? v.__mediaStats() : null;
  if (!st) { console.log(tag + ' NOSTATS'); return null; }
  console.log(tag + ' decoded=' + st.framesDecoded + ' shown=' + st.framesShown +
              ' dropped=' + st.framesDropped + ' audio=' + st.audioFrames +
              ' drift=' + Math.round(st.driftMeanMs) + '/' + Math.round(st.driftMaxMs) +
              ' t=' + v.currentTime.toFixed(3) + ' rs=' + v.readyState +
              ' ended=' + (v.ended ? 1 : 0) + ' err=' + (v.error ? v.error.code : 0));
  return st;
}

/* Sampled twice, seconds apart, and both samples are printed. "> 0 frames" is
   satisfied by one picture and a freeze; a RISE between two samples is not. */
function watch(tag, done) {
  var n = 0;
  var id = setInterval(function () {
    n++;
    var st = stats(tag + '-TICK' + n);
    if (!st) { clearInterval(id); done(null); return; }
    if (v.ended || (v.error && v.error.code) || n >= 60) {
      clearInterval(id);
      console.log(tag + '-END reason=' +
                  (v.ended ? 'ended' : (v.error ? 'error' + v.error.code : 'timeout')));
      done(st);
    }
  }, 500);
}

function report(tag, st) {
  if (!st) { console.log(tag + '-FAIL no stats'); return; }
  console.log(tag + '-RESULT decoded=' + st.framesDecoded + ' shown=' + st.framesShown +
              ' dropped=' + st.framesDropped + ' audio=' + st.audioFrames +
              ' ended=' + (v.ended ? 1 : 0) +
              ' err=' + (v.error ? v.error.code + ':' + v.error.message : 'none') +
              ' size=' + v.videoWidth + 'x' + v.videoHeight);
}

/* ---------------- layer 1: a plain progressive file ------------------- */
function layer1(next) {
  console.log('L1-START canPlay=' + v.canPlayType('video/mp4; codecs="avc1.42E01E"'));
  v.onerror = function () {
    console.log('L1-ERROR code=' + (v.error ? v.error.code : '?') +
                ' msg=' + (v.error ? v.error.message : '?'));
  };
  v.muted = false;
  v.src = '/movie.mp4';
  v.play();
  watch('L1', function (st) { report('L1', st); next(); });
}

/* ---------------- layer 2: MSE, driven by hand ------------------------ */
function layer2() {
  if (typeof MediaSource !== 'function') { console.log('L2-FAIL no MediaSource'); done2(); return; }
  console.log('L2-START isTypeSupported=' +
              MediaSource.isTypeSupported('video/mp4; codecs="avc1.640033"') + '/' +
              MediaSource.isTypeSupported('audio/mp4; codecs="mp4a.40.2"'));
  var v2 = document.createElement('video');
  document.body.appendChild(v2);
  v = v2;
  var ms = new MediaSource();
  v.src = URL.createObjectURL(ms);
  ms.addEventListener('sourceopen', function () {
    var vb, ab;
    try {
      vb = ms.addSourceBuffer('video/mp4; codecs="avc1.640033"');
      ab = ms.addSourceBuffer('audio/mp4; codecs="mp4a.40.2"');
    } catch (e) { console.log('L2-FAIL addSourceBuffer ' + e); return; }

    /* The DASH shape: init segment, then media segments in order, each fetched
       and appended when the previous append has finished. Nothing here knows
       what site it came from -- this is the whole of what a player library
       does with the bytes. */
    var vq = ['/mse/init-video.mp4'], aq = ['/mse/init-audio.mp4'];
    for (var i = 1; i <= %NV%; i++) vq.push('/mse/video-' + i + '.m4s');
    for (var j = 1; j <= %NA%; j++) aq.push('/mse/audio-' + j + '.m4s');

    function feed(sb, q, name, whenDone) {
      var k = 0;
      function step() {
        if (k >= q.length) { console.log('L2-FED ' + name + ' ' + k); whenDone(); return; }
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
        }).catch(function (e) { console.log('L2-FAIL fetch ' + e); });
      }
      step();
    }

    var left = 2;
    function oneDone() {
      if (--left) return;
      try { ms.endOfStream(); } catch (e) { console.log('L2-eos ' + e); }
      console.log('L2-APPENDED buffered=' +
                  (v.buffered.length ? v.buffered.end(0).toFixed(3) : 'none'));
      v.play();
      watch('L2', function (st) { report('L2', st); done2(); });
    }
    feed(vb, vq, 'video', oneDone);
    feed(ab, aq, 'audio', oneDone);
  });
}

function done2() { console.log('VIDEO-DONE'); }

if (LAYER === 1) { layer1(function () { console.log('VIDEO-DONE'); }); }
else if (LAYER === 2) { layer2(); }
else { layer1(function () { layer2(); }); }
</script>
</body></html>
"""


def page_bytes():
    p = PAGE.replace("%LAYER%", {"1": "1", "2": "2"}.get(LAYER, "0"))
    p = p.replace("%NV%", str(VIDEO_SEGS)).replace("%NA%", str(AUDIO_SEGS))
    return p.encode()


tmp = tempfile.mkdtemp(prefix="qmp_video_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")
wav_path = os.path.join(tmp, "guest.wav")

served = []


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        served.append(self.path)
        raw, ctype = None, "application/octet-stream"
        if self.path.startswith("/page"):
            raw, ctype = page_bytes(), "text/html"
        elif self.path.startswith("/movie.mp4"):
            with open(PROGRESSIVE, "rb") as fh:
                raw = fh.read()
            ctype = "video/mp4"
        elif self.path.startswith("/mse/"):
            name = os.path.basename(self.path)
            path = os.path.join(MSEFX, name)
            if os.path.exists(path):
                with open(path, "rb") as fh:
                    raw = fh.read()
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


srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
PORT = srv.server_port
threading.Thread(target=srv.serve_forever, daemon=True).start()

# A REAL SOUND CARD, and the `wav` backend rather than `none`. The engine's
# master clock is the card's play cursor: with no card it falls back to the
# monotonic clock and this harness would be measuring something easier than
# what a user gets. `wav` is timer-driven, consumes at the real rate, and
# leaves a file of what the guest actually played -- which is the only way to
# tell audio from silence from outside the guest.
proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % DISK,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-audiodev", "wav,id=snd0,path=%s,out.frequency=48000,out.channels=2,"
                  "out.format=s16" % wav_path,
     "-device", "intel-hda", "-device", "hda-output,audiodev=snd0",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
     "-display", "none", "-no-reboot",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", "file:" + serial_path,
     "-qmp", "unix:%s,server,nowait" % qmp_path],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

checks = []


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def die(msg):
    print("FAIL: " + msg)
    print("----- artefacts in %s -----" % tmp)
    print("----- serial (tail) -----")
    print(serial()[-9000:])
    print("-------------------------")
    proc.kill()
    sys.exit(1)


def ck(cond, name):
    checks.append((bool(cond), name))
    print(("ok: " if cond else "FAIL: ") + name)
    if not cond:
        die(name)


def note(s):
    print("     " + s)


def wait_serial(needle, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for " + what)
        time.sleep(0.25)
    return False


def fields(marker):
    """The k=v pairs on the last serial line containing `marker`."""
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


def ticks(tag):
    """Every TICK line for a layer, in order, as dicts."""
    out = []
    for ln in serial().splitlines():
        i = ln.find(tag + "-TICK")
        if i < 0:
            continue
        d = {}
        for tok in ln[i:].split()[1:]:
            if "=" in tok:
                k, _, val = tok.partition("=")
                d[k] = val
        out.append(d)
    return out


def pcm_energy(path):
    """Non-silent samples in the WAV QEMU wrote, its peak, and when it was loud.

    A boot log with no errors is not evidence that audio played. This is the
    only channel in the harness the guest cannot write to.

    IT DOES NOT USE THE `wave` MODULE, AND THAT IS THE WHOLE POINT OF THIS
    COMMENT. QEMU's wav backend streams samples as it goes and patches the RIFF
    and `data` length fields IN THE HEADER when the device is closed at
    shutdown. This harness terminates QEMU, so those fields are still zero --
    `wave.open(...).getnframes()` returns 0 over a file with 1.77 MB of real
    PCM in it, and the first version of this function duly reported "0 frames,
    peak 0" and failed the run. That is rule 1 exactly: the measurement was
    fine and the instrument read the wrong field. Everything after byte 44 is
    s16le stereo whether or not anyone wrote down how much of it there is."""
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


def layer_report(tag):
    """Print and judge one layer. Returns True when it played."""
    res = fields(tag + "-RESULT")
    tk = ticks(tag)
    if res is None:
        note("%s: no RESULT line -- the layer never finished" % tag)
        for d in tk[-3:]:
            note("%s tick: %s" % (tag, d))
        return False
    note("%s: %s" % (tag, " ".join("%s=%s" % kv for kv in sorted(res.items()))))
    shown = int(res.get("shown", "0"))
    decoded = int(res.get("decoded", "0"))
    ck(decoded > 0, "%s: the decoder produced pictures (%d)" % (tag, decoded))
    ck(shown > 0, "%s: pictures reached the screen (%d shown)" % (tag, shown))
    # A RISE, not a total. One frame and a freeze passes "shown > 0".
    seen = [int(d.get("shown", "0")) for d in tk]
    rising = any(b > a for a, b in zip(seen, seen[1:]))
    note("%s: framesShown over time: %s" % (tag, seen))
    ck(rising, "%s: framesShown ROSE between samples -- this is playback, "
               "not one picture and a stall" % tag)
    ck(shown * 10 >= decoded * 9,
       "%s: %d of %d decoded pictures were shown (>=90%%)" % (tag, shown, decoded))
    ck(res.get("ended") == "1",
       "%s: reached the end of the media -- 'ended' fired, so the master clock "
       "got all the way to the last picture" % tag)
    return True


try:
    if not wait_serial("LOGIT_BOOT_OK", 300, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    if not wait_serial("desktop live", 120, "desktop"):
        die("the window manager never brought the desktop up")
    time.sleep(3)

    ui = Session(qmp_path, serial=serial_path)
    # One click, at the tile the GUEST names for browser.aex, verified against the
    # guest's own [wm] launched line. The re-click loop this replaces was the
    # apology a hand-kept slot constant needed: when the pack list grows, the dock
    # is re-centred, every hard-coded index moves half a slot, and the miss looks
    # exactly like a slow launch. See tests/qmp/qmp_ui.py's dock block.
    try:
        ui.launch_app("browser")
    except AssertionError as e:
        die(str(e))
    time.sleep(8)

    # Address bar via the caret (qmp_addrbar): the retired (420, 145) was in
    # the tab strip, and typing only worked because the Browser boots editing.
    bar = qmp_addrbar.focus(ui)
    for _ in range(70):
        ui.key("backspace", settle=0.02)
    ui.typ("http://10.0.2.2:%d/page.html" % PORT)
    qmp_addrbar.typed_echo(ui, bar)
    ui.key("ret")

    ck(wait_serial("L1-START", 90, "the page's script"),
       "the page loaded and its script ran in browser.aex")

    ok1 = ok2 = None
    if LAYER in ("1", "both"):
        wait_serial("L1-RESULT", 240, "layer 1")
        print("\n== layer 1: <video src>, one whole progressive file ==")
        ok1 = layer_report("L1")
    if LAYER in ("2", "both"):
        wait_serial("L2-RESULT", 300, "layer 2")
        print("\n== layer 2: MediaSource + appendBuffer, the DASH shape ==")
        ok2 = layer_report("L2")

    wait_serial("VIDEO-DONE", 60, "the page to finish")

    # The audio channel, read off the host's own disk.
    print("\n== audio, as QEMU's card wrote it ==")
    time.sleep(2)
    proc.terminate()
    try:
        proc.wait(timeout=20)
    except Exception:
        proc.kill()
    frames, peak, loud, span = pcm_energy(wav_path)
    note("guest.wav: %d frames (%.2f s), peak amplitude %d, %d samples above the "
         "noise floor%s"
         % (frames, frames / 48000.0, peak, loud,
            ("", " -- audible %.2fs..%.2fs" % span)[span is not None]))
    ck(frames > 0, "the sound card received a stream at all (%d frames)" % frames)
    ck(loud > 1000,
       "and it was AUDIO, not silence: %d samples above the floor, peak %d "
       "-- the DMA engine runs happily into silence and every register reads "
       "back correct, so this is the only channel that can tell" % (loud, peak))

    print("\nqmp_video_page: %d checks, 0 failures" % len(checks))
    print("VIDEO-PAGE-OK")
except SystemExit:
    raise
finally:
    try:
        proc.kill()
    except Exception:
        pass
