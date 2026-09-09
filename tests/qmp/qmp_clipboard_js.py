#!/usr/bin/env python3
"""navigator.clipboard, on the real machine, checked by a SECOND REAL PROCESS.

    python3 tests/qmp/qmp_clipboard_js.py [--iso PATH] [--disk PATH]

This is the control the work order asked for, built the way clipboard.c's own
header insists a clipboard has to be tested: "the claim is that the clipboard
survives the death of the process that filled it, and the only way to test
that is two real processes." Here the two processes are the GUI Browser
(clicked over QMP, a real navigator.clipboard.writeText call) and the SERIAL
shell's /bin/clip (a completely different process, spawned by init over a
different tty), and the assertion is that /bin/clip reads back byte-for-byte
what the browser's click handler wrote -- including the CJK in it.

Four claims, each with a serial marker the page prints from inside a real
click handler (not page load -- a click is what every measured writeText call
site is gated behind):

  write     navigator.clipboard.writeText(CJK payload) resolves, and a SECOND
            PROCESS (/bin/clip paste, over the serial shell) reads the exact
            same bytes back, with an OWNER pid that is NOT the shell's own pid
            -- the two-process proof clipboard.c's own tests use.
  read      navigator.clipboard.readText() and .read() REJECT with a real
            DOMException named NotAllowedError. Neither resolves "" -- a page
            that got "" cannot tell "empty clipboard" from "read refused",
            which is the failure this item's own precedent (baidu's
            getContext-returns-null trap) is about.
  cap       a write over CLIP_MAX_BYTES REJECTS and leaves the PREVIOUS
            content on the store untouched -- checked by the second process
            again, so "untouched" is a claim about the kernel's store and not
            about what the page believes.
  absent    delete navigator.clipboard and click again: no exception escapes
            the handler (a page must be able to feature-test this), and the
            store is NOT touched -- proving the write in the first claim came
            from the API under test and not from some other path.
"""

import os
import sys
import subprocess
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM      # noqa: E402
import qmp_addrbar                  # noqa: E402  caret-derived address-bar geometry

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

RED, BLUE = (254, 1, 2), (1, 2, 254)

# The CJK payload is fixed in the page source (not Date.now()-derived) so the
# shell side of this driver can assert against a LITERAL string rather than
# parsing it back out of a serial log line -- one fewer thing that could agree
# with itself for the wrong reason.
PAYLOAD = "logit-clip-中文测试-\U0001F600-end"

PAGE = """<!doctype html>
<html><head><title>clip</title><style>
html, body { background: #ffffff; margin: 0; padding: 0; }
#btn { display: block; width: 220px; height: 70px; background: #fe0102;
       color: #000000; font-size: 24px; }
#btn2 { display: block; width: 220px; height: 70px; margin-top: 10px;
        background: #0102fe; color: #000000; font-size: 24px; }
#btn3 { display: block; width: 220px; height: 70px; margin-top: 10px;
        background: #01fe02; color: #000000; font-size: 24px; }
</style></head><body>
<div id="btn">COPY</div>
<div id="btn2">COPY-NOAPI</div>
<div id="btn3">BAIDU-REAL-CODE</div>
<script>
/* VERBATIM, byte for byte, from tests/fixtures/jsperf/baidu-async-search.js
 * (its t.prototype.copyToClipboard, renamed to a free function -- one of the
 * 6 real writeText call sites this item was built for) so the check below is
 * against the measured bundle's own bytes and not a paraphrase of them. */
var copyToClipboard = function(e){if(navigator.clipboard&&window.isSecureContext)return navigator.clipboard.writeText(e).then(function(){return!0
})["catch"](function(){return!1});try{var t=document.createElement("textarea");t.value=e,t.style.position="absolute",t.style.left="-9999px",t.style.opacity="0",document.body.appendChild(t),t.select(),t.setSelectionRange(0,e.length);var o=document.execCommand("copy");return document.body.removeChild(t),Promise.resolve(o)}catch(n){return Promise.resolve(!1)}};

document.getElementById('btn3').addEventListener('click', function () {
  /* This is baidu's OWN click-handler logic (s006.js), reproduced exactly:
     the promise's boolean decides between a "复制成功" and a "复制失败,
     请重试" toast -- the literal Chinese strings the real page shows a
     real user. */
  copyToClipboard('baidu-real-code-payload').then(function (c) {
    console.log('CLIP-BAIDU-TOAST ' + (c ? '复制成功' : '复制失败，请重试'));
  });
});
</script>
<script>
console.log('CLIP-START typeof-clipboard=' + (typeof navigator.clipboard) +
            ' typeof-writeText=' + (typeof (navigator.clipboard && navigator.clipboard.writeText)) +
            ' typeof-readText=' + (typeof (navigator.clipboard && navigator.clipboard.readText)) +
            ' typeof-read=' + (typeof (navigator.clipboard && navigator.clipboard.read)) +
            ' typeof-write=' + (typeof (navigator.clipboard && navigator.clipboard.write)));

var PAYLOAD = %s;

/* THE ACTIVATION GATE, TESTED AT LOAD -- i.e. with no click, ever.
 *
 * navigator.clipboard.writeText writes the clipboard EVERY PROCESS on this
 * machine reads. Without a gate any page could overwrite what the user had
 * copied out of Terminal or TextEdit, from a timer, with the browser in the
 * background. It shipped that way and an adversarial review of the diff
 * caught it -- no gate did, because this one did not exist.
 *
 * The payload is deliberately distinctive so the SECOND half of the check can
 * run in another process: if this write is wrongly allowed, /bin/clip will
 * hold this string. A log line saying "rejected" and a clipboard that
 * nonetheless changed is the failure a message-only assertion cannot see. */
navigator.clipboard.writeText('UNATTENDED-MUST-NOT-LAND').then(function () {
  console.log('CLIP-UNATTENDED-RESOLVED-BAD');
}).catch(function (e) {
  console.log('CLIP-UNATTENDED-REJECTED name=' + e.name);
});

document.getElementById('btn').addEventListener('click', function () {
  /* TRANSIENT ACTIVATION IS A WINDOW, NOT A FLAG, and this is the case that
   * distinguishes them. Every real copy button that awaits anything -- a
   * fetch, a canvas encode, one microtask -- calls writeText after its
   * handler has returned. A gate cleared at the end of dispatch would refuse
   * the single commonest legitimate use of this API while allowing nothing
   * extra, and would still pass a test that only writes synchronously. */
  /* It writes THE SAME PAYLOAD, not a marker of its own, and that is not
   * tidiness -- the first version wrote 'DEFERRED-WITHIN-WINDOW' and landed
   * AFTER the synchronous write, so it replaced the clipboard and reddened
   * two assertions that had nothing to do with it ("TEXT length 22 vs 32",
   * "a SECOND PROCESS read back the exact CJK payload"). A new case that
   * changes shared state under the cases around it manufactures failures,
   * which is worse than missing one. Writing the same bytes proves the
   * window without moving the store. */
  setTimeout(function () {
    navigator.clipboard.writeText(PAYLOAD).then(function () {
      console.log('CLIP-DEFERRED-OK');
    }).catch(function (e) {
      console.log('CLIP-DEFERRED-REJECTED-BAD name=' + e.name);
    });
  }, 50);

  navigator.clipboard.writeText(PAYLOAD).then(function (v) {
    console.log('CLIP-WRITE-OK returned=' + JSON.stringify(v));
  }).catch(function (e) {
    console.log('CLIP-WRITE-FAIL ' + e.name + ':' + e.message);
  });

  navigator.clipboard.readText().then(function (t) {
    console.log('CLIP-READ-RESOLVED ' + JSON.stringify(t));
  }).catch(function (e) {
    console.log('CLIP-READ-REJECTED name=' + e.name + ' isDOMException=' +
                (e instanceof DOMException) + ' msg=' + e.message);
  });

  navigator.clipboard.read().then(function (t) {
    console.log('CLIP-READ2-RESOLVED');
  }).catch(function (e) {
    console.log('CLIP-READ2-REJECTED name=' + e.name);
  });

  var big = new Array(70000).join('x');       // > CLIP_MAX_BYTES (64 KiB)
  navigator.clipboard.writeText(big).then(function () {
    console.log('CLIP-BIG-RESOLVED-BAD');
  }).catch(function (e) {
    console.log('CLIP-BIG-REJECTED name=' + e.name);
  });
});

document.getElementById('btn2').addEventListener('click', function () {
  try {
    delete navigator.clipboard;
    var r = navigator.clipboard;
    console.log('CLIP-NOAPI-DELETED typeof-after=' + typeof r);
    if (r && r.writeText) {
      r.writeText('should-not-run').then(function () {
        console.log('CLIP-NOAPI-WROTE-BAD');
      });
    } else {
      console.log('CLIP-NOAPI-NO-CRASH');
    }
  } catch (e) {
    console.log('CLIP-NOAPI-THREW ' + e);
  }
});
</script></body></html>
""" % ('"' + PAYLOAD.replace("\\", "\\\\").replace('"', '\\"') + '"')


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        raw = PAGE.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except OSError:
            pass

    def log_message(self, *_a):
        pass


class Guest:
    """QEMU with a QMP socket (the GUI Browser) AND a serial shell (/bin/clip).

    Exactly qmp_notify.py's Guest -- see that file's docstring for why
    `-serial stdio` with a held-open PIPE is the tested shape and a
    `-chardev socket` is not equivalent."""

    def __init__(self, iso, disk, tmp):
        self.tmp = tmp
        self.sock = os.path.join(tmp, "qmp.sock")
        self.serial = os.path.join(tmp, "serial.log")
        self.serfh = open(self.serial, "wb")
        self.qemu = subprocess.Popen(
            ["qemu-system-x86_64",
             "-cdrom", iso,
             "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % disk,
             "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
             "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi", "-cpu", "max",
             "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
             "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
             "-serial", "stdio", "-no-reboot",
             "-display", "none", "-qmp", "unix:%s,server,nowait" % self.sock],
            stdin=subprocess.PIPE, stdout=self.serfh, stderr=subprocess.DEVNULL)
        deadline = time.time() + 300
        while time.time() < deadline:
            if os.path.exists(self.serial) and "desktop live" in self.log():
                break
            if self.qemu.poll() is not None:
                raise RuntimeError("qemu exited early")
            time.sleep(0.2)
        else:
            raise RuntimeError("guest never reported a live desktop")
        self.s = Session(self.sock, serial=self.serial)
        for _ in range(10):
            self.sh("echo shell-is-up")
            if self.wait_for("shell-is-up", 12, after=1):
                break
        else:
            raise RuntimeError("the serial shell never answered")

    def log(self):
        try:
            with open(self.serial, errors="replace") as fh:
                return fh.read()
        except OSError:
            return ""

    def sh(self, line):
        self.qemu.stdin.write((line + "\n").encode())
        self.qemu.stdin.flush()

    def wait_for(self, needle, secs=15, after=0):
        end = time.time() + secs
        while time.time() < end:
            if self.log().count(needle) > after:
                return True
            time.sleep(0.1)
        return False

    def stop(self):
        try:
            self.qemu.stdin.close()
        except OSError:
            pass
        self.qemu.terminate()
        try:
            self.qemu.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.qemu.kill()
        self.serfh.close()


def line_after(text, marker):
    for ln in text.splitlines():
        i = ln.find(marker)
        if i >= 0:
            return ln[i + len(marker):].strip()
    return None


def main(argv):
    iso = disk = None
    i = 1
    while i < len(argv):
        if argv[i] == "--iso":   iso = argv[i + 1]; i += 2
        elif argv[i] == "--disk": disk = argv[i + 1]; i += 2
        else:
            print("unknown arg %r" % argv[i]); return 2
    iso = iso or os.path.join(ROOT, "build-apis", "logit.iso")
    disk = disk or os.path.join(ROOT, "build-apis", "disk.img")

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
    PORT = srv.server_port
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    tmp = tempfile.mkdtemp(prefix="logit-clipjs-")
    checks = []

    def ck(cond, name):
        checks.append((bool(cond), name))
        print(("ok: " if cond else "FAIL: ") + name)

    g = Guest(iso, disk, tmp)
    try:
        # ---- seed a KNOWN prior value, from the SHELL, so the "cap refusal
        # leaves the previous content untouched" check has a previous content
        # that this driver, not the browser, put there. ----
        SEED = "seed-before-browser-touches-anything"
        g.sh('clip copy %s' % SEED)
        ck(g.wait_for("CLIP_COPIED", 10), "seed: the shell wrote a known baseline")

# One click, at the tile the GUEST names for browser.aex, verified against the
        # guest's own [wm] launched line -- the re-click loop this replaces was
        # the apology a hand-kept slot constant needed. See qmp_ui's dock block.
        ui = g.s
        try:
            ui.launch_app("browser")
        except AssertionError as e:
            ck(False, str(e))
            return 1
        time.sleep(6)

        # Address bar via the caret (qmp_addrbar): the retired (420, 145) was
        # in the tab strip, and typing only worked because the Browser boots
        # with the bar already focused.
        bar = qmp_addrbar.focus(ui)
        for _ in range(70):
            ui.key("backspace", settle=0.02)
        ui.typ("http://10.0.2.2:%d/page.html" % PORT)
        qmp_addrbar.typed_echo(ui, bar)
        ui.key("ret")

        ck(g.wait_for("CLIP-START", 60), "the page loaded and its script ran")
        start = line_after(g.log(), "CLIP-START ")
        print("   " + str(start))
        ck(start == "typeof-clipboard=object typeof-writeText=function "
                    "typeof-readText=function typeof-read=function typeof-write=undefined",
           "the surface is exactly writeText/readText/read -- no write() (%s)" % start)

        # ---- THE ACTIVATION GATE, both halves ----
        # The page called writeText at load, with no click having happened.
        # A page that can write the machine's clipboard unattended can replace
        # whatever the user copied out of another application, silently.
        ck(g.wait_for("CLIP-UNATTENDED-REJECTED", 20),
           "a writeText with no user gesture behind it is REFUSED")
        ck("CLIP-UNATTENDED-RESOLVED-BAD" not in g.log(),
           "and it did not resolve -- a resolved promise over a write that did "
           "not happen is the lie this whole surface is written against")
        # AND THE SECOND PROCESS, because a rejected promise and a clipboard
        # that changed anyway look identical from inside the page. This is the
        # half a log-line assertion cannot see.
        g.sh("clip paste")
        time.sleep(1)
        ck("UNATTENDED-MUST-NOT-LAND" not in g.log(),
           "the refused write left NOTHING on the real clipboard -- checked "
           "from another process, not from the page that was refused")

        # ---- click #btn: writeText, readText, read, oversized writeText ----
        time.sleep(1.0)
        p0 = PPM(ui.screendump(os.path.join(tmp, "before.ppm")))
        box = p0.find_color(RED)
        ck(box is not None, "the COPY button is painted")
        ui.click_at((box[0] + box[2]) // 2, (box[1] + box[3]) // 2)

        ck(g.wait_for("CLIP-WRITE-OK", 20), "writeText() resolved")
        ck(g.wait_for("CLIP-READ-REJECTED", 20), "readText() rejected")
        rr = line_after(g.log(), "CLIP-READ-REJECTED ")
        ck(rr is not None and "name=NotAllowedError" in rr and "isDOMException=true" in rr,
           "...with a real DOMException named NotAllowedError (%s)" % rr)
        ck(g.wait_for("CLIP-READ2-REJECTED", 20), "read() rejected too")
        r2 = line_after(g.log(), "CLIP-READ2-REJECTED ")
        ck(r2 == "name=NotAllowedError", "...same DOMException name (%s)" % r2)
        ck(g.wait_for("CLIP-BIG-REJECTED", 20), "a >64KiB writeText() rejected")

        # The other side of the gate, and the one a too-strict implementation
        # fails: a copy button that awaits ANYTHING calls writeText after its
        # handler has returned. If activation were a flag cleared at the end of
        # dispatch, this would be refused -- and the synchronous case above
        # would still pass, so the suite would call a broken gate correct.
        ck(g.wait_for("CLIP-DEFERRED-OK", 20),
           "a writeText deferred out of the click handler still writes -- "
           "activation is a WINDOW, not a flag cleared at end of dispatch")
        ck("CLIP-DEFERRED-REJECTED-BAD" not in g.log(),
           "and it was not refused for having missed the synchronous moment")
        ck("CLIP-BIG-RESOLVED-BAD" not in g.log(), "...and never resolved")
        ck("CLIP-READ-RESOLVED" not in g.log(), 'readText() never resolved "" or anything else')
        ck("CLIP-READ2-RESOLVED" not in g.log(), "read() never resolved either")

        # ---- THE TWO-PROCESS CHECK: a DIFFERENT process reads it back ----
        # This shell has no ';' command separator (CLAUDE.md: "/bin/sh does not
        # accept -c"; it does not chain statements either) -- one line each.
        marker = "PASTE-%d" % int(time.time() * 1000)
        g.sh("echo %s-BEGIN" % marker)
        g.sh("clip info")
        g.sh("clip paste")
        g.sh("echo %s-END" % marker)
        ck(g.wait_for(marker + "-END", 15), "the shell's /bin/clip ran and returned")

        chunk = g.log().split(marker + "-BEGIN", 1)[-1].split(marker + "-END", 1)[0]
        info_line = line_after(chunk, "CLIP_FLAVOURS ")
        ck(info_line is not None, "clip info printed (%s)" % info_line)
        payload_utf8 = PAYLOAD.encode("utf-8")
        if info_line:
            # info_line starts right after "CLIP_FLAVOURS ", so re-anchor:
            toks = ("FLAVOURS " + info_line).split()
            d = {toks[i]: toks[i + 1] for i in range(0, len(toks) - 1, 2)}
            ck(d.get("TEXT") == str(len(payload_utf8)),
               "the store's TEXT length is the payload's UTF-8 byte length (%s vs %d)"
               % (d.get("TEXT"), len(payload_utf8)))
            ck("PID" in d and "OWNER" in d and d["OWNER"] != d["PID"],
               "the OWNER pid (the Browser, which wrote it) differs from this "
               "shell process's own PID (%s) -- TWO REAL PROCESSES (%s)" % (d.get("PID"), info_line))

        # The serial log echoes typed INPUT as well as printing real OUTPUT, so
        # e.g. "echo PASTE-...-BEGIN" (input) and "PASTE-...-BEGIN" (output)
        # BOTH contain the marker substring, and the marker itself can end up
        # split across the two -- filtering by "does not contain the marker"
        # is not reliable here. An EXACT match against the known payload has
        # no such ambiguity.
        pasted = next((ln.rstrip("\r") for ln in chunk.splitlines()
                       if ln.rstrip("\r") == PAYLOAD), None)
        ck(pasted == PAYLOAD,
           "a SECOND PROCESS read back the exact CJK payload the browser wrote\n"
           "       want: %r\n       got:  %r\n       ---- raw chunk ----\n%s"
           % (PAYLOAD, pasted, chunk))

        # ---- click #btn3: the REAL baidu bundle's copyToClipboard, verbatim.
        # Must click BEFORE #btn2 -- that one `delete`s navigator.clipboard
        # for the rest of the page's life, which would push baidu's own code
        # onto its execCommand fallback and prove nothing about the API. ----
        p1 = PPM(ui.screendump(os.path.join(tmp, "mid.ppm")))
        box3 = p1.find_color((1, 254, 2))
        ck(box3 is not None, "the BAIDU-REAL-CODE button is painted")
        ui.click_at((box3[0] + box3[2]) // 2, (box3[1] + box3[3]) // 2)
        ck(g.wait_for("CLIP-BAIDU-TOAST", 20), "baidu's own copyToClipboard ran end to end")
        toast = line_after(g.log(), "CLIP-BAIDU-TOAST ")
        ck(toast == "复制成功",
           "the REAL bundle's own toast branch is success, not '复制失败，请重试' (got %r)" % toast)

        # ---- click #btn2: navigator.clipboard deleted, must not crash and
        # must not touch the store (the oversized write above must have left
        # the GOOD payload in place, and this click must not disturb it either) ----
        p1 = PPM(ui.screendump(os.path.join(tmp, "mid.ppm")))
        box2 = p1.find_color(BLUE)
        ck(box2 is not None, "the COPY-NOAPI button is painted")
        ui.click_at((box2[0] + box2[2]) // 2, (box2[1] + box2[3]) // 2)
        ck(g.wait_for("CLIP-NOAPI-DELETED", 20), "delete navigator.clipboard; click again")
        na = line_after(g.log(), "CLIP-NOAPI-DELETED ")
        ck(na == "typeof-after=undefined", "...and it is really gone, not just falsy (%s)" % na)
        ck(g.wait_for("CLIP-NOAPI-NO-CRASH", 20), "the handler took the missing-API branch cleanly")
        ck("CLIP-NOAPI-THREW" not in g.log(), "a well-written feature test does not throw")

        # The last GENUINE write was baidu's own copyToClipboard (#btn3, just
        # above) -- the deleted-API click (#btn2) must leave that untouched.
        LAST_GOOD = "baidu-real-code-payload"
        marker2 = "PASTE2-%d" % int(time.time() * 1000)
        g.sh("echo %s-BEGIN" % marker2)
        g.sh("clip paste")
        g.sh("echo %s-END" % marker2)
        ck(g.wait_for(marker2 + "-END", 15), "second read-back ran")
        chunk2 = g.log().split(marker2 + "-BEGIN", 1)[-1].split(marker2 + "-END", 1)[0]
        pasted2 = next((ln.rstrip("\r") for ln in chunk2.splitlines()
                        if ln.rstrip("\r") == LAST_GOOD), None)
        ck(pasted2 == LAST_GOOD,
           "the store is UNTOUCHED by the deleted-API click -- still holds what "
           "baidu's own code last wrote (%r)" % pasted2)

    finally:
        g.stop()

    failed = [name for ok, name in checks if not ok]
    print()
    if failed:
        print("FAIL (%d/%d): %s" % (len(failed), len(checks), "; ".join(failed)))
        return 1
    print("PASS: %d/%d checks" % (len(checks), len(checks)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
