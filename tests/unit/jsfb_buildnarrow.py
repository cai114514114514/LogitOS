#!/usr/bin/env python3
"""jsfb_buildnarrow.py -- build a NAMED set of js-framework-benchmark
implementations and report, per implementation, whether there is a document to
open afterwards.

    python3 tests/unit/jsfb_buildnarrow.py --list-file tests/jsfb/NARROW
    python3 tests/unit/jsfb_buildnarrow.py keyed/react-hooks keyed/vue ...
    ... --node /path/to/node/bin       prepend a node toolchain to PATH
    ... --install ci|none              default `none`: build only, node_modules
                                       must already be there
    ... --json out.json

WHY THIS IS NOT tests/unit/jsfb_buildcost.py.  That file answers "what does it
COST to make an implementation runnable" and always runs `npm ci` first, which
is the right shape for a cost question and the wrong shape for this one: on a
host where node_modules is already installed, `npm ci` deletes and reinstalls
it, so a two-second question becomes a two-minute one and 27 of them become an
hour.  Everything about WHERE THE DOCUMENT IS is still asked of
tests/unit/jsfb_corpus.py -- the one reader -- exactly as buildcost does, so
this adds no second door onto that jar.

WHAT IT IS FOR, AND IT IS AN APPARATUS CHECK BEFORE IT IS A BUILD.
An implementation directory that already holds a `dist/` reads as built.  It is
not evidence of one: a build that failed half way leaves exactly the same
directory listing as a build that succeeded, and the entry document's <script
src> resolving on disk cannot tell them apart.  This runs `npm run build-prod`
for each named implementation and records the exit status beside the document
path, so "there is a document" and "a build produced it, today, and exited 0"
are two separate columns instead of one guess.

A BUILD FAILURE IS NOT A BROWSER FINDING, and this file exists partly to keep
those apart.  Every row carries `ok`, `rc` and the first line of the error, and
the caller is expected to exclude a failed row from the matrix by name rather
than to let it appear there as a red row -- which would be a claim about this
browser made out of somebody's node version.  jsfb_buildcost.py's header
already records the live example: the four `ng` implementations refuse to build
below Angular's node floor, and nothing static predicts it.
"""

import argparse
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORPUS = os.environ.get("JSFB_ROOT", os.path.join(ROOT, "build", "jsfb"))

sys.path.insert(0, os.path.join(ROOT, "tests", "unit"))
import jsfb_corpus                                             # noqa: E402


def row_of(name):
    """This implementation's corpus row, re-read every time, because a build
    CREATES the document and a cached answer taken before the build is
    guaranteed to be the stale one."""
    for i in jsfb_corpus.impls(CORPUS):
        if i["name"] == name:
            return i
    return None


def doc_of(name):
    i = row_of(name)
    return i["doc"] if i else None


def build_one(name, env, timeout, install):
    d = os.path.join(CORPUS, "frameworks", name)
    r = {"name": name, "dir": d}
    if not os.path.isfile(os.path.join(d, "package.json")):
        r.update(ok=False, rc=None, failed_at="absent",
                 error="no package.json under %s" % d)
        return r
    row = row_of(name)
    r["doc_before"] = row["doc"] if row else None
    r["node_modules_before"] = os.path.isdir(os.path.join(d, "node_modules"))

    # BUILD-FREE IS ASKED, NOT ASSUMED FROM THE NAME. Four of the names this is
    # run with need no build at all, and `npm run build-prod` on one of them
    # exits 1 with "Missing script" -- which would enter the report as a build
    # failure of an implementation that has nothing to build. jsfb_corpus is the
    # one reader of that property (absent / `exit 0` / an `echo`), and asking it
    # is what keeps this correct when upstream changes a script.
    if row and row["build_free"]:
        r.update(ok=bool(row["doc"]), rc=None, skipped="build-free",
                 doc_after=row["doc"], build_s=0.0)
        if not row["doc"]:
            r.update(ok=False, failed_at="no-entry-document",
                     error="build-free but ships no document")
        return r

    if install == "ci":
        cmd = ["npm", "ci"] if os.path.exists(os.path.join(d, "package-lock.json")) \
            else ["npm", "install"]
        t0 = time.time()
        p = subprocess.run(cmd, cwd=d, env=env, capture_output=True, text=True,
                           errors="replace", timeout=timeout)
        r["install_s"] = round(time.time() - t0, 1)
        r["install_rc"] = p.returncode
        if p.returncode != 0:
            r.update(ok=False, rc=p.returncode, failed_at="install",
                     error=_first_error(p.stderr or p.stdout),
                     tail=(p.stderr or p.stdout)[-800:])
            return r

    t0 = time.time()
    try:
        p = subprocess.run(["npm", "run", "build-prod"], cwd=d, env=env,
                           capture_output=True, text=True, errors="replace",
                           timeout=timeout)
        rc, out, err = p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        rc = -9
        out = (e.stdout or b"").decode("utf8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        err = "TIMEOUT after %ds" % timeout
    r["build_s"] = round(time.time() - t0, 1)
    r["rc"] = rc
    # The corpus read is stale the moment build-prod returns.
    jsfb_corpus_cache_reset()
    r["doc_after"] = doc_of(name)
    # THE BAR IS NOT "npm exited 0". It is "npm exited 0 AND there is a document
    # to open." buildcost states the same rule; a build that exits 0 and emits
    # nothing is not a runnable implementation.
    r["ok"] = (rc == 0 and r["doc_after"] is not None)
    if not r["ok"]:
        r["failed_at"] = "build" if rc != 0 else "no-entry-document"
        r["error"] = _first_error(err or out)
        r["tail"] = (err or out)[-800:]
    return r


def jsfb_corpus_cache_reset():
    """jsfb_corpus.impls() re-walks the corpus on every call and holds no cache,
    so there is nothing to drop. The call is kept as the place a cache would
    have to be dropped if one is ever added -- the failure it would cause is
    silent (every doc_after equal to its doc_before) and would read as "no build
    produced anything"."""
    return


def _first_error(text):
    """The first line that looks like the reason, not the first line of output.
    npm prefixes several lines of its own before a tool ever speaks."""
    lines = [l.rstrip() for l in (text or "").split("\n") if l.strip()]
    for l in lines:
        s = l.strip()
        if s.startswith(("npm warn", "npm notice", ">", "npm WARN")):
            continue
        return s[:300]
    return lines[0][:300] if lines else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("impls", nargs="*")
    ap.add_argument("--list-file", help="one implementation name per line; # comments")
    ap.add_argument("--node", help="a node bin/ directory to put FIRST on PATH")
    ap.add_argument("--install", choices=("ci", "none"), default="none")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--json", metavar="PATH")
    a = ap.parse_args()

    names = list(a.impls)
    if a.list_file:
        with open(a.list_file) as f:
            names += [l.split("#", 1)[0].strip() for l in f]
    names = [n for n in names if n]
    if not names:
        ap.error("name implementations, or pass --list-file")

    if not os.path.isdir(CORPUS):
        print("jsfb-buildnarrow: no corpus at %s" % CORPUS, file=sys.stderr)
        print("  settle it with: make jsfb-fetch", file=sys.stderr)
        return 2

    env = dict(os.environ)
    if a.node:
        env["PATH"] = os.path.abspath(a.node) + os.pathsep + env.get("PATH", "")
    nv = subprocess.run(["node", "--version"], env=env, capture_output=True,
                        text=True)
    print("jsfb-buildnarrow: node %s, npm run build-prod for %d implementation(s), "
          "install=%s" % (nv.stdout.strip() or "?", len(names), a.install))
    print("  upstream's own README warns these execute arbitrary code.")

    rows = []
    for n in names:
        print("  %-28s ..." % n, end="", flush=True)
        r = build_one(n, env, a.timeout, a.install)
        rows.append(r)
        print(" %-4s rc=%-4s %5ss  doc=%s"
              % (("ok" if r.get("ok") else "FAIL")
                 + ("*" if r.get("skipped") else ""), r.get("rc"),
                 r.get("build_s", "-"),
                 os.path.relpath(r["doc_after"], CORPUS) if r.get("doc_after")
                 else "(none)"), flush=True)
        if not r.get("ok"):
            print("      failed_at=%s  %s" % (r.get("failed_at"), r.get("error")))

    ok = [r for r in rows if r.get("ok") and not r.get("skipped")]
    skip = [r for r in rows if r.get("ok") and r.get("skipped")]
    bad = [r for r in rows if not r.get("ok")]
    print()
    print("jsfb-buildnarrow: %d built, %d build-free (nothing to build, marked *), "
          "%d FAILED TO BUILD" % (len(ok), len(skip), len(bad)))
    if bad:
        print("  A BUILD FAILURE IS NOT A BROWSER FINDING. These must be excluded")
        print("  from the matrix by name, not carried into it as red rows:")
        for r in bad:
            print("    %-28s %-18s %s" % (r["name"], r.get("failed_at"),
                                          r.get("error", "")[:90]))
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)
        print("  wrote %s" % a.json)
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
