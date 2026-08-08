#!/usr/bin/env python3
"""Capture a real page + every stylesheet it <link>s into a fixture directory.

The corpus this builds is the WORK ORDER for the CSS engine: what the engine
must support is decided by counting what these pages actually declare, not by
reading a spec. Fixtures are committed (see README) because a corpus that is
re-fetched at measure time changes under the measurement.

  ./capture.py <name> <url>
"""
import os, re, sys, urllib.parse, subprocess, hashlib

UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 " \
     "(KHTML, like Gecko) Chrome/126.0 Safari/537.36"

def fetch(url, timeout=30):
    r = subprocess.run(["curl", "-sSL", "--compressed", "-m", str(timeout),
                        "-A", UA, "-H", "Accept-Language: en-US,en;q=0.9",
                        url], capture_output=True)
    if r.returncode != 0:
        return None
    return r.stdout

def main():
    name, url = sys.argv[1], sys.argv[2]
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    os.makedirs(d, exist_ok=True)
    html = fetch(url)
    if not html or len(html) < 512:
        print("%-12s FAIL (%s bytes)" % (name, len(html) if html else 0))
        return 1
    open(os.path.join(d, "index.html"), "wb").write(html)
    text = html.decode("utf-8", "replace")

    hrefs = []
    for m in re.finditer(r'<link\b[^>]*>', text, re.I):
        tag = m.group(0)
        if not re.search(r'rel\s*=\s*["\']?[^"\'>]*stylesheet', tag, re.I):
            continue
        # (?<![-\w]) so `data-href=` on GitHub's inert theme <link>s is not
        # mistaken for a real href -- those are alternates the page swaps in
        # with JS, and counting all 12 of them would triple the corpus's CSS.
        h = re.search(r'(?<![-\w])href\s*=\s*(?:"([^"]*)"|\'([^\']*)\'|([^\s>]+))', tag, re.I)
        if not h:
            continue
        href = (h.group(1) or h.group(2) or h.group(3)).strip()
        href = href.replace("&amp;", "&")
        if href and href not in hrefs:
            hrefs.append(href)

    man = ["# %s\n# %s\n" % (name, url)]
    n = 0
    for href in hrefs[:26]:
        full = urllib.parse.urljoin(url, href)
        css = fetch(full, 25)
        if not css or len(css) < 64:
            man.append("MISS %s\n" % full)
            continue
        n += 1
        fn = "sheet-%d.css" % n
        open(os.path.join(d, fn), "wb").write(css)
        man.append("%s %d %s\n" % (fn, len(css), full))
    open(os.path.join(d, "MANIFEST"), "w").write("".join(man))
    print("%-12s html=%-8d sheets=%-2d css=%d" %
          (name, len(html), n,
           sum(os.path.getsize(os.path.join(d, "sheet-%d.css" % i)) for i in range(1, n + 1))))
    return 0

sys.exit(main())
