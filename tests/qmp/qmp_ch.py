#!/usr/bin/env python3
"""GATE B: the chat window streams, ON THE MACHINE, and the text grows.

THE ASSERTION THAT MATTERS, and the one everything else is scaffolding for:
two screendumps taken WHILE the stream is running, and the second must contain
strictly more text than the first. A reader that buffered the whole reply and
printed it at the end would produce a perfectly correct final window and would
fail this. "The answer appeared" is not the claim; "the answer arrived in
pieces and each piece was drawn" is.

Everything the guest talks to is a mock served from this file, over QEMU's
SLIRP NAT (the host is 10.0.2.2 from inside the guest). THERE IS NO KEY
ANYWHERE IN THIS TEST and the endpoint needs none -- /etc/ai.conf is written
with an empty `key`, which the app treats as "send no Authorization header",
and the mock ASSERTS that no Authorization header arrived.

The one key-shaped string here is a CANARY, not a credential: the 401 phase
puts `CANARY-NOT-A-KEY-...` in the config and then requires that string to be
absent from every byte of the serial console. A program that leaks its key does
it in a diagnostic, and that is the only place a test can look.

The mock also does the two things that break naive clients, on purpose:
  - chunked transfer-encoding, so there is no Content-Length to lean on;
  - SSE records SPLIT ACROSS TCP WRITES, mid-JSON and mid-`data:` prefix.
The host gate (tests/unit/ch_sse_test.c) proves the parser survives that
synthetically; this proves the whole path does, over a real NIC.

  python3 tests/qmp/qmp_ch.py --iso build/logit.iso --disk build/disk.img
  python3 tests/qmp/qmp_ch.py ... --only refusal     (the no-config path)
"""

import argparse
import json
import os
import re
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

# --------------------------------------------------------------- the reply --
# 24 slow tokens (the window in which the screendumps are taken) then a BURST of
# 200 with no delay at all. The burst is not padding: it is what makes the
# repaint bound measurable. If the window repainted per token it would draw 224
# frames; the app's budget is one frame per 40 ms, so the burst must cost a
# handful. CH_STREAM_END prints both numbers.
SLOW = ("Streaming ", "means ", "the ", "answer ", "arrives ", "in ", "pieces ",
        "and ", "each ", "piece ", "is ", "drawn ", "the ", "moment ", "it ",
        "lands, ", "so ", "you ", "read ", "the ", "reply ", "while ", "it ",
        "is ", "still ", "being ", "written. ")
BURST = tuple("tok%d " % i for i in range(200))
TOKENS = SLOW + BURST
SLOW_DELAY = 0.42

REPLY = "".join(TOKENS)


def fnv1a(s):
    h = 2166136261
    for b in s.encode("utf-8"):
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


CANARY = "CANARY-NOT-A-KEY-0000000000000000"

# ------------------------------------------------------------- the mock ------

seen_lock = threading.Lock()
seen = []                       # (path, headers dict, body str)
serve_state = {"started": 0, "finished": 0}


def sse_record(tok):
    obj = {"choices": [{"index": 0, "delta": {"content": tok}}]}
    return ("data: " + json.dumps(obj, separators=(",", ":")) + "\n\n").encode()


class Handler(socketserver.BaseRequestHandler):
    def _chunk(self, b):
        self.request.sendall(("%x\r\n" % len(b)).encode() + b + b"\r\n")

    def handle(self):
        c = self.request
        c.settimeout(30)
        buf = b""
        try:
            while b"\r\n\r\n" not in buf:
                d = c.recv(65536)
                if not d:
                    return
                buf += d
            head, rest = buf.split(b"\r\n\r\n", 1)
            lines = head.decode("latin1").split("\r\n")
            path = lines[0].split(" ")[1] if len(lines[0].split(" ")) > 1 else "/"
            hdr = {}
            for ln in lines[1:]:
                if ":" in ln:
                    k, v = ln.split(":", 1)
                    hdr[k.strip().lower()] = v.strip()
            n = int(hdr.get("content-length", "0"))
            while len(rest) < n:
                d = c.recv(65536)
                if not d:
                    break
                rest += d
            with seen_lock:
                seen.append((path, hdr, rest[:n].decode("utf-8", "replace")))

            if path.endswith("/401"):
                body = (b'{"error":{"message":"Incorrect API key provided",'
                        b'"type":"invalid_request_error","code":"invalid_api_key"}}')
                c.sendall(b"HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n"
                          b"Content-Length: " + str(len(body)).encode() +
                          b"\r\nConnection: close\r\n\r\n" + body)
                return
            if path.endswith("/429"):
                body = (b'{"error":{"message":"Rate limit reached for requests",'
                        b'"type":"rate_limit_error"}}')
                c.sendall(b"HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\n"
                          b"Retry-After: 7\r\nContent-Length: " + str(len(body)).encode() +
                          b"\r\nConnection: close\r\n\r\n" + body)
                return

            c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                      b"Cache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n"
                      b"Connection: close\r\n\r\n")
            with seen_lock:
                serve_state["started"] += 1

            trunc = path.endswith("/trunc")
            slow_n = 3 if trunc else len(SLOW)
            toks = TOKENS[:3] if trunc else TOKENS

            self._chunk(b": stream open\n\n")          # a keep-alive comment
            for i, t in enumerate(toks):
                rec = sse_record(t)
                # SPLIT ACROSS TCP WRITES. Every third record goes out as two
                # chunks cut mid-JSON; every seventh is cut inside the literal
                # `data: ` prefix itself, which is the split that kills a reader
                # that looks for the prefix in the buffer it was handed.
                if i % 7 == 3:
                    self._chunk(rec[:3]); time.sleep(0.02); self._chunk(rec[3:])
                elif i % 3 == 0:
                    k = len(rec) // 2
                    self._chunk(rec[:k]); time.sleep(0.02); self._chunk(rec[k:])
                else:
                    self._chunk(rec)
                if i % 11 == 5:
                    self._chunk(b": ping\n\n")
                if i < slow_n:
                    time.sleep(SLOW_DELAY)
            if trunc:
                # No [DONE], no terminating chunk: just hang up mid-stream.
                c.close()
                return
            self._chunk(b"data: [DONE]\n\n")
            c.sendall(b"0\r\n\r\n")
            with seen_lock:
                serve_state["finished"] += 1
        except OSError:
            pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


# ------------------------------------------------------------------- pixels --

class PPM:
    def __init__(self, path):
        with open(path, "rb") as fh:
            data = fh.read()
        if not data.startswith(b"P6"):
            raise ValueError(path + ": not a binary PPM")
        fields, i = [], 2
        while len(fields) < 3:
            while i < len(data) and data[i:i + 1].isspace():
                i += 1
            if data[i:i + 1] == b"#":
                while i < len(data) and data[i] != 0x0A:
                    i += 1
                continue
            j = i
            while j < len(data) and not data[j:j + 1].isspace():
                j += 1
            fields.append(int(data[i:j]))
            i = j
        self.w, self.h, _ = fields
        self.px = data[i + 1:]

    def diff_box(self, other, x0, y0, x1, y1, thresh=24):
        """How many pixels in the box differ from `other`, and how far down the
        lowest one is. Counting CHANGE rather than darkness is what makes this
        immune to the wallpaper, the window chrome and the theme."""
        n, low = 0, -1
        row = self.w * 3
        for y in range(y0, min(y1, self.h)):
            base = y * row
            hit = False
            for x in range(x0, min(x1, self.w)):
                o = base + x * 3
                if (abs(self.px[o] - other.px[o]) +
                        abs(self.px[o + 1] - other.px[o + 1]) +
                        abs(self.px[o + 2] - other.px[o + 2])) > thresh:
                    n += 1
                    hit = True
            if hit:
                low = y
        return n, low

    def to_png(self, path):
        raw = b"".join(b"\x00" + self.px[y * self.w * 3:(y + 1) * self.w * 3]
                       for y in range(self.h))

        def chunk(tag, data):
            return (struct.pack(">I", len(data)) + tag + data +
                    struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
        png = (b"\x89PNG\r\n\x1a\n" +
               chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)) +
               chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
        with open(path, "wb") as fh:
            fh.write(png)


# --------------------------------------------------------------------- guest --

class Guest:
    def __init__(self, iso, disk, shots):
        self.tmp = tempfile.mkdtemp(prefix="qmp_ch_")
        self.shots = shots
        self.qmp_path = os.path.join(self.tmp, "qmp.sock")
        self.ser_path = os.path.join(self.tmp, "serial.sock")
        self.log = ""
        qemu = os.environ.get("QEMU", "qemu-system-x86_64")
        self.proc = subprocess.Popen(
            [qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", iso,
             "-drive", f"file={disk},format=raw,if=none,id=hd0,file.locking=off",
             "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
             "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
             "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
             "-display", "none", "-no-reboot",
             "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
             "-serial", f"unix:{self.ser_path},server=on,wait=off",
             "-qmp", f"unix:{self.qmp_path},server,nowait"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.ser = self._connect(self.ser_path)
        # THE SERIAL IS DRAINED BY A THREAD, NOT BY THE TEST'S POLL LOOP, and
        # that is the second freeze this harness had to fix. QEMU's serial
        # chardev applies back-pressure: when the host socket stops accepting
        # bytes, the emulated UART stops reporting transmit-ready and the guest
        # busy-waits inside serial_putc. The machine then looks exactly like a
        # kernel hang -- QMP still answers `query-status: running`, and the
        # serial log simply ends mid-line -- while the real cause is on this
        # side of the socket. An in-line poll loop that also has to sleep, take
        # screendumps and wait on QMP replies WILL eventually stop reading for
        # long enough. A thread that does nothing but recv cannot.
        self.loglock = threading.Lock()
        self.log = ""
        threading.Thread(target=self._ser_reader, daemon=True).start()
        self.qmp = self._connect(self.qmp_path)
        self.qf = self.qmp.makefile("rw")
        # A READER THREAD, and it is not tidiness -- it is the fix for a freeze
        # that cost an hour. QEMU's QMP monitor writes from the main loop, and
        # the main loop is the thread that also runs the VM. A client that
        # issues qmp_capabilities and then goes quiet for thirty seconds while
        # it drives the serial console lets QMP's ASYNC EVENTS fill the socket,
        # and when that buffer stops accepting writes the whole guest stops --
        # deterministically, mid-execve, with no error anywhere and a serial log
        # that simply ends. Draining continuously in a thread makes the
        # condition unreachable; `cmd` then just waits for the reply.
        self.qreplies = []
        self.qlock = threading.Lock()
        self.qdead = False
        threading.Thread(target=self._qmp_reader, daemon=True).start()
        self.qcaps = False
        self.cursor = [640, 400]

    def _qmp_reader(self):
        dbg = os.environ.get("CH_QMPLOG")
        fh = open(dbg, "w") if dbg else None
        try:
            for line in self.qf:
                if fh:
                    fh.write("%.2f %s" % (time.time(), line))
                    fh.flush()
                try:
                    m = json.loads(line)
                except ValueError:
                    continue
                if "return" in m or "error" in m:
                    with self.qlock:
                        self.qreplies.append(m)
        except Exception as e:
            if fh:
                fh.write("reader died: %r\n" % (e,))
        self.qdead = True
        if fh:
            fh.close()

    def _connect(self, path):
        for _ in range(400):
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(path)
                return s
            except OSError:
                if self.proc.poll() is not None:
                    self.die("qemu exited before its sockets came up")
                time.sleep(0.1)
        self.die("could not connect to " + path)

    def die(self, msg):
        print("FAIL: " + msg)
        try:
            print("  qemu alive: %s   qmp query-status: %s"
                  % (self.proc.poll() is None,
                     self.cmd({"execute": "query-status"}, timeout=5)))
        except Exception as e:
            print("  (query-status failed: %r)" % (e,))
        print("----- serial (tail) -----")
        print(self.log[-8000:])
        print("-------------------------")
        self.kill()
        sys.exit(1)

    def kill(self):
        try:
            self.proc.kill()
        except Exception:
            pass

    def _ser_reader(self):
        buf = b""
        while True:
            try:
                b = self.ser.recv(65536)
            except OSError:
                return
            if not b:
                return
            buf += b
            try:
                text = buf.decode("utf-8")
                buf = b""
            except UnicodeDecodeError:
                # A multi-byte sequence straddling a recv. Keep the tail.
                text = buf[:-3].decode("utf-8", "replace")
                buf = buf[-3:]
            with self.loglock:
                self.log += text

    def snap(self):
        with self.loglock:
            return self.log

    def pump(self, seconds):
        time.sleep(seconds)

    def wait_for(self, marker, timeout, since=0):
        end = time.time() + timeout
        while time.time() < end:
            if marker in self.snap()[since:]:
                return True
            if self.proc.poll() is not None:
                time.sleep(0.3)
                return marker in self.snap()[since:]
            time.sleep(0.1)
        return False

    def cmd(self, d, timeout=30):
        with self.qlock:
            self.qreplies = []
        self.qf.write(json.dumps(d) + "\n")
        self.qf.flush()
        end = time.time() + timeout
        while time.time() < end:
            with self.qlock:
                if self.qreplies:
                    return self.qreplies.pop(0)
            if self.qdead:
                return None
            time.sleep(0.01)
        return None

    def caps(self):
        if not self.qcaps:
            self.cmd({"execute": "qmp_capabilities"})
            self.qcaps = True

    def sh(self, line):
        n = len(self.log)
        self.ser.sendall((line + "\n").encode())
        self.pump(0.9)
        if os.environ.get("CH_VERBOSE"):
            print("    $ %s\n%s" % (line, self.log[n:]))

    def screendump(self, name):
        p = os.path.join(self.shots, name + ".ppm")
        self.cmd({"execute": "screendump", "arguments": {"filename": p}})
        time.sleep(0.4)
        return PPM(p)

    # ---- input ----
    def goto(self, tx, ty):
        """Walk the pointer there in <=120 px steps. QEMU's PS/2 mouse is
        RELATIVE, so there is no 'move to' -- the position has to be tracked on
        this side, and the WM prints its starting point (`[wm] ptr 640 400`)."""
        while self.cursor[0] != tx or self.cursor[1] != ty:
            sx = max(-120, min(120, tx - self.cursor[0]))
            sy = max(-120, min(120, ty - self.cursor[1]))
            self.cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "rel", "data": {"axis": "x", "value": sx}},
                {"type": "rel", "data": {"axis": "y", "value": sy}}]}})
            self.cursor[0] += sx
            self.cursor[1] += sy
            time.sleep(0.05)
        time.sleep(0.2)

    def click(self):
        for d in (True, False):
            self.cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"button": "left", "down": d}}]}})
            time.sleep(0.12)

    def key(self, q, hold=0.10):
        for down in (True, False):
            self.cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "key", "data": {"key": {"type": "qcode", "data": q},
                                         "down": down}}]}})
        time.sleep(hold)

    def shift_key(self, q):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "key", "data": {"key": {"type": "qcode", "data": "shift"},
                                     "down": True}}]}})
        self.key(q)
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "key", "data": {"key": {"type": "qcode", "data": "shift"},
                                     "down": False}}]}})

    def meta_key(self, q):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "key", "data": {"key": {"type": "qcode", "data": "meta_l"},
                                     "down": True}}]}})
        self.key(q)
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "key", "data": {"key": {"type": "qcode", "data": "meta_l"},
                                     "down": False}}]}})

    KMAP = {" ": "spc", ".": "dot", "\n": "ret", "-": "minus", "/": "slash",
            "?": "slash"}

    def typ(self, text):
        for ch in text:
            if ch == "?":
                self.shift_key("slash")
            else:
                self.key(self.KMAP.get(ch, ch))


# ------------------------------------------------------------------ the test --

fails = []


def chk(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        fails.append(msg)


def write_conf(g, port, path, key=""):
    """Write /etc/ai.conf from the serial shell. The shell's `echo a = b` prints
    its arguments joined by spaces, which is exactly the file format."""
    g.sh("rm /etc/ai.conf")
    g.sh("echo host = 10.0.2.2 > /etc/ai.conf")
    g.sh("echo port = %d >> /etc/ai.conf" % port)
    g.sh("echo path = %s >> /etc/ai.conf" % path)
    g.sh("echo tls = 0 >> /etc/ai.conf")
    g.sh("echo model = mock-stream-1 >> /etc/ai.conf")
    g.sh("echo key = %s >> /etc/ai.conf" % key)
    # Deliberately NOT `cat /etc/ai.conf`: this file is a credential file and a
    # harness that dumps it to the console is doing the exact thing the program
    # is being tested for not doing.


def write_conf_tls(g, port):
    g.sh("rm /etc/ai.conf")
    g.sh("echo host = 10.0.2.2 > /etc/ai.conf")
    g.sh("echo port = %d >> /etc/ai.conf" % port)
    g.sh("echo path = /v1/chat/completions >> /etc/ai.conf")
    g.sh("echo tls = 1 >> /etc/ai.conf")
    g.sh("echo model = mock-stream-1 >> /etc/ai.conf")
    g.sh("echo key = >> /etc/ai.conf")


WINRE = re.compile(r"\[wm\] win (\d+) frame (-?\d+) (-?\d+) (\d+) (\d+) content "
                   r"(\d+) (\d+) pt.*Chat")
TITLEBAR = 30            # points; c/kernel/gui/wm.c TITLEBAR_H


def launch(g):
    mark = len(g.log)
    g.sh("as /usr/as/examples/chlaunch.as")
    if not g.wait_for("CHLAUNCH_OK", 60, mark):
        g.die("chlaunch.as did not report CHLAUNCH_OK")
    if not g.wait_for("CH_READY", 90, mark):
        g.die("the chat window never printed CH_READY")
    g.pump(2.0)

    # Where the window landed. Read from the WM rather than assumed: the
    # compositor cascades windows, so a hardcoded origin silently clicks the
    # desktop the day another app opens first, and every assertion after that
    # fails for the wrong reason.
    m = None
    for line in g.snap()[mark:].splitlines():
        mm = WINRE.search(line)
        if mm:
            m = mm
    if not m:
        g.die("the WM never reported the Chat window's geometry")
    fx, fy, cw, chh = int(m.group(2)), int(m.group(3)), int(m.group(6)), int(m.group(7))
    # The prompt field, in the app's own layout terms: AUI_PAD in from the left,
    # one control height plus 2*AUI_SP(2) up from the bottom.
    fld_x = fx + 16 + 120
    fld_y = fy + TITLEBAR + chh - (28 + 16) + 8 + 14
    g.goto(fld_x, fld_y)
    g.click()
    g.pump(0.6)
    return (fx, fy, cw, chh)


def send(g, prompt):
    g.typ(prompt)
    g.pump(0.4)
    g.key("ret")


def run_refusal(g):
    print("\n-- the loud refusal: no /etc/ai.conf at all --")
    g.sh("rm /etc/ai.conf")
    launch(g)
    chk("CH_CONF_REFUSED" in g.log, "the app refused instead of starting anyway")
    chk("/etc/ai.conf is missing" in g.log,
        "and the refusal NAMES the file and what is wrong with it")
    shot = g.screendump("ch-refusal")
    shot.to_png(os.path.join(g.shots, "ch-refusal.png"))
    print("      screenshot: " + os.path.join(g.shots, "ch-refusal.png"))
    # A malformed file, and the refusal must not echo the line's content.
    g.sh("echo host = 10.0.2.2 > /etc/ai.conf")
    g.sh("echo nonsense line with no equals sign >> /etc/ai.conf")
    mark = len(g.log)
    send(g, "hi")
    if not g.wait_for("CH_ERROR", 40, mark):
        g.die("a malformed config produced no refusal at all")
    tail = g.log[mark:]
    chk("line 2" in tail, "a malformed line is reported BY LINE NUMBER")
    chk("nonsense line" not in tail,
        "and the line's CONTENT is never echoed (the key line is the likeliest "
        "one to be malformed)")
    # Close it: SYS_GUI_CREATE allows one window per app, so the streaming phase
    # needs this instance gone before it can launch its own.
    g.meta_key("w")
    g.pump(3)


def run_stream(g, port):
    print("\n-- streaming, and the text must grow while it streams --")
    write_conf(g, port, "/v1/chat/completions")
    launch(g)
    chk("CH_CONF_OK" in g.log, "the config was accepted")
    chk("auth=none" in g.log, "and no Authorization header will be sent (empty key)")

    base = g.screendump("ch-0-idle")
    mark = len(g.log)
    send(g, "hi")

    if not g.wait_for("CH_HEADERS", 90, mark):
        g.die("no response headers -- the guest never reached the mock endpoint")
    hdr = [l for l in g.log[mark:].splitlines() if l.startswith("CH_HEADERS")][0]
    chk("code=200" in hdr, "the endpoint answered 200: " + hdr)
    chk("chunked=1" in hdr, "the response used CHUNKED transfer-encoding")
    chk("streaming=1" in hdr, "and http1.c is delivering it through the body sink")

    # TWO SHOTS, both inside the slow phase. The slow phase is
    # len(SLOW) * SLOW_DELAY seconds long, so these land comfortably inside it.
    time.sleep(2.5)
    g.pump(0.1)
    a = g.screendump("ch-1-early")
    time.sleep(5.0)
    g.pump(0.1)
    b = g.screendump("ch-2-later")

    # The region: below the menu bar (whose clock ticks) and above the dock.
    box = (0, 40, 1280, 700)
    na, lowa = a.diff_box(base, *box)
    nb, lowb = b.diff_box(base, *box)
    print("      changed pixels vs the idle window:  early %d (lowest row %d)  "
          "later %d (lowest row %d)" % (na, lowa, nb, lowb))
    chk(na > 0, "text was already on screen at the first shot")
    chk(nb > na, "THE SECOND SHOT HAS STRICTLY MORE TEXT THAN THE FIRST "
                 "(%d > %d changed pixels)" % (nb, na))
    chk(lowb >= lowa, "and the text grew DOWNWARD (lowest changed row %d -> %d)"
        % (lowa, lowb))

    if not g.wait_for("CH_STREAM_END", 180, mark):
        g.die("the stream never ended")
    end = [l for l in g.log[mark:].splitlines() if l.startswith("CH_STREAM_END")][0]
    print("      " + end)
    kv = dict(p.split("=", 1) for p in end.split()[1:] if "=" in p)

    # CH_STREAM_END is printed by finish(), which only marks the window dirty --
    # the last frame has not been composited yet. Photographing on the marker
    # alone catches the window one repaint short of the final token, which looks
    # exactly like a bug in the thing under test.
    time.sleep(2.5)
    final = g.screendump("ch-3-final")
    final.to_png(os.path.join(g.shots, "ch-3-final.png"))
    a.to_png(os.path.join(g.shots, "ch-1-early.png"))
    b.to_png(os.path.join(g.shots, "ch-2-later.png"))
    print("      screenshot: " + os.path.join(g.shots, "ch-3-final.png"))

    nc, _ = final.diff_box(base, *box)
    chk(nc > nb, "the final window has more text again (%d > %d)" % (nc, nb))

    chk(kv.get("deltas") == str(len(TOKENS)),
        "every one of the %d tokens arrived (deltas=%s)" % (len(TOKENS), kv.get("deltas")))
    chk(kv.get("refused") == "0", "no record was refused (refused=%s)" % kv.get("refused"))
    chk(kv.get("code") == "200", "code=200")
    chk(kv.get("chunked") == "1", "chunked=1")
    # Every comment line, counted exactly. A loose ">0" here would have missed
    # the sink-misrouting bug: the FIRST chunk of the body was being filed as an
    # error body, which cost one comment and one token and nothing else.
    want_comments = 1 + sum(1 for i in range(len(TOKENS)) if i % 11 == 5)
    chk(kv.get("comments") == str(want_comments),
        "every keep-alive comment line was seen and ignored (comments=%s, want %d)"
        % (kv.get("comments"), want_comments))
    chk(kv.get("records") == str(len(TOKENS) + 1),
        "every record was dispatched, [DONE] included (records=%s, want %d)"
        % (kv.get("records"), len(TOKENS) + 1))

    # THE BYTES SURVIVED. Same hash, computed on the host over what was sent.
    want = "%08x" % fnv1a(REPLY)
    chk(kv.get("fnv") == want,
        "the assembled reply is byte-identical to what the server sent "
        "(fnv %s, want %s)" % (kv.get("fnv"), want))

    # THE REPAINT BOUND. 200 of those tokens went out with no delay at all; a
    # window that repainted per token would have drawn 224 frames.
    ms = int(kv.get("ms", "0"))
    rep = int(kv.get("repaints", "0"))
    bound = ms // 40 + 20
    print("      repaint budget: %d repaints for %d deltas over %d ms "
          "(bound %d, skipped %s)" % (rep, len(TOKENS), ms, bound, kv.get("skipped")))
    chk(rep <= bound,
        "repaints are bounded by TIME, not by token count (%d <= %d)" % (rep, bound))
    chk(rep < len(TOKENS),
        "and the 200-token burst did NOT cost 200 frames (%d < %d)"
        % (rep, len(TOKENS)))

    # What the mock actually received.
    with seen_lock:
        reqs = list(seen)
    chk(len(reqs) >= 1, "the mock endpoint received the request")
    if reqs:
        path, hdr, body = reqs[-1]
        chk(path == "/v1/chat/completions", "request-target was %s" % path)
        chk("authorization" not in hdr,
            "NO Authorization header was sent for an empty key")
        chk('"stream":true' in body.replace(" ", ""), "the body asked for stream:true")
        chk('"mock-stream-1"' in body, "the body carried the configured model")
        chk('"role":"user"' in body, "the body carried the user turn")


def run_failures(g, port):
    print("\n-- the failures, out loud --")

    # 401, with a CANARY in the key slot. Not a credential: a string that must
    # never appear on the console.
    write_conf(g, port, "/v1/401", key=CANARY)
    # The window from here on is the APP's output. The check cannot cover the
    # whole log, and that is a property of the harness, not a weakening of the
    # claim: the only way to put a file on this disk from outside is to type at
    # the serial shell, and the shell ECHOES what it is typed. So the canary is
    # on the console because this script put it there. What is asserted is that
    # nothing the program itself writes -- its status line, its error path, its
    # request handling -- ever contains it.
    mark = len(g.snap())
    send(g, "hi")
    if not g.wait_for("CH_HTTP_FAIL", 90, mark):
        g.die("the 401 produced no reported failure")
    after = g.snap()[mark:]
    chk("code=401" in after, "a 401 is reported as a 401")
    chk(CANARY not in after,
        "THE KEY IS NEVER PRINTED: with a canary key configured, nothing the "
        "app wrote during a 401 contains it")
    g.screendump("ch-4-401").to_png(os.path.join(g.shots, "ch-4-401.png"))
    g.pump(1)

    # 429
    write_conf(g, port, "/v1/429")
    mark = len(g.log)
    send(g, "hi")
    if not g.wait_for("CH_HTTP_FAIL", 90, mark):
        g.die("the 429 produced no reported failure")
    chk("code=429" in g.log[mark:], "a 429 is reported as a 429")
    g.pump(1)

    # A truncated stream: three tokens, then the server hangs up with no [DONE].
    write_conf(g, port, "/v1/trunc")
    mark = len(g.log)
    send(g, "hi")
    if not g.wait_for("CH_TRUNCATED", 120, mark):
        g.die("a stream cut short was not reported as truncated")
    line = [l for l in g.log[mark:].splitlines() if l.startswith("CH_TRUNCATED")][0]
    chk("deltas=3" in line,
        "a truncated stream keeps what arrived and says it is incomplete: " + line)
    g.screendump("ch-5-truncated").to_png(os.path.join(g.shots, "ch-5-truncated.png"))
    g.pump(1)

    # TLS against a server that speaks none: the handshake must be refused,
    # named, and survivable. No certificate and no key are involved.
    write_conf_tls(g, port)
    mark = len(g.log)
    send(g, "hi")
    if not g.wait_for("CH_ERROR", 150, mark):
        g.die("a TLS handshake against a plaintext server produced no error")
    chk("TLS" in g.log[mark:] or "connection" in g.log[mark:],
        "a refused TLS handshake is named, not swallowed")
    g.pump(1)


def run_close_midstream(g, port):
    print("\n-- closing the window mid-stream --")
    write_conf(g, port, "/v1/chat/completions")
    mark = len(g.log)
    send(g, "hi")
    if not g.wait_for("CH_HEADERS", 90, mark):
        g.die("the stream did not start, so it cannot be interrupted")
    time.sleep(2.0)
    g.meta_key("w")                      # Cmd+W -> EV_CLOSE (see wm_shortcut)
    if not g.wait_for("CH_CLOSED_MIDSTREAM", 60, mark):
        g.die("closing the window mid-stream was not handled")
    chk(True, "the app reported the mid-stream close and exited")
    g.pump(2)
    chk("CH_STREAM_END" not in g.log[mark:],
        "and it did not pretend the stream finished")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--only", default="all")
    ap.add_argument("--shots", default=None)
    args = ap.parse_args()

    shots = args.shots or os.path.join("build", "ch-shots")
    os.makedirs(shots, exist_ok=True)

    srv = Server(("0.0.0.0", 0), Handler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("mock endpoint on host port %d (10.0.2.2:%d from the guest), no key"
          % (port, port))

    g = Guest(args.iso, args.disk, shots)
    if not g.wait_for("LOGIT_BOOT_OK", 300):
        g.die("kernel did not boot")
    g.pump(6)
    g.caps()

    try:
        if args.only in ("all", "refusal"):
            run_refusal(g)
        if args.only in ("all", "stream"):
            run_stream(g, port)
        if args.only in ("all", "failures"):
            run_failures(g, port)
        if args.only in ("all", "close"):
            run_close_midstream(g, port)
    finally:
        g.pump(0.5)
        path = os.path.join(shots, "serial.log")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(g.snap())
        print("\nserial console: " + path)
        g.kill()

    print()
    if fails:
        print("FAIL: %d assertion%s" % (len(fails), "" if len(fails) == 1 else "s"))
        for f in fails:
            print("   - " + f)
        sys.exit(1)
    print("PASS: the reply arrived in pieces and each piece was drawn")
    print("screenshots in " + shots)


if __name__ == "__main__":
    main()
