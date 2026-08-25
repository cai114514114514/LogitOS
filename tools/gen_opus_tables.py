#!/usr/bin/env python3
"""Generate c/lib/audio/opus_tables.h from RFC 6716's OWN reference source.

WHY THIS IS A GENERATOR AND NOT A TYPED HEADER.  The same argument
tools/gen_vp8_tables.py makes, and it is stronger here.  CELT's tables are not
perceptual tuning that a wrong entry merely shades: cache_bits50 is the inverse
of the pulse-count-to-bits curve and feeds bits2pulses(), which decides HOW MANY
BITS the next band consumes.  One wrong byte does not make a band sound thin --
it desynchronises the range decoder and every subsequent symbol in the frame is
noise.  e_prob_model is the same: it is the probability model the arithmetic
decoder is driven by, so a typo there is a DIFFERENT decoder, not a worse one.
Three thousand numbers of that kind are not checkable by eye, so they are
lifted mechanically.

WHERE FROM.  RFC 6716 Appendix A embeds the entire reference implementation as
base64 inside the RFC text and states the SHA-1 of the tarball that comes out.
tools/opusvec.sh does the extraction and checks that hash, so this script's
input is the normative source, verified -- not a copy of libopus from
somewhere, and not this author's transcription.

WHAT IT REFUSES TO DO.  It takes only real array definitions, matched by
declared type AND name, requires the value count to equal the declared shape,
and dies if a name is missing, defined twice, or the wrong length.  The
FIXED_POINT preprocessor branches are resolved by an explicit mini-evaluator
rather than by "take the first match": eMeans, pred_coef, beta_coef and
beta_intra are each defined TWICE in quant_bands.c, once per build, and the two
differ by a factor of 32768.  A scan that took whichever came first would
produce a decoder that is wrong by 90 dB and still runs.

Usage:
    python3 tools/gen_opus_tables.py <ref-src-dir> [-o out.h]
    python3 tools/gen_opus_tables.py <ref-src-dir> --check
"""
import argparse
import hashlib
import os
import re
import sys

# (file, C type, name, shape) -- every dimension is asserted against the number
# of values actually parsed.
WANT = [
    ("modes.c",              "opus_int16",    "eband5ms",          (22,)),
    ("modes.c",              "unsigned char", "band_allocation",   (11, 21)),
    ("static_modes_float.h", "opus_val16",    "window120",         (120,)),
    ("static_modes_float.h", "opus_int16",    "logN400",           (21,)),
    ("static_modes_float.h", "opus_int16",    "cache_index50",     (105,)),
    ("static_modes_float.h", "unsigned char", "cache_bits50",      (392,)),
    ("static_modes_float.h", "unsigned char", "cache_caps50",      (168,)),
    ("quant_bands.c",        "unsigned char", "e_prob_model",      (4, 2, 42)),
    ("quant_bands.c",        "unsigned char", "small_energy_icdf", (3,)),
    ("quant_bands.c",        "opus_val16",    "eMeans",            (25,)),
    ("quant_bands.c",        "opus_val16",    "pred_coef",         (4,)),
    ("quant_bands.c",        "opus_val16",    "beta_coef",         (4,)),
    ("celt.c",               "unsigned char", "trim_icdf",         (11,)),
    ("celt.c",               "unsigned char", "spread_icdf",       (4,)),
    ("celt.c",               "unsigned char", "tapset_icdf",       (3,)),
    ("celt.c",               "signed char",   "tf_select_table",   (4, 8)),
    ("rate.c",               "unsigned char", "LOG2_FRAC_TABLE",   (24,)),
    ("bands.c",              "int",           "ordery_table",      (30,)),
    ("bands.c",              "opus_int16",    "exp2_table8",       (8,)),
]

CTYPE = {
    "opus_int16":    "short",
    "unsigned char": "unsigned char",
    "signed char":   "signed char",
    "int":           "int",
    "opus_val16":    None,      # per-table: see FLOAT_TABLES
}
FLOAT_TABLES = {"window120", "eMeans", "pred_coef", "beta_coef"}

DQUOTE = chr(34)
SQUOTE = chr(39)


def strip_comments(text):
    """Remove block and line comments without touching string literals.

    None of the wanted arrays contains a string literal, but the function is
    written not to care -- "it happens not to matter here" is exactly how a
    generator acquires a silent failure mode two years later."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == DQUOTE or c == SQUOTE:
            q = c
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == chr(92):          # backslash escape
                    i += 2
                    if i - 1 < n:
                        out.append(text[i - 1])
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                break
            # Keep the newlines so reported line numbers stay usable.
            out.append(chr(10) * text.count(chr(10), i, j))
            i = j + 2
            continue
        if text.startswith("//", i):
            j = text.find(chr(10), i)
            if j < 0:
                break
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def resolve_fixed_point(text):
    """Delete the FIXED_POINT arm of every conditional; keep the float arm.

    Only FIXED_POINT is evaluated.  Every other #if is left in place with its
    body, because none of the wanted tables sits inside one and inventing a
    general expression evaluator would be a second thing to get wrong.  Nesting
    IS tracked for all directives, so an unrelated #if inside a FIXED_POINT
    block cannot make the #endif match the wrong opener."""
    out = []
    stack = []          # [is_fixed_point_conditional, emitting_this_arm]
    for ln in text.split(chr(10)):
        s = ln.strip()
        m = re.match(r"#\s*(ifdef|ifndef|if|else|elif|endif)\b(.*)", s)
        if m:
            d, rest = m.group(1), m.group(2).strip()
            if d in ("ifdef", "ifndef", "if"):
                flat = rest.replace(" ", "")
                if d == "ifdef" and rest == "FIXED_POINT":
                    stack.append([True, False]); out.append(""); continue
                if d == "ifndef" and rest == "FIXED_POINT":
                    stack.append([True, True]); out.append(""); continue
                if d == "if" and flat in ("defined(FIXED_POINT)", "FIXED_POINT"):
                    stack.append([True, False]); out.append(""); continue
                if d == "if" and flat in ("!defined(FIXED_POINT)",):
                    stack.append([True, True]); out.append(""); continue
                stack.append([False, True])
            elif d == "else":
                if stack and stack[-1][0]:
                    stack[-1][1] = not stack[-1][1]
                    out.append("")
                    continue
            elif d == "elif":
                if stack and stack[-1][0]:
                    raise SystemExit("#elif inside a FIXED_POINT conditional: "
                                     "this evaluator does not handle it")
            elif d == "endif":
                if stack:
                    was_fp = stack.pop()[0]
                    if was_fp:
                        out.append("")
                        continue
        if all(emit for isfp, emit in stack if isfp):
            out.append(ln)
        else:
            out.append("")
    if stack:
        raise SystemExit("unbalanced preprocessor conditionals")
    return chr(10).join(out)


def find_array(text, ctype, name, path):
    """Find `static const <ctype> <name>[...] = { ... };` and return its tokens.

    Matching the declared type as well as the name is belt-and-braces against a
    regression in the FIXED_POINT resolution above, because the failure that
    would cause is silent."""
    pat = re.compile(r"static\s+const\s+" + re.escape(ctype) + r"\s+" +
                     re.escape(name) + r"\s*((?:\[[^\]]*\]\s*)+)=\s*")
    hits = list(pat.finditer(text))
    if not hits:
        raise SystemExit("%s: no `static const %s %s[...]` found"
                         % (path, ctype, name))
    if len(hits) > 1:
        raise SystemExit("%s: `static const %s %s` defined %d times -- the "
                         "FIXED_POINT resolution did not collapse it"
                         % (path, ctype, name, len(hits)))
    i = text.index("{", hits[0].end() - 1)
    depth, j = 0, i
    while j < len(text):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                break
        j += 1
    else:
        raise SystemExit("%s: unterminated initialiser for %s" % (path, name))
    toks = [t.strip() for t in re.split(r"[,{}]", text[i + 1:j])]
    return [t for t in toks if t]


def as_int(tok, name, path):
    t = tok.rstrip("uUlL")
    try:
        return int(t, 0)
    except ValueError:
        raise SystemExit("%s: %s: not an integer literal: %r" % (path, name, tok))


def as_float(tok, name, path):
    t = tok.rstrip("fF")
    # quant_bands.c writes these as `29440/32768.`, an exact binary fraction,
    # so evaluating the division is exact in double.
    m = re.match(r"^\s*(-?[0-9.eE+-]+)\s*/\s*(-?[0-9.eE+-]+)\s*$", t)
    try:
        if m:
            return float(m.group(1)) / float(m.group(2))
        return float(t)
    except ValueError:
        raise SystemExit("%s: %s: not a float literal: %r" % (path, name, tok))


def _f(v):
    return repr(float(v))


def _rows(vals, isf, n, indent="  "):
    per = 5 if isf else 12
    out = []
    for i in range(0, n, per):
        chunk = vals[i:i + per]
        if isf:
            out.append(indent + ", ".join(_f(v) for v in chunk) + ",")
        else:
            out.append(indent + ", ".join("%d" % v for v in chunk) + ",")
    return chr(10).join(out)


def emit(vals, sha1):
    L = []
    A = L.append
    A("/* c/lib/audio/opus_tables.h -- GENERATED. Do not edit.")
    A(" *")
    A(" *   python3 tools/gen_opus_tables.py <ref> -o c/lib/audio/opus_tables.h")
    A(" *   python3 tools/gen_opus_tables.py <ref> --check    (idempotence gate)")
    A(" *")
    A(" * Source: the reference implementation embedded in RFC 6716 Appendix A,")
    A(" * extracted from the RFC text by tools/opusvec.sh and verified against")
    A(" * the SHA-1 the RFC itself prints for it:")
    A(" *")
    A(" *   opus-rfc6716.tar.gz  sha1 %s" % sha1)
    A(" *")
    A(" * THE FLOAT TABLES ARE WIDENED TO double AND THAT IS DELIBERATE.")
    A(" * window120, eMeans, pred_coef and beta_coef are float32 in the")
    A(" * reference's float build and Q15/Q14 integers in its fixed build; the")
    A(" * two builds disagree in the low bits and BOTH pass the conformance")
    A(" * vectors, which is the whole reason the bar for this codec is")
    A(" * opus_compare and not byte equality (see the top of opus.c). Nothing")
    A(" * downstream of these four tables re-enters the range decoder, so their")
    A(" * width cannot move a single decoded bit -- only the synthesised")
    A(" * waveform, by about 1e-7 of full scale.")
    A(" *")
    A(" * Everything that DOES feed the bitstream parse -- the pulse cache, the")
    A(" * band allocation, every icdf, the Laplace model, the tf table -- is")
    A(" * integral here and is used in exact integer arithmetic downstream. */")
    A("#ifndef LOGIT_OPUS_TABLES_H")
    A("#define LOGIT_OPUS_TABLES_H")
    A("")
    for path, ctype, name, shape in WANT:
        v = vals[name]
        isf = name in FLOAT_TABLES
        ct = "double" if isf else CTYPE[ctype]
        dims = "".join("[%d]" % d for d in shape)
        total = 1
        for d in shape:
            total *= d
        A("/* %s -- %s%s from %s, %d values */" % (name, ctype, dims, path, total))
        A("static const %s opus_%s%s = {" % (ct, name, dims))
        if len(shape) == 1:
            A(_rows(v, isf, shape[0]))
        elif len(shape) == 2:
            for r in range(shape[0]):
                A("  {")
                A(_rows(v[r * shape[1]:(r + 1) * shape[1]], isf, shape[1], "   "))
                A("  },")
        else:
            per = shape[1] * shape[2]
            for r in range(shape[0]):
                A("  {")
                for q in range(shape[1]):
                    o = r * per + q * shape[2]
                    A("   {")
                    A(_rows(v[o:o + shape[2]], isf, shape[2], "    "))
                    A("   },")
                A("  },")
        A("};")
        A("")
    A("/* beta_intra is a scalar, not an array, and is carried here so that every")
    A(" * number the coarse-energy recursion uses comes from one place. */")
    A("#define OPUS_BETA_INTRA %s" % _f(vals["beta_intra"][0]))
    A("")
    A("#endif /* LOGIT_OPUS_TABLES_H */")
    return chr(10).join(L) + chr(10)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("refdir", help="unpacked opus-rfc6716 directory")
    ap.add_argument("-o", "--out", default="c/lib/audio/opus_tables.h")
    ap.add_argument("--check", action="store_true",
                    help="regenerate and diff against --out; nonzero if it moved")
    ap.add_argument("--sha1", default="(not supplied)")
    args = ap.parse_args()

    srcs = {}
    for sub in ("celt", "src", "silk"):
        d = os.path.join(args.refdir, sub)
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            srcs.setdefault(f, os.path.join(d, f))

    cache, vals = {}, {}
    for path, ctype, name, shape in WANT:
        if path not in srcs:
            raise SystemExit("reference source not found: %s (under %s)"
                             % (path, args.refdir))
        if path not in cache:
            fh = open(srcs[path], "r", errors="replace")
            cache[path] = resolve_fixed_point(strip_comments(fh.read()))
            fh.close()
        toks = find_array(cache[path], ctype, name, path)
        want = 1
        for d in shape:
            want *= d
        if len(toks) != want:
            raise SystemExit("%s: %s has %d values, declared shape wants %d"
                             % (path, name, len(toks), want))
        if name in FLOAT_TABLES:
            vals[name] = [as_float(t, name, path) for t in toks]
        else:
            vals[name] = [as_int(t, name, path) for t in toks]
        sys.stderr.write("  %-20s %-14s %-22s %5d values\n"
                         % (name, ctype, path, len(toks)))

    txt = cache["quant_bands.c"]
    hits = re.findall(r"static\s+const\s+opus_val16\s+beta_intra\s*=\s*([^;]+);",
                      txt)
    if len(hits) != 1:
        raise SystemExit("quant_bands.c: beta_intra found %d times, want 1"
                         % len(hits))
    vals["beta_intra"] = [as_float(hits[0], "beta_intra", "quant_bands.c")]
    sys.stderr.write("  %-20s %-14s %-22s %5d values\n"
                     % ("beta_intra", "opus_val16", "quant_bands.c", 1))

    text = emit(vals, args.sha1)
    sys.stderr.write("  generated %d bytes, sha256 %s\n"
                     % (len(text), hashlib.sha256(text.encode()).hexdigest()))

    if args.check:
        try:
            fh = open(args.out, "r")
        except IOError:
            raise SystemExit("--check: %s does not exist" % args.out)
        have = fh.read()
        fh.close()
        if have != text:
            raise SystemExit("--check: %s differs from a fresh generation"
                             % args.out)
        sys.stderr.write("  --check: %s reproduces byte for byte\n" % args.out)
        return
    fh = open(args.out, "w", newline=chr(10))
    fh.write(text)
    fh.close()
    sys.stderr.write("  wrote %s\n" % args.out)


if __name__ == "__main__":
    main()
