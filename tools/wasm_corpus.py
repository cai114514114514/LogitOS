#!/usr/bin/env python3
"""Turn the official WebAssembly spec test suite into a flat corpus of binary
modules plus a manifest saying, for each one, WHAT THE SPEC SAYS MUST HAPPEN.

THE POINT OF THIS FILE IS THE NEGATIVE HALF.  The suite's value here is not
its valid modules -- it is the thousands of MALFORMED and INVALID ones, each
written by somebody who is not us, each of which a conformant implementation
must REJECT.  A validator that accepts a malformed module is the only failure
in this phase that matters, because everything downstream trusts it.

TWO MODES, AND THE MANIFEST SAYS WHICH ONE PRODUCED IT.

  binary-only (no external tool, always available)
      Extracts every `(module binary "...")` form out of the .wast text.
      That is the whole of binary.wast, binary-leb128.wast, custom.wast and
      the four utf8-*.wast files -- i.e. essentially all of the MALFORMED
      corpus, which is the half that is the control.

  wast2json (wabt; a superset)
      Adds the assert_invalid cases whose module is written in the TEXT
      format, which is most of them: block.wast alone is 155, if.wast 92,
      unreached-invalid.wast 121.  Those are what exercise the type checker
      rather than the decoder, so this mode is worth having.

      AND IT DROPS FILES WHOLE.  wabt 1.0.36 refuses an entire .wast over one
      form it cannot parse -- 54 of the pinned suite's 257 files, including
      unreached-invalid.wast, which is the corpus for the polymorphic stack.
      That was silent until 2026-08-30.  See `convert_prefix` below: a refused
      file is now converted up to its first error (110 of unreached-invalid's
      121 cases come back) and its discarded tail is swept by the binary-only
      extractor, and the manifest records which files went that way.

The manifest is TSV, one case per line:

      kind <TAB> file <TAB> origin <TAB> expected-text

kind is accept | malformed | invalid.  `origin` is `foo.wast:LINE`, so a
failure names the assertion a person can go read.

A SECOND MANIFEST, runs.tsv, CARRIES THE EXECUTION HALF: assert_return,
assert_trap and assert_exhaustion, which are what a validator cannot be judged
by and an interpreter can be judged by almost entirely.  It is ordered and
stateful in the way the .wast itself is -- a `mod` row selects the instance
that every row after it acts on, exactly as a `(module ...)` form does -- and
that ordering is load-bearing: the same export name means a different function
in each of memory_grow.wast's four modules.

Values are carried as the BIT PATTERN in decimal, which is what wast2json
already emits, and never as a printed float.  A decimal float cannot
distinguish -0.0 from 0.0 nor one NaN payload from another, and both
distinctions are things this suite asserts.  The two NaN wildcards the suite
uses survive as `nanc` (canonical) and `nana` (arithmetic: any quiet NaN),
because the specification genuinely does not fix the payload there and a gate
that demanded an exact one would be asserting something untrue.

Export names are hex-encoded.  They are arbitrary UTF-8 in this suite --
names.wast exports a field containing a NUL and several containing tabs -- and
a TSV column cannot hold those.
"""
import json, os, re, shutil, subprocess, sys

# ---------------------------------------------------------------- lexer

def toks(text):
    """(, ), a string literal (as raw bytes), or an atom.  Comments dropped.

    The line number is carried incrementally rather than recomputed with
    text.count("\n", 0, i) per token -- that is O(n) inside an O(n) loop, and
    on a 700 KB .wast it is the difference between a second and a minute.  The
    first version of this file did it the quadratic way and it read as a hung
    process rather than as slow code."""
    i, n, out = 0, len(text), []
    line = 1
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            if c == "\n": line += 1
            i += 1
        elif text.startswith(";;", i):
            j = text.find("\n", i)
            if j < 0:
                i = n
            else:
                line += 1; i = j + 1
        elif text.startswith("(;", i):
            depth, i = 1, i + 2
            while i < n and depth:
                if text.startswith("(;", i): depth, i = depth + 1, i + 2
                elif text.startswith(";)", i): depth, i = depth - 1, i + 2
                else:
                    if text[i] == "\n": line += 1
                    i += 1
        elif c == "(" or c == ")":
            out.append(c); i += 1
        elif c == '"':
            start_line = line
            i += 1
            buf = bytearray()
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    e = text[i + 1]
                    if e == "n": buf.append(10); i += 2
                    elif e == "t": buf.append(9); i += 2
                    elif e == "r": buf.append(13); i += 2
                    elif e == "\\": buf.append(92); i += 2
                    elif e == '"': buf.append(34); i += 2
                    elif e == "'": buf.append(39); i += 2
                    elif e == "u":
                        j = text.index("}", i)
                        buf += chr(int(text[i + 3:j], 16)).encode("utf-8")
                        i = j + 1
                    else:
                        buf.append(int(text[i + 1:i + 3], 16)); i += 3
                else:
                    if text[i] == "\n": line += 1
                    buf += text[i].encode("utf-8"); i += 1
            i += 1
            out.append(("str", bytes(buf), start_line))
        else:
            j = i
            while j < n and text[j] not in ' \t\r\n()";':
                j += 1
            if j == i:
                # A lone ';' -- not ";;" and not ";)".  It is in the delimiter
                # set, so the atom scan stops before consuming it and the outer
                # loop never advances: an INFINITE LOOP that reads exactly like
                # a slow parser.  This cost half an hour; the symptom was a
                # 300-second timeout with no output, which is what sent the
                # first guess to "the lexer is quadratic" instead of "the lexer
                # is stuck".  Suspect the apparatus first.
                i += 1
                continue
            out.append(("atom", text[i:j], line))
            i = j
    return out

def parse(tk):
    """token list -> nested python lists of top-level forms"""
    pos = [0]
    def form():
        assert tk[pos[0]] == "("
        line = None
        pos[0] += 1
        items = []
        while tk[pos[0]] != ")":
            t = tk[pos[0]]
            if t == "(":
                items.append(form())
            else:
                if line is None: line = t[2]
                items.append(t); pos[0] += 1
        pos[0] += 1
        return (items, line)
    out = []
    while pos[0] < len(tk):
        if tk[pos[0]] == "(":
            out.append(form())
        else:
            pos[0] += 1
    return out

def module_binary(items):
    """(module [$id] binary "..." "...") -> bytes, else None"""
    if not items or not isinstance(items[0], tuple) or items[0][0] != "atom":
        return None
    if items[0][1] != "module":
        return None
    rest = items[1:]
    if rest and isinstance(rest[0], tuple) and rest[0][0] == "atom" and rest[0][1].startswith("$"):
        rest = rest[1:]
    if not rest or not isinstance(rest[0], tuple) or rest[0][0] != "atom" or rest[0][1] != "binary":
        return None
    blob = b""
    for t in rest[1:]:
        if isinstance(t, tuple) and t[0] == "str":
            blob += t[1]
        else:
            return None
    return blob

def extract_binary(wast_path):
    """-> [(kind, bytes, line, expected_text)]"""
    text = open(wast_path, encoding="utf-8", errors="surrogateescape").read()
    out = []
    for items, line in parse(toks(text)):
        head = items[0] if items else None
        if not (isinstance(head, tuple) and head[0] == "atom"):
            continue
        name = head[1]
        if name == "module":
            b = module_binary(items)
            if b is not None:
                out.append(("accept", b, line, ""))
        elif name in ("assert_malformed", "assert_invalid"):
            if len(items) < 2 or not isinstance(items[1], tuple) or isinstance(items[1][0], tuple) is False:
                pass
            sub = items[1]
            if not (isinstance(sub, tuple) and isinstance(sub[0], list)):
                continue
            b = module_binary(sub[0])
            if b is None:
                continue          # text/quoted module: needs an assembler
            msg = ""
            for t in items[2:]:
                if isinstance(t, tuple) and t[0] == "str":
                    msg = t[1].decode("utf-8", "replace")
            out.append(("malformed" if name == "assert_malformed" else "invalid",
                        b, line, msg))
    return out

# ------------------------------------------------- rescuing a refused file
#
# wast2json refuses a WHOLE FILE when any single form uses syntax it does not
# know, and --enable-all does not help because the failure is in the PARSER,
# not the feature gate.  Measured against wabt 1.0.36 on the pinned suite:
# 54 of 257 files, and among them unreached-invalid.wast, whose 121
# assert_invalid cases are the corpus for exactly the part of this validator
# most likely to be silently wrong (the polymorphic stack after `unreachable`).
# One `ref.as_non_null` at line 701 was costing all of them; 110 come back.
#
# So a refused file is converted UP TO its first error instead of being
# dropped: cut at the last top-level form that ends before the error line,
# retry, repeat.  wast2json reports errors as `path:LINE:COL: error:`, and
# truncating only the tail leaves every surviving line number equal to the
# original, so origins stay `file.wast:LINE` and the baseline keeps working.
# What the cut discards is then swept by the no-dependency binary extractor,
# so nothing is lost twice and nothing is counted twice.

def top_level_ends(text):
    """[(line, byte_offset_just_past)] for every top-level `)` in a .wast."""
    i, n, line, depth, out = 0, len(text), 1, 0, []
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1; i += 1
        elif text.startswith(";;", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("(;", i):
            d, i = 1, i + 2
            while i < n and d:
                if text.startswith("(;", i): d, i = d + 1, i + 2
                elif text.startswith(";)", i): d, i = d - 1, i + 2
                else:
                    if text[i] == "\n": line += 1
                    i += 1
        elif c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 2
                else:
                    if text[i] == "\n": line += 1
                    i += 1
            i += 1
        elif c == "(":
            depth += 1; i += 1
        elif c == ")":
            depth -= 1; i += 1
            if depth == 0:
                out.append((line, i))
        else:
            i += 1
    return out

ERRLINE = re.compile(r":(\d+):\d+: error")

def convert_prefix(w2j, path, jpath, tmp_wast):
    """Convert the longest parseable PREFIX of `path`.  -> (ok, cut_line).

    cut_line is the last original line that was converted; 0 means the file
    failed at its very first form and nothing was recovered."""
    text = open(path, encoding="utf-8", errors="surrogateescape").read()
    ends = top_level_ends(text)
    cut = len(text)
    cut_line = ends[-1][0] if ends else 0
    for _ in range(8):
        with open(tmp_wast, "w", encoding="utf-8", errors="surrogateescape") as fh:
            fh.write(text[:cut])
        p = subprocess.run([w2j, "--enable-all", "-o", jpath, tmp_wast],
                           capture_output=True)
        if p.returncode == 0 and os.path.exists(jpath):
            return True, cut_line
        errs = [int(m) for m in ERRLINE.findall(p.stderr.decode("utf-8", "replace"))]
        if not errs:
            return False, 0
        first = min(errs)
        cands = [(l, b) for (l, b) in ends if l < first and b < cut]
        if not cands:
            return False, 0
        cut_line, cut = cands[-1]
    return False, 0

# ---------------------------------------------------------------- main

# ------------------------------------------------- the execution manifest
#
# wast2json writes every scalar as the DECIMAL BIT PATTERN of its type, so an
# f32 is "1078530011" rather than "3.14159".  That is exactly what is wanted:
# it survives -0.0, every NaN payload, and the round trip through a TSV column.
# The only values that are not bit patterns are the two NaN wildcards, and they
# are wildcards because the specification does not fix the payload there.

_VT = {"i32", "i64", "f32", "f64"}

def enc_val(v):
    """-> "i32:123" or None if the type is out of scope for an MVP interpreter."""
    t = v.get("type")
    if t not in _VT:
        return None                      # v128, funcref, externref
    s = v.get("value", "")
    if s == "nan:canonical":  return t + ":nanc"
    if s == "nan:arithmetic": return t + ":nana"
    if not s or not s.lstrip("-").isdigit():
        return None
    return t + ":" + s

def enc_vals(vs):
    out = []
    for v in vs:
        e = enc_val(v)
        if e is None:
            return None
        out.append(e)
    return ",".join(out)

def hexname(s):
    return s.encode("utf-8", "surrogateescape").hex()

def exec_rows(f, commands):
    """[(kind, field-hex, args, expected, origin)] for one .wast's commands."""
    rows = []
    cur = None            # the $name of the module a bare action applies to
    for cmd in commands:
        t = cmd.get("type")
        org = "%s:%d" % (f, cmd.get("line", 0))
        if t in ("module", "assert_uninstantiable"):
            fn = cmd.get("filename")
            cur = cmd.get("name")
            if fn and fn.endswith(".wasm"):
                rows.append(("modtrap" if t == "assert_uninstantiable" else "mod",
                             fn, "", "", org))
            continue
        if t == "register":
            # A registered module becomes importable by later ones.  This
            # runner links only against `spectest`, so those later modules are
            # reported as unlinkable rather than guessed at -- see the C side,
            # which counts them in their own column.
            continue
        if t not in ("assert_return", "assert_trap", "assert_exhaustion", "action"):
            continue
        act = cmd.get("action") or {}
        # AN ACTION MAY NAME AN EARLIER MODULE.  store1.wast stores 1 into $M1
        # and 2 into $M2 and then reads both back, and a runner that ignores
        # the `module` key applies every action to whichever module it loaded
        # last -- which produces a WRONG-VALUE failure that looks exactly like
        # an interpreter bug (measured: store1.wast:27, "got 2 want 1").
        # A runner that holds one instance at a time cannot answer these, so
        # they are marked rather than mis-attributed.
        if act.get("module") and act.get("module") != cur:
            rows.append(("skip", "", "", "action targets an earlier module", org))
            continue
        if act.get("type") == "get":
            exp = enc_vals(cmd.get("expected", []))
            if exp is None:
                rows.append(("skip", "", "", "non-MVP value type", org))
            else:
                rows.append(("get", hexname(act.get("field", "")), "", exp, org))
            continue
        if act.get("type") != "invoke":
            continue
        args = enc_vals(act.get("args", []))
        if args is None:
            rows.append(("skip", "", "", "non-MVP value type", org))
            continue
        if t == "assert_return":
            exp = enc_vals(cmd.get("expected", []))
            if exp is None:
                rows.append(("skip", "", "", "non-MVP value type", org))
                continue
            rows.append(("ret", hexname(act["field"]), args, exp, org))
        elif t == "assert_trap":
            rows.append(("trap", hexname(act["field"]), args,
                         cmd.get("text", ""), org))
        elif t == "assert_exhaustion":
            rows.append(("exh", hexname(act["field"]), args, cmd.get("text", ""), org))
        else:
            rows.append(("act", hexname(act["field"]), args, "", org))
    return rows

def find_wast2json():
    for c in (os.environ.get("WAST2JSON"), shutil.which("wast2json")):
        if c and os.path.exists(c):
            return c
    return None

def main():
    if len(sys.argv) < 3:
        sys.stderr.write("usage: wasm_corpus.py <testsuite-dir> <out-dir>\n")
        return 2
    suite, out = sys.argv[1], sys.argv[2]
    wasts = sorted(f for f in os.listdir(suite) if f.endswith(".wast"))
    if not wasts:
        sys.stderr.write("wasm_corpus: no .wast files in %s\n" % suite)
        return 2
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith((".wasm", ".json", ".wat", ".tsv", ".wast")):
            os.remove(os.path.join(out, f))

    w2j = find_wast2json()
    mode = "wast2json" if w2j else "binary-only"
    rows, k = [], 0
    runs = []             # the execution manifest; see exec_rows
    refused = []          # .wast files wast2json would not convert AT ALL

    if w2j:
        # The whole suite through wabt.  --enable-all makes wast2json ASSEMBLE
        # post-MVP text modules rather than refuse the file; whether the module
        # is in scope for an MVP decoder is then decided per case by the C
        # harness, which is where that judgement belongs.
        for f in wasts:
            j = os.path.join(out, f[:-5] + ".json")
            src = os.path.join(suite, f)
            p = subprocess.run([w2j, "--enable-all", "-o", j, src],
                               capture_output=True)
            cut_line = None
            if p.returncode != 0 or not os.path.exists(j):
                # THE FILE WAS REFUSED WHOLE, AND UNTIL 2026-08-30 THAT WAS
                # SILENT: the corpus printed 4661 cases without mentioning that
                # 54 of 257 files had produced none of them, which reads as
                # full coverage.  Recovering them took it to 5235 -- and the
                # first run of the recovered corpus found a real validator
                # bug (wasm.h, WASM_VT_UNKNOWN).
                #
                # Two recoveries, in this order, and the manifest records both:
                #   1. convert the longest PREFIX wast2json can parse
                #      (unreached-invalid.wast: 110 of its commands come back,
                #       and they are the whole reason to want this file);
                #   2. sweep the discarded tail with the no-dependency
                #      `(module binary "...")` extractor, so elem.wast's seven
                #      typed-funcref modules are not lost with it.
                refused.append(f)
                tmp = os.path.join(out, f[:-5] + ".trunc.wast")
                ok, cut_line = convert_prefix(w2j, src, j, tmp)
                try:
                    os.remove(tmp)
                except OSError:
                    pass
                try:
                    cases = extract_binary(src)
                except Exception as ex:                  # noqa: BLE001
                    sys.stderr.write("wasm_corpus: %s: %s\n" % (f, ex))
                    cases = []
                for kind, blob, line, msg in cases:
                    if ok and line <= cut_line:
                        continue        # already covered by the prefix, exactly once
                    fn = "b%05d.wasm" % k; k += 1
                    open(os.path.join(out, fn), "wb").write(blob)
                    rows.append((kind, fn, "%s:%d" % (f, line), msg))
                if not ok:
                    continue
            d = json.load(open(j))
            runs += exec_rows(f, d["commands"])
            for cmd in d["commands"]:
                t = cmd.get("type")
                fn = cmd.get("filename")
                if not fn or not fn.endswith(".wasm"):
                    continue
                if t in ("module", "assert_uninstantiable", "assert_unlinkable"):
                    kind = "accept"
                elif t == "assert_malformed":
                    kind = "malformed"
                elif t == "assert_invalid":
                    kind = "invalid"
                else:
                    continue
                if cmd.get("module_type") not in (None, "binary"):
                    continue
                rows.append((kind, fn, "%s:%d" % (f, cmd.get("line", 0)),
                             cmd.get("text", "")))
    else:
        for f in wasts:
            try:
                cases = extract_binary(os.path.join(suite, f))
            except Exception as ex:                      # noqa: BLE001
                sys.stderr.write("wasm_corpus: %s: %s\n" % (f, ex))
                continue
            for kind, blob, line, msg in cases:
                fn = "c%05d.wasm" % k; k += 1
                open(os.path.join(out, fn), "wb").write(blob)
                rows.append((kind, fn, "%s:%d" % (f, line), msg))

    with open(os.path.join(out, "runs.tsv"), "w", encoding="utf-8") as fh:
        fh.write("# mode\t%s\n" % mode)
        if not w2j:
            fh.write("# note\tbinary-only mode carries no actions: the "
                     "assert_return / assert_trap corpus is written in the "
                     "TEXT format and needs wast2json.  Run: make wasm-fetch\n")
        for kind, fld, args, exp, org in runs:
            fh.write("%s\t%s\t%s\t%s\t%s\n"
                     % (kind, fld, args, exp.replace("\t", " "), org))

    with open(os.path.join(out, "manifest.tsv"), "w", encoding="utf-8") as fh:
        fh.write("# mode\t%s\n" % mode)
        fh.write("# files\t%d converted of %d; %d refused by wast2json\n"
                 % (len(wasts) - len(refused), len(wasts), len(refused)))
        if refused:
            fh.write("# refused\t%s\n" % " ".join(refused))
        for kind, fn, origin, msg in rows:
            fh.write("%s\t%s\t%s\t%s\n" % (kind, fn, origin, msg.replace("\t", " ")))
    n = {}
    for kind, _, _, _ in rows:
        n[kind] = n.get(kind, 0) + 1
    print("wasm_corpus: mode=%s  %d cases  (%s)" %
          (mode, len(rows), ", ".join("%s %d" % kv for kv in sorted(n.items()))))
    rn = {}
    for kind, _, _, _, _ in runs:
        rn[kind] = rn.get(kind, 0) + 1
    print("wasm_corpus: runs.tsv %d rows  (%s)" %
          (len(runs), ", ".join("%s %d" % kv for kv in sorted(rn.items())) or "none"))
    if refused:
        # Printed rather than buried: a corpus that quietly lost a fifth of the
        # suite still prints a big case count, and the case count is the number
        # a reader trusts.
        print("wasm_corpus: %d of %d .wast files were REFUSED WHOLE by wast2json; "
              "each was converted up to its first error and its tail swept for "
              "`(module binary)` cases" % (len(refused), len(wasts)))
        print("wasm_corpus:   %s" % " ".join(refused))
    return 0

if __name__ == "__main__":
    sys.exit(main())
