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

MODULE GRAPHS ARE WALKED, AND THE REASON IS THE WHOLE POINT OF THIS EDIT.
The version of this script that captured the committed corpus fetched the
document's <script src> attributes and stopped. That is a complete capture of a
page written in 2010 and a partial one of every page written since: a module
entry point is one file that imports the other hundred and thirty, and none of
those hundred and thirty were in any fixture. So `kimi: 3 scripts, all clean`
was a measurement of kimi with the application removed.

Worse, kimi's ENTRY POINT was not captured either -- index-h6DE6Ow7.js is
1.55 MB and --max-kb defaulted to 512, so the one file the fixture existed for
was dropped by a size cap that printed a line nobody read. The cap now defaults
high enough for a real bundle, and a transitive chunk is keyed in the manifest
by its ABSOLUTE URL, which is what the module loader resolves a specifier to.

Specifiers are extracted with three narrow patterns rather than one broad one
(see IMPORT_RES): minified code is full of strings, and a regex loose enough to
catch every import is loose enough to fetch a page's error messages.
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

# The three shapes a module specifier takes. `from` is required not to be
# preceded by a dot so that Array.from("abc") is not read as an import -- that
# single exclusion is the difference between walking a module graph and
# downloading a bundle's string table.
IMPORT_RES = [
    ("static", re.compile(rb"""(?<![.\w$])from\s*["']([^"'\n]+)["']""")),
    ("dynamic", re.compile(rb"""(?<![.\w$])import\s*\(\s*["']([^"'\n]+)["']\s*\)""")),
    ("static", re.compile(rb"""(?<![.\w$])import\s*["']([^"'\n]+)["']""")),
]


def specifiers(body):
    """(kind, specifier) for every import target in `body`, deduplicated.

    STATIC AND DYNAMIC ARE SEPARATED, and that distinction decides what has to
    be in the fixture. A static import is a LINK EDGE: QuickJS resolves the
    whole static graph inside one JS_Eval, so one missing static chunk means
    the entry module does not compile and the application does not start at
    all. A dynamic import() is a runtime request that only fires on the code
    path that reaches it, and a missing one costs that feature and nothing
    else. So the static closure is captured completely and the dynamic edges
    get their own budget.

    Only path-shaped specifiers: a bare one ("react") has no meaning without an
    import map and js_module.c refuses it out loud, so fetching it here would
    put a file in the fixture that the loader can never ask for."""
    out, seen = [], set()
    for kind, rx in IMPORT_RES:
        for m in rx.finditer(body):
            s = m.group(1).decode("latin1")
            if not (s.startswith("./") or s.startswith("../") or
                    s.startswith("/") or s.startswith("http")):
                continue
            if s in seen:
                continue
            seen.add(s)
            out.append((kind, s))
    return out


def curl(url, ua, timeout=40):
    p = subprocess.run(
        # --compressed is not an optimisation, it is CORRECTNESS. Without it
        # curl does not send Accept-Encoding, but plenty of servers ship
        # gzip/brotli anyway (or ignore its absence), and the fixture then
        # holds compressed BYTES that look like a JavaScript file and are not.
        # MEASURED: tests/fixtures/webapi/baidureal/s012.js was 20 KB of binary
        # that node --check also refuses, and the probe reported it as
        # "SyntaxError: unexpected token in expression: ''" -- i.e. as an
        # engine bug of ours, on the user's own reported page.
        ["curl", "-sSL", "--compressed", "--max-time", str(timeout), "-A", ua, url],
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


def add_urls(name, urls):
    """Append already-known absolute URLs to an existing fixture.

    WHY A SECOND ENTRY POINT. A webpack bundle does not name its lazy chunks in
    source: the specifier is `import("./" + chunkId + "." + hash + ".js")`, built
    from a table at run time, so no amount of static analysis over the bytes
    will find them -- measured on MDN, whose entire Web Components layer loads
    that way and whose static closure is four files.

    They ARE named at run time, and both instruments already report them: the
    probe prints every module URL the fixture could not answer, and
    tests/chrome/webapi_chromediff.mjs logs every request its server 404s. So
    the corpus is completed from the measurement rather than from a parser:

        make probe-webapi PROBE=--json | grep '\tfetch\t' | cut -f5 \\
          | xargs python3 capture.py mdn --add

    which is a closed loop -- run, see what was missing, fetch exactly that."""
    out = os.path.join(HERE, name)
    if not os.path.isdir(out):
        print("no such fixture: %s" % out)
        return 1
    mpath = os.path.join(out, "manifest.txt")
    man = open(mpath).read().rstrip("\n").split("\n") if os.path.exists(mpath) else []
    man = [l for l in man if l.strip()]
    have = set(l.split("\t")[0] for l in man)
    n = max([int(l.split("\t")[1][1:4]) for l in man if l.split("\t")[1].startswith("s")] or [0])
    added = 0
    for u in urls:
        u = u.strip()
        if not u or u in have or not u.startswith("http"):
            continue
        body = curl(u, UA)
        if body is None:
            print("   (fetch failed) %s" % u[:90])
            continue
        n += 1
        fn = "s%03d.js" % n
        open(os.path.join(out, fn), "wb").write(body)
        man.append("%s\t%s" % (u, fn))
        have.add(u)
        added += 1
        print("   %-12s %8d bytes  %s" % (fn, len(body), u[:90]))
    open(mpath, "w").write("\n".join(man) + "\n")
    print("%s: +%d files, %d total" % (name, added, len(man)))
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    if sys.argv[2] == "--add":
        urls = sys.argv[3:]
        if not urls:
            urls = [l for l in sys.stdin.read().split()]
        return add_urls(sys.argv[1], urls)
    name, url = sys.argv[1], sys.argv[2]
    ua = UA
    # 4 MiB. The old default of 512 KiB silently dropped kimi's 1.55 MB entry
    # module -- the single file the kimi fixture was captured for.
    max_kb = 4096
    max_chunks = 200
    a = sys.argv[3:]
    if "--ua" in a:
        ua = a[a.index("--ua") + 1]
    if "--max-kb" in a:
        max_kb = int(a[a.index("--max-kb") + 1])
    if "--max-chunks" in a:
        max_chunks = int(a[a.index("--max-chunks") + 1])
    # The static closure is what the entry module needs to LINK; the dynamic
    # tail is what it would fetch later, and on a real application that tail is
    # four times the size of the closure (kimi: 79 files vs 337). --no-dynamic
    # captures the part without which nothing runs at all.
    no_dynamic = "--no-dynamic" in a

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
    have = {}                  # absolute URL -> local filename
    # (absolute URL, base URL it was reached from). A module's own URL is the
    # base its specifiers resolve against, so the base has to travel with the
    # work item -- resolving a chunk's imports against the DOCUMENT would be
    # right only for the entry point and wrong for every hop after it.
    queue = []

    def fetch_to_file(full, label):
        nonlocal n
        body = curl(full, ua)
        if body is None:
            print("   (skip, fetch failed) %s" % label[:90])
            return None
        if len(body) > max_kb * 1024:
            print("   (SKIP, %d KiB > %d KiB cap) %s" % (len(body) // 1024, max_kb, label[:90]))
            return None
        n += 1
        fn = "s%03d.js" % n
        open(os.path.join(out, fn), "wb").write(body)
        have[full] = fn
        print("   %-12s %8d bytes  %s" % (fn, len(body), label[:90]))
        return body

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
        body = fetch_to_file(full, src)
        if body is None:
            continue
        # Keyed by the RAW src attribute: that is the string on the DOM node.
        man.append("%s\t%s" % (src, have[full]))
        if typ == "module":
            queue.append((full, body))

    # The module graph, breadth first, keyed in the manifest by ABSOLUTE URL --
    # that is what js_module.c's normalizer produces and therefore the only key
    # the loader can ever look up.
    #
    # STATIC EDGES ARE EXHAUSTED FIRST, to the cap. Measured on kimi: with one
    # shared budget the walk spent all 200 slots on the dynamic long tail and
    # never fetched `katex-CLyXPy3k.js`, which is a STATIC import two hops in --
    # so the entry module failed to link and the probe reported the application
    # as broken. A partial static closure does not measure a page a bit less
    # well; it does not measure the page at all.
    seen = set()
    dyn = []                            # absolute URLs reached only by import()

    def walk(work):
        """BFS the STATIC closure of everything in `work`, queueing dynamic
        edges for later. Returns when the closure is complete or the cap hits."""
        while work and len(have) < max_chunks:
            base, body = work.pop(0)
            for kind, spec in specifiers(body):
                full = urllib.parse.urljoin(base, spec)
                if not full.startswith("http"):
                    continue
                if kind == "dynamic":
                    if full not in seen and full not in dyn:
                        dyn.append(full)
                    continue
                if full in seen:
                    continue
                seen.add(full)
                if full in have:
                    continue
                sub = fetch_to_file(full, spec)
                if sub is None:
                    continue
                man.append("%s\t%s" % (full, have[full]))
                work.append((full, sub))
                if len(have) >= max_chunks:
                    print("   (stopping: %d chunks is the --max-chunks cap)" % max_chunks)
                    return

    walk(queue)
    print("   -- static closure: %d files --" % len(have))
    while dyn and len(have) < max_chunks and not no_dynamic:
        full = dyn.pop(0)
        if full in seen:
            continue
        seen.add(full)
        if full in have:
            continue
        sub = fetch_to_file(full, "import() " + full.rsplit("/", 1)[-1])
        if sub is None:
            continue
        man.append("%s\t%s" % (full, have[full]))
        walk([(full, sub)])             # a lazy chunk has a static closure of its own
    print("   -- with dynamic chunks: %d files --" % len(have))

    open(os.path.join(out, "manifest.txt"), "w").write("\n".join(man) + ("\n" if man else ""))
    open(os.path.join(out, "SOURCE"), "w").write("%s\nUA: %s\n" % (url, ua))
    return 0


if __name__ == "__main__":
    sys.exit(main())
