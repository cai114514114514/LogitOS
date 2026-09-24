#!/usr/bin/env python3
"""The Fluid release's userland tools, diffed against the HOST's own tools.

Every tool in c/apps/coreutils/text/ is written against POSIX and mini-libc, so
the same source builds natively here. Each case in cases.txt is one shell
snippet; it runs twice, in two fresh copies of fixture/, with PATH led by

    ours  -- the tools under test, built natively (tests/textutils.mk)
    ref   -- the host's tools of the same names

and the two runs must agree on stdout and on exit status. The reference is
somebody else's implementation, never an expectation this file wrote down --
the same arrangement tests/libc.mk uses against glibc.

A case line is `TAG  SNIPPET`:

    posix  the host's plain tool is the reference (/usr/bin, /bin)
    gnu    GNU behaviour is the reference: `g`-prefixed tools (brew coreutils,
           gnu-sed, grep) stand in; without them the case SKIPS, loudly, with
           the command that would settle it -- never passes silently
    bsd    kept for cases where BSD is the reference on purpose

Output is compared with LC_ALL=C. `#` starts a comment line.

Usage: run_host.py OURS_DIR [--only TOOL]
"""
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "fixture")
CASES = os.path.join(HERE, "cases.txt")


def tools_in(ours):
    return sorted(n for n in os.listdir(ours) if os.access(os.path.join(ours, n), os.X_OK))


def ref_dir(tag, tool, names, tmp):
    """A directory of links to the host's tools of these names, or (None, why).

    For a `gnu` case only the TOOL UNDER TEST must be GNU (the g-prefixed
    build); any other tool the snippet uses is the host's own, as in a posix
    case."""
    d = os.path.join(tmp, "ref-%s-%s" % (tag, tool))
    os.makedirs(d, exist_ok=True)
    for n in names:
        want_gnu = tag == "gnu" and n == tool
        found = None
        bases = (["/opt/homebrew/bin", "/usr/local/bin"] if want_gnu else []) + \
                ["/usr/bin", "/bin", "/usr/sbin", "/sbin"]
        for base in bases:
            p = os.path.join(base, ("g" + n) if want_gnu else n)
            if os.access(p, os.X_OK):
                found = p
                break
        if not found:
            if want_gnu:
                return None, "g%s not found (brew install coreutils gnu-sed grep diffutils findutils)" % n
            continue
        dst = os.path.join(d, n)
        if not os.path.exists(dst):
            os.symlink(found, dst)
    return d, None


def run(snippet, lead, tmp, label):
    work = os.path.join(tmp, label)
    if os.path.exists(work):
        shutil.rmtree(work)
    shutil.copytree(FIXTURE, work, symlinks=True)
    env = dict(os.environ)
    env["PATH"] = lead + ":/usr/bin:/bin:/usr/sbin:/sbin"
    env["LC_ALL"] = "C"
    p = subprocess.run(["/bin/sh", "-c", snippet], cwd=work, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    return p.returncode, p.stdout


def cases():
    """(line number, tag, snippet). A snippet may span lines: it continues
    until its quotes balance, so a sed script with embedded newlines is one
    case (e.g. `:a` loops and POSIX `a\\` text)."""
    lines = open(CASES, encoding="utf-8").read().split("\n")
    i = 0
    while i < len(lines):
        line, ln = lines[i], i + 1
        i += 1
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        while True:
            try:
                shlex.split(line)
                break
            except ValueError:
                if i >= len(lines):
                    break
                line += "\n" + lines[i]
                i += 1
        tag, snippet = line.split(None, 1)
        yield ln, tag, snippet


def words(snippet):
    """Command words of a snippet: the first word of each pipeline stage."""
    out = []
    try:
        toks = shlex.split(snippet, posix=True)
    except ValueError:
        return out
    start = True
    for t in toks:
        if start:
            out.append(t)
            start = False
        if t in ("|", "&&", "||", ";") or t.endswith(";"):
            start = True
    # A stage glued to a separator ("a|b" in quotes is not one) -- catch the
    # common "cmd | tool" spacing as well.
    for part in snippet.replace("&&", "|").replace(";", "|").split("|"):
        w = part.split()
        if w:
            out.append(w[0])
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    ours = os.path.abspath(argv[1])
    only = argv[argv.index("--only") + 1] if "--only" in argv else None
    names = tools_in(ours)
    tmp = tempfile.mkdtemp(prefix="textutils-")
    refs = {}
    passed = failed = skipped = 0
    try:
        for ln, tag, snippet in cases():
            tool = next((w for w in words(snippet) if w in names), None)
            if only and tool != only:
                continue
            if tool is None:
                print("FAIL %s:%d runs none of the tools built (%s): %s"
                      % (os.path.basename(CASES), ln, " ".join(names), snippet))
                failed += 1
                continue
            key = (tag, tool)
            if key not in refs:
                refs[key] = ref_dir(tag, tool, names, tmp)
            rdir, why = refs[key]
            if rdir is None:
                print("SKIP [%s] %s -- %s" % (tag, snippet, why))
                skipped += 1
                continue
            rc_o, out_o = run(snippet, ours, tmp, "ours")
            rc_r, out_r = run(snippet, rdir, tmp, "ref")
            if rc_o == rc_r and out_o == out_r:
                passed += 1
            else:
                failed += 1
                print("FAIL [%s:%s] %s" % (tag, tool, snippet))
                if rc_o != rc_r:
                    print("     exit: ours %d, reference %d" % (rc_o, rc_r))
                if out_o != out_r:
                    print("     ours:      %r" % out_o[:400])
                    print("     reference: %r" % out_r[:400])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("textutils-host: %d passed, %d failed, %d skipped" % (passed, failed, skipped))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
