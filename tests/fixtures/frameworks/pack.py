#!/usr/bin/env python3
"""Turn a framework's BUILT OUTPUT into a probe fixture.

    python3 tests/fixtures/frameworks/pack.py <name> <dist-dir> [--url URL]

WHY THE BUILT OUTPUT AND NOT THE SOURCE
A browser never receives `App.tsx`. It receives whatever the framework's own
toolchain emitted: a bundler runtime, a chunk-loading function, a hydration
entry point and a module graph. That emitted code is the only thing that
touches the platform, so it is the only thing worth measuring. A hand-written
`<script>` that pulls React off a CDN exercises none of it -- no chunk loader,
no `document.currentScript`, no code splitting -- and would have made this whole
corpus a measurement of nothing.

So: `tests/fixtures/frameworks/_src/<name>/` holds the app sources (small, and
the whole reason the corpus is reproducible), `build_apps.sh` runs each
framework's OWN default toolchain over them, and this script packs the `dist/`
that comes out into the layout tests/unit/webapi_probe.c reads.

WHY IT IS THE SAME LAYOUT AS tests/fixtures/webapi/
Because then the numbers are comparable. nodejs.org and x.com are already in
that corpus and already fail; a framework fixture in a different shape would
need a second probe, and two instruments measuring the same thing is how you
get two different answers. The layout is:

    <name>/index.html     the document the toolchain emitted, unmodified
    <name>/s###.js        every emitted script, flat
    <name>/manifest.txt   "<src as the page names it>\t<local file>"
    <name>/SOURCE         the document's absolute URL, first line

THE MANIFEST IS KEYED TWICE, ON PURPOSE.
webapi_probe.c looks a `<script src>` up by the RAW attribute string
(`collect()`, a strcmp) and looks a module import up by the ABSOLUTE URL the
specifier resolved to (`bfetch`, via the manifest's derived third column). Those
are different strings for the same file whenever the document says
`src="main-ABC.js"` and the graph says `/main-ABC.js`. So every emitted script
gets an entry under its root-relative path -- which is what an import resolves
to -- and every `<script src>` additionally gets one under the attribute exactly
as written. Duplicate rows pointing at one file are harmless; a missing row is
not, and shows up as the probe reporting a page with its application removed.

WHAT IS PRUNED, AND WHY THAT IS SAFE
Only `.js`/`.mjs` are packed. CSS, fonts, images, favicons and source maps are
dropped: the probe has no network and fetches no sub-resource, so they could not
change a single JavaScript number, and Chrome's 404 for them lands in the
`network` bucket webapi_chromediff.mjs already excludes on both sides. It is the
difference between a 300 KB fixture and a 3 MB one. Said out loud rather than
done quietly, because "the page has no CSS" is a real limitation for the
question "does it PAINT" -- see the README.
"""

import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

SCRIPT_RE = re.compile(rb"<script\b([^>]*)>", re.I)
ATTR_RE = re.compile(rb"""(\w[\w-]*)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))""")

KEEP = (".js", ".mjs")


def script_srcs(html):
    """The `src` attribute of every <script> in the document, as written.

    Read with the same two regexes capture.py uses rather than a parser: the
    value needed here is the literal attribute text, and every HTML parser in
    reach normalises something about it."""
    out = []
    for m in SCRIPT_RE.finditer(html):
        attrs = {}
        for a in ATTR_RE.finditer(m.group(1)):
            k = a.group(1).decode("ascii", "replace").lower()
            v = a.group(2) or a.group(3) or a.group(4) or b""
            attrs[k] = v.decode("utf-8", "replace")
        if "src" in attrs and attrs["src"]:
            out.append(attrs["src"])
    return out


def main():
    args = [a for a in sys.argv[1:]]
    url = None
    if "--url" in args:
        i = args.index("--url")
        url = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__.strip().split("\n")[2])
        return 2
    name, dist = args
    dist = os.path.abspath(dist)
    if not url:
        url = "https://%s.fixture.logitos/" % name

    index = os.path.join(dist, "index.html")
    if not os.path.exists(index):
        print("pack: no index.html in %s" % dist)
        return 1

    out = os.path.join(HERE, name)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)

    html = open(index, "rb").read()
    open(os.path.join(out, "index.html"), "wb").write(html)
    open(os.path.join(out, "SOURCE"), "w", newline="\n").write(url + "\n")

    # Every emitted script, keyed by the root-relative path an import resolves
    # to. Sorted so the fixture is byte-identical across two runs of the packer
    # over the same dist -- os.walk order is not.
    files = []
    for root, _dirs, names in os.walk(dist):
        for n in sorted(names):
            if not n.endswith(KEEP):
                continue
            full = os.path.join(root, n)
            rel = os.path.relpath(full, dist).replace(os.sep, "/")
            files.append((rel, full))
    files.sort()

    rows = []          # (manifest key, local file)
    bypath = {}        # root-relative path -> local file
    for i, (rel, full) in enumerate(files, 1):
        local = "s%03d.js" % i
        shutil.copyfile(full, os.path.join(out, local))
        bypath["/" + rel] = local
        rows.append(("/" + rel, local))

    # ... and each <script src> under the attribute exactly as the page writes
    # it, because that is the string the probe's collect() compares.
    for src in script_srcs(html):
        key = src.split("#")[0]
        norm = key if key.startswith("/") else "/" + key.lstrip("./")
        if norm in bypath and key != norm:
            rows.append((key, bypath[norm]))
        elif norm not in bypath:
            print("  pack: <script src=%r> is not in the dist -- fixture is incomplete" % src)

    with open(os.path.join(out, "manifest.txt"), "w", newline="\n") as f:
        for key, local in rows:
            f.write("%s\t%s\n" % (key, local))

    total = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out))
    print("  %-9s %2d scripts, %d manifest rows, %d KB  (%s)"
          % (name, len(files), len(rows), total // 1024, url))
    return 0


if __name__ == "__main__":
    sys.exit(main())
