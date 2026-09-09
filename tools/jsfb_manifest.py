#!/usr/bin/env python3
"""jsfb_manifest.py -- record and verify what the fetched jsfb corpus contains.

WHY THIS EXISTS, AND IT IS A MEASURED FAILURE RATHER THAN A PRECAUTION.

On 2026-08-29 a workflow ran a causal experiment by INJECTING a fourteen-line
`__retain` prelude into four documents inside the fetched corpus --
keyed/svelte, keyed/solid-store, keyed/lit, keyed/react-hooks -- and left them
there. A later run of the same probe binary silently inherited the intervention
and disagreed with the published matrix (svelte 7/8 instead of 0/8, solid-store
8/8 instead of 0/8).

IT WAS CAUGHT ONLY BECAUSE TWO RUNS OF THE SAME BINARY DISAGREED AND SOMEBODY
CHASED IT INSTEAD OF PUBLISHING IT. Nothing in the tree would have said a word.
That is the whole argument for this file: build/jsfb is the one input to the
conformance gate that nothing checks and that workflows are expected to poke at,
and an injected experiment reads as a browser improvement.

It is deliberately NOT a checksum of every file. The corpus is ~230
implementations and most of what npm leaves in it -- node_modules, dist output,
lockfile churn -- is expected to differ between machines and between builds, so
hashing all of it produces a gate that is red for uninteresting reasons, which
this tree already knows is worse than no gate (CLAUDE.md, rule 5). What is
hashed is exactly the surface a MEASUREMENT reads: each implementation's
document (wherever jsfb_corpus says it lives) plus the shared css/ the harness
serves. Those are upstream's bytes; nothing in a normal build or a normal
`npm ci` rewrites them, so a difference means somebody edited the thing being
measured.

The reader of "where is the document" is jsfb_corpus.py and only jsfb_corpus.py
-- one jar, one door. This file asks it rather than re-deriving customURL, which
is the mistake tools/jsfb_fetch.sh's own comment records having already made
once with `build-prod`.

    jsfb_manifest.py --root build/jsfb --write     after a fetch
    jsfb_manifest.py --root build/jsfb --check     before believing a number

--check exits 0 when the corpus matches, 2 when it has drifted (naming every
file and whether it was modified, added or removed), and 1 when it cannot tell
-- an absent manifest is NOT a pass. A verifier that reports success because it
found nothing to verify is the `test-url reports 32/32 while both corpora are
absent` shape, and this tree has that one written down.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS_PY = os.path.join(HERE, "..", "tests", "unit", "jsfb_corpus.py")
MANIFEST = "JSFB_MANIFEST.json"


def documents(root):
    """Ask jsfb_corpus where every implementation's document is.

    Not a second implementation of customURL parsing: jsfb_corpus.py is the
    one reader in this tree and the fetch script already says why."""
    out = subprocess.run(
        [sys.executable, CORPUS_PY, "--root", root, "--json"],
        capture_output=True, text=True)
    if out.returncode != 0:
        sys.stderr.write("jsfb_manifest: jsfb_corpus.py failed:\n" + out.stderr)
        return None
    try:
        data = json.loads(out.stdout)
    except json.JSONDecodeError:
        # jsfb_corpus predates --json, or its shape moved. Say which, rather
        # than silently falling back to a scan that would hash a different set
        # of files than the harness reads -- a manifest over the wrong files
        # is worse than none, because it goes green.
        sys.stderr.write(
            "jsfb_manifest: jsfb_corpus.py --json did not return JSON.\n"
            "  This file must not guess where documents live; that is exactly\n"
            "  the second door tools/jsfb_fetch.sh's comment warns about.\n"
            "  Teach jsfb_corpus.py --json, or run with --docs-from-scan and\n"
            "  accept that the set may differ from what the matrix reads.\n")
        return None
    paths = []
    for impl in (data if isinstance(data, list) else data.get("implementations", [])):
        # `doc` is jsfb_corpus's spelling and it is ROOT-PREFIXED (it emits
        # "build/jsfb/frameworks/..."), so it has to be relativised here.
        # Getting either half wrong is silent: the first draft of this file
        # looked for a key named "document", found none, and wrote a manifest
        # of 121 files that were all css/ -- a verifier over none of the thing
        # it exists to verify, which would have gone green forever.
        p = impl.get("doc")
        if not p:
            continue
        paths.append(os.path.relpath(p, root) if os.path.isabs(p) or
                     p.startswith(root) else p)
    return paths


def scan_docs(root):
    """Fallback: every index.html under frameworks/, plus css/.

    Explicitly announced by the caller, never silent -- see --docs-from-scan."""
    hits = []
    fw = os.path.join(root, "frameworks")
    for base, dirs, files in os.walk(fw):
        dirs[:] = [d for d in dirs if d != "node_modules"]
        for f in files:
            if f == "index.html":
                hits.append(os.path.relpath(os.path.join(base, f), root))
    return hits


def shared(root):
    """The css/ tree the harness serves to every implementation. A change here
    moves every row at once, which is the drift most likely to be read as a
    browser regression."""
    out = []
    d = os.path.join(root, "css")
    for base, _dirs, files in os.walk(d):
        for f in files:
            out.append(os.path.relpath(os.path.join(base, f), root))
    return out


def digest(root, rel):
    p = os.path.join(root, rel)
    try:
        with open(p, "rb") as fh:
            return hashlib.sha256(fh.read()).hexdigest()
    except OSError:
        return None


def build(root, use_scan):
    docs = None if use_scan else documents(root)
    how = "jsfb_corpus"
    if docs is None:
        if not use_scan:
            return None, None
        docs = scan_docs(root)
        how = "scan (index.html under frameworks/, node_modules excluded)"
    rels = sorted(set(list(docs) + shared(root)))
    files = {}
    for rel in rels:
        h = digest(root, rel)
        if h:
            files[rel] = h
    rev = ""
    try:
        with open(os.path.join(root, "JSFB_REVISION")) as fh:
            rev = fh.read().strip()
    except OSError:
        pass
    return {"revision": rev, "documents_from": how, "files": files}, how


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="build/jsfb")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--docs-from-scan", action="store_true",
                    help="derive the document set by scanning instead of asking "
                         "jsfb_corpus -- the set may differ from what the matrix "
                         "reads, and the manifest records that it was used")
    a = ap.parse_args(argv[1:])

    if not os.path.isdir(a.root):
        print("jsfb-manifest: SKIP -- no corpus at %s" % a.root)
        print("               settle it with: make jsfb-fetch")
        return 0

    mpath = os.path.join(a.root, MANIFEST)

    if a.write:
        m, how = build(a.root, a.docs_from_scan)
        if m is None:
            return 1
        with open(mpath, "w") as fh:
            json.dump(m, fh, indent=1, sort_keys=True)
        print("jsfb-manifest: recorded %d files (%s), revision %s"
              % (len(m["files"]), how, m["revision"] or "unknown"))
        return 0

    if a.check:
        if not os.path.exists(mpath):
            # NOT a pass. An absent manifest means nobody can tell whether the
            # corpus is upstream's, and reporting that as ok is the exact shape
            # of test-url printing 32/32 over two absent corpora.
            print("jsfb-manifest: CANNOT VERIFY -- no %s in %s" % (MANIFEST, a.root))
            print("               An unverified corpus is not a verified one.")
            print("               settle it with: make jsfb-manifest")
            return 1
        with open(mpath) as fh:
            old = json.load(fh)
        new, _how = build(a.root, a.docs_from_scan or
                          old.get("documents_from", "").startswith("scan"))
        if new is None:
            return 1
        o, n = old["files"], new["files"]
        mod = sorted(k for k in o if k in n and o[k] != n[k])
        add = sorted(k for k in n if k not in o)
        rem = sorted(k for k in o if k not in n)
        if not (mod or add or rem):
            print("jsfb-manifest: ok -- %d files match the recorded corpus "
                  "(revision %s)" % (len(n), new["revision"] or "unknown"))
            return 0
        print("jsfb-manifest: THE CORPUS HAS DRIFTED. Any number measured "
              "against it describes a corpus nobody published.")
        for k in mod:
            print("   MODIFIED %s" % k)
        for k in add:
            print("   ADDED    %s" % k)
        for k in rem:
            print("   REMOVED  %s" % k)
        print("   A previous session left an injected `__retain` prelude in "
              "four documents here and a later run inherited it silently,")
        print("   disagreeing with the published matrix. That is what this "
              "check exists for.")
        print("   To take upstream's bytes back:  make jsfb-fetch && make jsfb-manifest")
        return 2

    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
