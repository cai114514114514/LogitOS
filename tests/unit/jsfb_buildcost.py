#!/usr/bin/env python3
"""jsfb_buildcost.py -- what does it COST to make a js-framework-benchmark
implementation runnable, measured rather than estimated.

  python3 tests/unit/jsfb_buildcost.py keyed/react-hooks keyed/svelte ...
  python3 tests/unit/jsfb_buildcost.py --survey        # no builds, static only
  python3 tests/unit/jsfb_buildcost.py --refs          # where to root a server
  python3 tests/unit/jsfb_buildcost.py --json out.json

THE ANSWER, MEASURED 2026-08-28 ON THIS HOST -- 31 implementations actually
built, 29 ok, 2 FAILED, and it is much cheaper than the question implies.

  31 builds        65.4 s wall for 30 of them in two batches, under a host
                   load average of 19-28, warm npm cache
  -> the 199       ~7 min wall, ~11 GB of node_modules if every tree is kept
                   at once, ~14 MB of dist if node_modules is pruned after
                   each build (peak then ~200 MB, the largest single tree)
  cold cache       measured with `npm ci --cache <fresh dir>`: 2.1-2.2x the
                   warm install. 8,482 unique (name,version) tarballs across
                   all 229 lockfiles against 35,787 declared -- a 4.2x
                   duplication, so a shared cache is warm for most of the run
                   after the first fifty or so. ~0.3-0.6 GB downloaded once.

BOTH FAILURES ARE ONE CAUSE AND IT IS NOT A FRAMEWORK. The four `ng`
implementations (angular-cf, angular-cf-new-nozone, angular-cf-signals,
angular-cf-signals-nozone) install cleanly and then `ng build` refuses:

    Node.js version v24.13.0 detected.
    The Angular CLI requires a minimum Node.js version of v22.22.3 or
    v24.15.0 or v26.0.0.

Settled rather than argued: the same build under node v24.20.0 exits 0 and
emits dist/angular/browser/index.html. NOTE THAT NO STATIC SCAN PREDICTS THIS
-- only 4 of 229 package.json files declare `engines` at all and angular-cf is
not one of them; the constraint is enforced at runtime by the CLI. So the
"29 of 31 build" rate is a MEASURED rate over a stratified sample, not a
property anyone can read off the corpus, and the 168 implementations nobody
has built here could hold more of the same shape.

Upstream's own .nvmrc says 20.9.0, which is BELOW the Angular floor too: there
is no single node version in the corpus's own documentation that builds all of
it. That is the corpus's problem, not this tree's, and it is the reason this
file reports a failure as a row rather than aborting a run.

WHY THIS EXISTS. 30 of the 229 implementations in build/jsfb run with no build
step; 199 need `npm install && npm run build-prod`, and every mainstream
framework a reader would look for -- React, Vue, Svelte, Solid, Preact, Lit,
Alpine, jQuery -- is in the 199. Whether those 199 are reachable is a cost
question, and a cost question answered by estimate is an opinion.

WHAT IS AND IS NOT TRUSTWORTHY IN THIS OUTPUT.

  Wall time is the WEAKEST number here and it is reported with the load
  average that was in force, because this host is shared -- CLAUDE.md's
  tools/perf/ rule is "the host is contended, other agents run QEMU
  concurrently, so host wall clock is worthless here". A build time from this
  script is an order of magnitude, not a measurement. Two runs will differ.

  Bytes and counts are STRONG: node_modules size, package count, dist size and
  whether index.html was emitted do not depend on host load at all. Prefer
  them. They are what the projection at the end is computed from.

  npm's own network cache makes the SECOND run of anything much faster than
  the first. `--cold` clears nothing (it must not: clearing a shared user-level
  npm cache would corrupt other work on this host); instead every row records
  whether the cache was already warm for that implementation, so a fast row
  cannot be read as a cheap one.

SECURITY, AND IT IS NOT THEORETICAL. Upstream's own README opens with it:
"`npm ci` and `npm install` can execute arbitrary commands, so they should be
executed only for packages you trust. Consequently I build on a dedicated
virtual private linux server such that I don't have to install the packages
for all those implementations on my laptop." The author of the corpus declines
to run these installs on his own machine. Building all 199 here runs the
postinstall scripts of 199 independent dependency trees on a developer host.
This script therefore builds ONLY what it is named on the command line, never
a wildcard, and prints the count it is about to install before doing it.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))   # tests/unit/x.py -> repo
CORPUS = os.environ.get("JSFB_ROOT", os.path.join(ROOT, "build", "jsfb"))

sys.path.insert(0, os.path.join(ROOT, "tests", "unit"))
import jsfb_corpus                                          # noqa: E402

# One read of the corpus, reused. jsfb_corpus.impls() walks every package.json,
# so calling it per-implementation inside a loop would re-read 229 files each
# time for a number that cannot change during a run.
_ROWS = None


def _corpus_row(impl_dir):
    global _ROWS
    if _ROWS is None:
        _ROWS = {os.path.abspath(i["dir"]): i
                 for i in jsfb_corpus.impls(CORPUS)}
    return _ROWS.get(os.path.abspath(impl_dir))


def _corpus_reset():
    """A build CREATES the document, so the cached corpus read is stale the
    moment `npm run build-prod` returns. Anything that builds must drop it."""
    global _ROWS
    _ROWS = None


def loadavg():
    try:
        return os.getloadavg()[0]
    except OSError:
        return -1.0


def du_bytes(path):
    """Bytes on disk under path. Walks rather than shelling out to du, because
    BSD and GNU du disagree about both units and the meaning of -s."""
    if not os.path.isdir(path):
        return 0
    n = 0
    for dirpath, dirnames, filenames in os.walk(path):
        for f in filenames:
            fp = os.path.join(dirpath, f)
            try:
                n += os.lstat(fp).st_size
            except OSError:
                pass
    return n


def count_packages(nm):
    """Installed packages = directories holding a package.json under
    node_modules, including scoped and nested ones. Counting top-level entries
    undercounts badly once npm hoists."""
    if not os.path.isdir(nm):
        return 0
    n = 0
    for dirpath, dirnames, filenames in os.walk(nm):
        if "package.json" in filenames:
            n += 1
            # do not descend past a package's own node_modules? we DO want
            # nested ones, so keep walking.
    return n


def lock_declared_packages(impl_dir):
    """How many packages the lockfile PROMISES, without installing anything.
    This is the load-independent predictor the projection is built on."""
    lf = os.path.join(impl_dir, "package-lock.json")
    if not os.path.exists(lf):
        return None
    try:
        j = json.load(open(lf, encoding="utf8"))
    except Exception:
        return None
    if "packages" in j:
        # lockfileVersion 2/3: keys are paths, "" is the root project itself.
        return len([k for k in j["packages"] if k])
    if "dependencies" in j:
        n = 0

        def walk(d):
            nonlocal n
            for _, v in d.items():
                n += 1
                if isinstance(v, dict) and "dependencies" in v:
                    walk(v["dependencies"])

        walk(j["dependencies"])
        return n
    return 0


def run(cmd, cwd, timeout):
    t0 = time.time()
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                           timeout=timeout)
        return time.time() - t0, p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return time.time() - t0, -9, "", "TIMEOUT after %ds" % timeout


# AN UNQUOTED ATTRIBUTE VALUE IS LEGAL HTML AND EVERY MINIFIER EMITS ONE.
# The first version of this pattern was `["']([^"']+)["']` -- quotes required.
# It read keyed/marko's built document, which opens
#     <link href=/css/currentStyle.css rel=stylesheet>
# and reported that marko references the shared stylesheet ZERO times. The
# document is not unusual; it is the ordinary output of an HTML minifier, and
# marko is simply the first implementation in this corpus whose toolchain runs
# one. A regex that cannot see an unquoted value does not report a smaller
# number, it reports a WRONG one -- and it reports it as a clean zero, which is
# the shape CLAUDE.md's first rule is about.
EXT_RE = re.compile(
    r"""(?:src|href)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'<>`]+))""", re.I)


def external_refs(impl_dir, entry_html, corpus_rel=None, corpus_root=None):
    """Every URL the entry document reaches for, in four buckets. The buckets
    are the question "where must a static file server be rooted?", because
    that is the only thing this answer is used for.

      relative      -- resolves next to the document. Root the server anywhere.
      abs_self      -- an ABSOLUTE url that points back inside this
                       implementation's own directory, e.g.
                       /frameworks/keyed/vue/dist/assets/index-BDSB6a9M.js.
                       Vite emits these. They are not "external" -- the bytes
                       are local -- but they resolve ONLY if the server root is
                       the CORPUS root, so they are the reason the root cannot
                       be the implementation directory.
      corpus_shared -- an absolute path that RESOLVES TO A FILE under the
                       corpus root and outside this implementation:
                       /css/currentStyle.css and, for four implementations,
                       /css/main.css and /css/bootstrap/... . Same requirement
                       as abs_self: root the server at the corpus.
      remote        -- http(s):// or protocol-relative, OR an absolute path
                       that resolves to nothing on disk. This is the only
                       bucket that needs the network or is simply broken, and
                       the only one that makes an implementation unservable
                       offline.

    THE THIRD BUCKET IS DECIDED BY os.path.exists, NOT BY A NAME. The previous
    version special-cased the substring "currentStyle.css" and put every other
    absolute path in `remote`, which classified keyed/mikado's
    /css/bootstrap/dist/css/bootstrap.min.css as needing a CDN. That file is
    in the checkout -- build/jsfb/css/bootstrap/dist/css/bootstrap.min.css --
    so the answer was wrong in the direction that matters: it claimed four
    implementations could not be served offline when all four can. Asking the
    filesystem is what makes "remote" mean remote. With no corpus_root the
    question cannot be answered, so nothing is claimed: absolute non-self paths
    go to `unresolved` rather than being guessed into either bucket.
    """
    if not entry_html or not os.path.exists(entry_html):
        return None
    html = open(entry_html, encoding="utf8", errors="replace").read()
    out = {"relative": [], "corpus_shared": [], "abs_self": [],
           "remote": [], "unresolved": []}
    prefix = ("/frameworks/%s/" % corpus_rel) if corpus_rel else None
    for m in EXT_RE.finditer(html):
        u = m.group(1) or m.group(2) or m.group(3) or ""
        if not u or u.startswith(("data:", "#", "mailto:", "javascript:")):
            continue
        if u.startswith(("http://", "https://", "//")):
            out["remote"].append(u)
        elif u.startswith("/"):
            if prefix and u.startswith(prefix):
                out["abs_self"].append(u)
            elif corpus_root is None:
                out["unresolved"].append(u)
            else:
                # strip a query/fragment before asking the filesystem
                p = u.split("?", 1)[0].split("#", 1)[0]
                on_disk = os.path.join(corpus_root, p.lstrip("/"))
                (out["corpus_shared"] if os.path.exists(on_disk)
                 else out["remote"]).append(u)
        else:
            out["relative"].append(u)
    return out


def find_entry(impl_dir):
    """Where the runnable document is -- ASKED, not guessed.

    THE FIRST VERSION OF THIS FUNCTION GUESSED, AND THE GUESS WAS WRONG.
    It tried "dist/index.html then index.html" and scored keyed/marko as a
    FAILURE: npm exited 0, a bundle was emitted, and the document was at
    dist/public/index.html because marko's package.json says
    `customURL: /dist/public`. The corpus DECLARES the path -- 39 of the 229
    implementations set it -- and tests/unit/jsfb_corpus.py is this tree's one
    reader of that declaration. Re-deriving it here would be the second door
    onto the jar, the shape this tree has paid for three times.

    Checked against the ten implementations built by hand: the delegating
    version and the guessing version agree on all ten, and the guessing version
    was additionally wrong about marko. A `dist/index.html` fallback is NOT
    kept as a safety net, because a fallback that fires would mean the
    implementation's own declaration disagrees with reality -- and silently
    papering over that is exactly how a corpus reader starts lying."""
    i = _corpus_row(impl_dir)
    if not i or not i.get("doc"):
        return None, None
    return os.path.relpath(i["doc"], impl_dir), i["doc"]


def survey(names):
    """Static facts only -- no install, no build, no network."""
    rows = []
    for name in names:
        d = os.path.join(CORPUS, "frameworks", name)
        pj = os.path.join(d, "package.json")
        if not os.path.exists(pj):
            continue
        j = json.load(open(pj, encoding="utf8"))
        bp = j.get("scripts", {}).get("build-prod", "")
        row = _corpus_row(d)
        # "needs a build" is jsfb_corpus's judgement, not a second copy of it.
        needs = not (row and row["build_free"])
        ent, entp = find_entry(d)
        rows.append({
            "name": name,
            "build_prod": bp,
            "needs_build": needs,
            "toolchain": bp.split()[0] if bp else "",
            "lock_packages": lock_declared_packages(d),
            "has_lockfile": os.path.exists(os.path.join(d, "package-lock.json")),
            "src_index_html": os.path.exists(os.path.join(d, "index.html")),
            "entry": ent,
            "src_bytes": du_bytes(d) - du_bytes(os.path.join(d, "node_modules")),
        })
    return rows


def build_one(name, timeout, use_ci):
    d = os.path.join(CORPUS, "frameworks", name)
    nm = os.path.join(d, "node_modules")
    dist = os.path.join(d, "dist")
    r = {"name": name}
    pj = json.load(open(os.path.join(d, "package.json"), encoding="utf8"))
    r["build_prod"] = pj.get("scripts", {}).get("build-prod", "")
    r["lock_packages"] = lock_declared_packages(d)
    r["had_node_modules"] = os.path.isdir(nm)
    r["index_html_before"] = os.path.exists(os.path.join(d, "index.html"))
    r["dist_before"] = os.path.isdir(dist)

    have_lock = os.path.exists(os.path.join(d, "package-lock.json"))
    inst = ["npm", "ci"] if (use_ci and have_lock) else ["npm", "install"]
    r["install_cmd"] = " ".join(inst)

    r["load_before"] = round(loadavg(), 2)
    dt, rc, out, err = run(inst, d, timeout)
    r["install_s"] = round(dt, 1)
    r["install_rc"] = rc
    r["install_tail"] = (err or out)[-600:] if rc != 0 else ""
    r["node_modules_bytes"] = du_bytes(nm)
    r["node_modules_packages"] = count_packages(nm)

    if rc != 0:
        r["ok"] = False
        r["failed_at"] = "install"
        r["load_after"] = round(loadavg(), 2)
        return r

    dt, rc, out, err = run(["npm", "run", "build-prod"], d, timeout)
    r["build_s"] = round(dt, 1)
    r["build_rc"] = rc
    r["build_tail"] = (err or out)[-600:] if rc != 0 else ""
    r["load_after"] = round(loadavg(), 2)

    r["dist_bytes"] = du_bytes(dist)
    _corpus_reset()          # the build just created the document
    ent, entp = find_entry(d)
    r["entry"] = ent
    # "the build produced the document" = the entry is not the source-root
    # index.html and there was no build output directory beforehand.
    r["entry_emitted_by_build"] = bool(
        ent and ent != "index.html" and not r["dist_before"])
    r["refs"] = external_refs(d, entp, corpus_rel=name, corpus_root=CORPUS)
    # The bar is not "npm exited 0". It is "there is a document to open".
    r["ok"] = (rc == 0 and ent is not None)
    if not r["ok"]:
        r["failed_at"] = "build" if rc != 0 else "no-entry-document"
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("impls", nargs="*",
                    help="e.g. keyed/react-hooks (explicit; never a wildcard)")
    ap.add_argument("--survey", action="store_true",
                    help="static facts for the WHOLE corpus, no install/build")
    ap.add_argument("--refs", action="store_true",
                    help="where must the file server be rooted: bucket every "
                         "URL of every document PRESENT on disk")
    ap.add_argument("--json", metavar="PATH")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--install", choices=("ci", "install"), default="ci")
    a = ap.parse_args()

    if not os.path.isdir(CORPUS):
        print("jsfb-buildcost: no corpus at %s" % CORPUS, file=sys.stderr)
        print("  settle it with: bash tools/jsfb_fetch.sh", file=sys.stderr)
        return 2

    if a.refs:
        # DOCUMENTS PRESENT ON DISK, not implementations. An implementation
        # that has never been built has no document, so it contributes nothing
        # here and must not be counted as "clean" -- the denominator printed
        # below is the number of documents examined, and it moves as builds
        # land. Reporting it against 229 would let an unbuilt corpus read as a
        # corpus with no remote references in it.
        rows = []
        for i in jsfb_corpus.impls(CORPUS):
            if not i.get("doc"):
                continue
            rel = os.path.relpath(i["dir"], os.path.join(CORPUS, "frameworks"))
            f = external_refs(i["dir"], i["doc"], corpus_rel=rel,
                              corpus_root=CORPUS)
            if f is None:
                continue
            # DO THE REFERENCED BYTES EXIST YET? A source index.html that
            # says <script src="dist/main.js"> is a perfectly local document
            # and it is NOT runnable until the build has emitted that file.
            # Bucketing URLs by origin answers "where must the server be
            # rooted"; it does not answer "is there anything to serve", and
            # conflating the two would let an entirely unbuilt corpus report
            # as clean. Every local reference is therefore stat'd.
            missing = []
            for u in f["relative"]:
                p = u.split("?", 1)[0].split("#", 1)[0]
                if not os.path.exists(os.path.join(os.path.dirname(i["doc"]),
                                                   p)):
                    missing.append(u)
            for u in f["abs_self"] + f["corpus_shared"]:
                p = u.split("?", 1)[0].split("#", 1)[0]
                if not os.path.exists(os.path.join(CORPUS, p.lstrip("/"))):
                    missing.append(u)
            rows.append({"name": i["name"], "doc": os.path.relpath(i["doc"],
                                                                   CORPUS),
                         "refs": f, "missing": missing})
        nrem = [r for r in rows if r["refs"]["remote"]]
        nabs = [r for r in rows if r["refs"]["abs_self"]]
        nsh = [r for r in rows if r["refs"]["corpus_shared"]]
        print("jsfb-buildcost refs: %d documents present of %d implementations"
              % (len(rows), len(list(jsfb_corpus.impls(CORPUS)))))
        print("  %d reference something REMOTE (needs network, or resolves to "
              "nothing on disk)" % len(nrem))
        for r in nrem:
            print("    %-28s %s" % (r["name"], r["refs"]["remote"]))
        print("  %d use an ABSOLUTE path into their own directory "
              "(server root must be the corpus root)" % len(nabs))
        print("  %d reference a corpus-shared file under /css/" % len(nsh))
        nmiss = [r for r in rows if r["missing"]]
        print("  %d have every referenced local file ON DISK (complete); "
              "%d are missing at least one (not built yet)"
              % (len(rows) - len(nmiss), len(nmiss)))
        print("  => root a static server at %s; no document in this corpus "
              "reaches outside it" % CORPUS)
        if a.json:
            json.dump(rows, open(a.json, "w"), indent=1)
            print("  wrote %s" % a.json)
        return 0

    if a.survey:
        import glob
        names = sorted(
            os.path.relpath(os.path.dirname(p),
                            os.path.join(CORPUS, "frameworks"))
            for p in glob.glob(os.path.join(CORPUS, "frameworks",
                                            "*", "*", "package.json")))
        rows = survey(names)
        need = [r for r in rows if r["needs_build"]]
        free = [r for r in rows if not r["needs_build"]]
        print("jsfb-buildcost survey: %d implementations, %d need a build, "
              "%d do not" % (len(rows), len(need), len(free)))
        nolock = [r for r in need if not r["has_lockfile"]]
        noidx = [r for r in need if not r["src_index_html"]]
        print("  of the %d that need a build: %d have no package-lock.json "
              "(npm ci impossible), %d ship no index.html at all "
              "(the build emits it)" % (len(need), len(nolock), len(noidx)))
        tc = {}
        for r in need:
            tc[r["toolchain"]] = tc.get(r["toolchain"], 0) + 1
        print("  toolchains: " + ", ".join(
            "%s=%d" % kv for kv in sorted(tc.items(), key=lambda x: -x[1])[:12]))
        locks = [r["lock_packages"] for r in need if r["lock_packages"]]
        if locks:
            locks.sort()
            print("  lockfile-declared packages across the %d: min=%d "
                  "median=%d mean=%d max=%d total=%d"
                  % (len(locks), locks[0], locks[len(locks) // 2],
                     sum(locks) // len(locks), locks[-1], sum(locks)))
        if a.json:
            json.dump(rows, open(a.json, "w"), indent=1)
            print("  wrote %s" % a.json)
        return 0

    if not a.impls:
        ap.error("name implementations explicitly, or pass --survey")

    print("jsfb-buildcost: about to run '%s' for %d implementation(s). "
          "Upstream's README warns these execute arbitrary code."
          % (a.install, len(a.impls)))
    rows = []
    for n in a.impls:
        print("  building %s ..." % n, flush=True)
        r = build_one(n, a.timeout, a.install == "ci")
        rows.append(r)
        print("    %-28s %s  install %ss / %s pkgs / %.1f MB   build %ss   "
              "dist %.2f MB  entry=%s"
              % (n, "ok " if r.get("ok") else "FAIL",
                 r.get("install_s"), r.get("node_modules_packages"),
                 r.get("node_modules_bytes", 0) / 1e6,
                 r.get("build_s", "-"), r.get("dist_bytes", 0) / 1e6,
                 r.get("entry")), flush=True)
        if not r.get("ok"):
            print("      failed_at=%s\n      %s"
                  % (r.get("failed_at"),
                     (r.get("install_tail") or r.get("build_tail",
                                                     ""))[-400:]))
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)
        print("wrote %s" % a.json)
    return 0 if all(r.get("ok") for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
