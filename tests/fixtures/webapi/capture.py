#!/usr/bin/env python3
"""Capture a real page and the scripts it loads into a reproducible fixture.

    python3 tests/fixtures/webapi/capture.py <name> <url> [--ua UA] [--max-kb N]

WHY THE CORPUS IS CAPTURED AND COMMITTED, NOT FETCHED
The point of the probe (tests/unit/webapi_probe.c) is a HISTOGRAM: which
JavaScript globals real pages reach for and do not find, ranked. A histogram is
only comparable against itself. A page fetched at run time changes under the
measurement -- the site ships a new bundle, the CDN A/Bs a different one -- and
then "the miss list moved" says nothing about our code. These are the exact
bytes the numbers in the commit message were measured on.

It is also the only way the probe can be a UNIT test: no network, no DNS, no
TLS, no QEMU. It parses committed bytes and runs committed script.

THE USER-AGENT MATTERS AND IS THEREFORE PINNED
Every one of these sites serves a different document per UA. The fixture must
be the document OUR browser gets, so the default UA here is byte-for-byte the
one js_page.c publishes as navigator.userAgent. Capturing with a Chrome UA
would measure a page LogitOS never receives.

Layout produced:
    tests/fixtures/webapi/<name>/index.html    the document, unmodified
    tests/fixtures/webapi/<name>/s###.js       each <script src> it loads
    tests/fixtures/webapi/<name>/manifest.txt  "<src attribute>\t<local file>"

The manifest is keyed by the RAW src attribute because that is what the probe
sees on the DOM node -- no URL resolution in the loop, so a resolver bug cannot
silently empty the corpus.
"""

import os
import re
import sys
import urllib.parse
import subprocess

UA = "Mozilla/5.0 (LogitOS; x86_64) Logit/1.0"
HERE = os.path.dirname(os.path.abspath(__file__))

SCRIPT_RE = re.compile(rb"<script\b([^>]*)>", re.I)
ATTR_RE = re.compile(rb"""(\w[\w-]*)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))""")


def curl(url, ua, timeout=40):
    p = subprocess.run(
        ["curl", "-sSL", "--max-time", str(timeout), "-A", ua, url],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        return None
    return p.stdout


def attrs(blob):
    """Attribute values with entities DECODED -- the manifest is keyed on what
    the DOM will hold, not on what the bytes say. Wikipedia's startup module is
    `load.php?lang=en&amp;modules=...`; keyed raw, the probe would look up a
    string no DOM node ever has and the corpus would silently lose the file."""
    out = {}
    for m in ATTR_RE.finditer(blob):
        v = m.group(2) or m.group(3) or m.group(4) or b""
        s = v.decode("latin1")
        s = s.replace("&amp;", "&").replace("&quot;", '"').replace("&#39;", "'")
        s = s.replace("&lt;", "<").replace("&gt;", ">")
        out[m.group(1).lower().decode("latin1")] = s
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    name, url = sys.argv[1], sys.argv[2]
    ua = UA
    max_kb = 512
    a = sys.argv[3:]
    if "--ua" in a:
        ua = a[a.index("--ua") + 1]
    if "--max-kb" in a:
        max_kb = int(a[a.index("--max-kb") + 1])

    out = os.path.join(HERE, name)
    os.makedirs(out, exist_ok=True)

    html = curl(url, ua)
    if html is None:
        print("FAILED to fetch", url)
        return 1
    open(os.path.join(out, "index.html"), "wb").write(html)
    print("%-10s %8d bytes  %s" % (name, len(html), url))

    man = []
    n = 0
    for m in SCRIPT_RE.finditer(html):
        at = attrs(m.group(1))
        src = at.get("src")
        if not src:
            continue
        typ = (at.get("type") or "").lower()
        if typ and "javascript" not in typ and typ != "module":
            continue
        full = urllib.parse.urljoin(url, src)
        if not full.startswith("http"):
            continue
        body = curl(full, ua)
        if body is None:
            print("   (skip, fetch failed) %s" % src[:80])
            continue
        if len(body) > max_kb * 1024:
            print("   (skip, %d KiB > %d KiB cap) %s" % (len(body) // 1024, max_kb, src[:80]))
            continue
        n += 1
        fn = "s%03d.js" % n
        open(os.path.join(out, fn), "wb").write(body)
        man.append("%s\t%s" % (src, fn))
        print("   %-12s %8d bytes  %s" % (fn, len(body), src[:80]))

    open(os.path.join(out, "manifest.txt"), "w").write("\n".join(man) + ("\n" if man else ""))
    open(os.path.join(out, "SOURCE"), "w").write("%s\nUA: %s\n" % (url, ua))
    return 0


if __name__ == "__main__":
    sys.exit(main())
