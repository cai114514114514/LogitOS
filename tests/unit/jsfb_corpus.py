#!/usr/bin/env python3
"""jsfb_corpus.py -- ONE reader for build/jsfb, so nothing spells its layout twice.

    python3 tests/unit/jsfb_corpus.py                 # every implementation
    python3 tests/unit/jsfb_corpus.py --free          # the ones needing no npm build
    python3 tests/unit/jsfb_corpus.py --free --paths  # ... just the document dirs
    python3 tests/unit/jsfb_corpus.py --json

WHY THIS FILE EXISTS AND IS NOT THREE LINES INLINE IN A HARNESS.
Two facts about this corpus are NOT derivable from a directory listing, and both
were got wrong the first time somebody looked:

  * WHETHER AN IMPLEMENTATION NEEDS AN npm BUILD is a property of its
    package.json `scripts.build-prod`, not of its name or its file list.
    tools/jsfb_fetch.sh already states the rule -- "writing it into a list here
    would be a second door onto the same jar" -- and calls this module rather
    than repeating the test, because this tree has paid three times for a
    constant spelled twice (/dev/log, LOGIT_ARG_MAX, the IME chord).

  * WHERE THE DOCUMENT IS is `js-framework-benchmark.customURL` when the
    package declares one, and `index.html` beside package.json when it does not.
    39 of 229 implementations declare one. A harness that assumes
    `<dir>/index.html` silently drops binding.scala and reflex-dom -- which are
    exactly the compiler-generated implementations the corpus is most valuable
    for, because their JavaScript was emitted by Scala and Haskell and exercises
    the DOM in shapes no hand-written bundle produces.

NOTHING HERE BRANCHES ON AN IMPLEMENTATION'S NAME. Every field is read out of
that implementation's own package.json. A matrix built on a name list is a
matrix that can be fitted to; this one cannot, and that is the entire reason the
corpus was chosen.
"""
import argparse
import json
import os
import sys

DEFAULT_ROOT = "build/jsfb"


def _build_free(script):
    """Does `npm run build-prod` produce nothing?

    Upstream spells "no build needed" three ways and they all mean the same
    thing: absent, `exit 0`, or an `echo` explaining that the generated
    JavaScript is already committed (binding.scala, reflex-dom). Judging by the
    SCRIPT rather than by a list is what keeps this correct when upstream adds
    an implementation, which it does continuously.
    """
    s = (script or "").strip()
    return s == "" or s == "exit 0" or s.startswith("echo")


def impls(root=DEFAULT_ROOT):
    """Every implementation in the corpus, as dicts, sorted by name."""
    out = []
    fw = os.path.join(root, "frameworks")
    if not os.path.isdir(fw):
        return out
    for kind in sorted(os.listdir(fw)):
        kdir = os.path.join(fw, kind)
        if not os.path.isdir(kdir):
            continue
        for name in sorted(os.listdir(kdir)):
            d = os.path.join(kdir, name)
            pj = os.path.join(d, "package.json")
            if not os.path.isfile(pj):
                continue
            try:
                pkg = json.load(open(pj))
            except Exception as e:                     # a package.json that does
                pkg = {"__error": str(e)}              # not parse is REPORTED,
            meta = pkg.get("js-framework-benchmark", {})   # never skipped
            if not isinstance(meta, dict):
                meta = {}
            custom = str(meta.get("customURL", "") or "").strip("/")
            docdir = os.path.join(d, custom) if custom else d
            doc = os.path.join(docdir, "index.html")
            out.append({
                "name": "%s/%s" % (kind, name),
                "kind": kind,
                "dir": d,
                # `docdir` is what a probe is pointed at: the directory the
                # document sits in, which is what its relative <script src>
                # attributes resolve against.
                "docdir": docdir if os.path.isfile(doc) else None,
                "doc": doc if os.path.isfile(doc) else None,
                "customURL": custom or None,
                "language": meta.get("language") or "",
                "build_free": _build_free(pkg.get("scripts", {}).get("build-prod")),
                "error": pkg.get("__error"),
            })
    return out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--free", action="store_true", help="only implementations needing no npm build")
    ap.add_argument("--paths", action="store_true", help="print document directories only")
    ap.add_argument("--args", action="store_true",
                    help="print webapi_probe NAME=PATH arguments (the label is the "
                         "implementation, not the leaf directory)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--count", action="store_true", help="one line: free/needs-build/no-document")
    a = ap.parse_args(argv)

    all_ = impls(a.root)
    if not all_:
        sys.stderr.write("jsfb_corpus: nothing under %s/frameworks -- run "
                         "`bash tools/jsfb_fetch.sh` first\n" % a.root)
        return 2
    rows = [i for i in all_ if i["build_free"]] if a.free else all_

    if a.count:
        free = [i for i in all_ if i["build_free"]]
        print("%d implementations: %d need no build, %d need an npm build, "
              "%d have no document" % (len(all_), len(free),
                                       len(all_) - len(free),
                                       sum(1 for i in all_ if not i["doc"])))
        return 0
    if a.json:
        print(json.dumps(rows, indent=1))
        return 0
    if a.args:
        for i in rows:
            if i["docdir"]:
                print("%s=%s" % (i["name"], i["docdir"]))
            else:
                sys.stderr.write("jsfb_corpus: NO DOCUMENT: %s\n" % i["name"])
        return 0
    if a.paths:
        for i in rows:
            # A row with no document is NAMED on stderr rather than dropped: a
            # list that silently shortens is how a denominator stops meaning
            # anything.
            if i["docdir"]:
                print(i["docdir"])
            else:
                sys.stderr.write("jsfb_corpus: NO DOCUMENT: %s\n" % i["name"])
        return 0
    for i in rows:
        print("%-28s %-12s %-4s %s" % (i["name"], i["language"][:12],
                                       "free" if i["build_free"] else "npm",
                                       i["docdir"] or "(no index.html)"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
