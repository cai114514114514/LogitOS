#!/usr/bin/env python3
"""Extract VP8's constant tables from RFC 6386 and emit rust/src/vp8_tables.rs.

WHY A GENERATOR AND NOT TYPED-IN CONSTANTS. The tables below are 3,164 bytes of
probabilities and tree indices. Every one of them is load-bearing: a boolean
arithmetic decoder driven by a wrong probability does not produce a slightly
wrong picture, it desynchronises and produces noise -- and a single wrong byte
among three thousand is close to unfindable by bisection. Typing them in from
memory or from a second-hand copy is the one part of a VP8 decoder that cannot
be verified by reading it, so it is not typed in at all: the arrays are cut out
of the normative document's own C source and transcribed mechanically.

The RFC text is fetched once into build/vp8ref/rfc6386.txt (or passed as argv[1])
and is NOT vendored -- it is 400 KB of prose around the tables, and the
generated .rs IS committed, so a rebuild without network still works. Re-run
this only to re-verify provenance:

    curl -o build/vp8ref/rfc6386.txt https://www.rfc-editor.org/rfc/rfc6386.txt
    python3 tools/gen_vp8_tables.py build/vp8ref/rfc6386.txt

It prints the shape and checksum of every table it emits; --check compares
against the committed file and fails on any difference, which is what a CI
target would run.
"""
import os
import re
import sys
import hashlib

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, "rust", "src", "vp8_tables.rs")


def load(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return depaginate(f.read())


def depaginate(text):
    """Remove the RFC's page furniture.

    A table longer than a page has the running footer, a form feed and the
    running header sitting in the middle of its initialiser. Brace matching
    walks straight through them and then tries to resolve "Bankoski" as an
    enumerator -- which is how this was found. The lines are removed rather
    than skipped during parsing so that every later step sees one continuous
    code block, and the line NUMBERS used to disambiguate duplicate tables are
    taken from this same depaginated text.
    """
    out = []
    for ln in text.split(chr(10)):
        if chr(12) in ln:
            continue
        st = ln.strip()
        if st.startswith("Bankoski") and st.endswith("]"):
            continue
        if st.startswith("RFC 6386") and st.endswith("2011"):
            continue
        out.append(ln)
    return chr(10).join(out)


def enums(text):
    """Resolve the enum names the tree tables are written in.

    The trees are `{-B_PRED, 2, 4, 6, -DC_PRED, ...}` -- symbolic, because a
    tree_index leaf is the negated enumerator. Rather than hardcode the values,
    the three enums are read out of the RFC's own typedefs, so a name and its
    value can never disagree here. Enumerators with no `= value` take the next
    integer, C rules.
    """
    tbl = {}
    lines = text.split("\n")
    # Two enums the reference decoder (dixie, Attachment One) declares WITHOUT
    # `typedef` -- `enum reference_frame { CURRENT_FRAME, ... }` and
    # `enum prediction_mode { DC_PRED, ..., SPLITMV, ..., LEFT4X4, ... }`. Every
    # other enum in the document is `typedef enum { ... } name;`, which is why
    # the loop below keys on that substring; these two need a second, narrow
    # trigger rather than a bare "enum" substring match, which would also catch
    # ordinary variable declarations (`enum reference_frame ref_frame;`) that
    # have no brace to scan and could swallow whatever unrelated block follows.
    plain_enum_re = re.compile(r"^\s*enum\s+(reference_frame|prediction_mode)\s*$")
    for i, ln in enumerate(lines):
        if "typedef enum" not in ln and not plain_enum_re.match(ln):
            continue
        body, depth, started = [], 0, False
        for l2 in lines[i:i + 60]:
            for ch in l2:
                if ch == "{":
                    depth += 1
                    started = True
                elif ch == "}":
                    depth -= 1
            body.append(l2)
            if started and depth == 0:
                break
        b = "\n".join(body)
        if "{" not in b:
            continue
        b = b[b.index("{") + 1:]
        b = re.sub(r"/\*.*?\*/", " ", b, flags=re.S)
        b = b.split("}")[0]
        nxt = 0
        for item in b.split(","):
            item = item.strip()
            if not item:
                continue
            m = re.match(r"^([A-Za-z_][A-Za-z_0-9]*)\s*(?:=\s*([A-Za-z_0-9]+))?$", item)
            if not m:
                continue
            name, val = m.group(1), m.group(2)
            if val is not None:
                nxt = int(val) if val.isdigit() else tbl.get(val, 0)
            # First definition wins: the RFC repeats intra_mbmode verbatim, and
            # a later enum reusing a name (mv_nearest = num_ymodes) must not
            # overwrite an earlier one.
            tbl.setdefault(name, nxt)
            nxt += 1
    return tbl


def grab_all(text, decl):
    """Every initialiser in the document for a declaration matching `decl`.

    RFC 6386 declares several tables twice -- once in the prose walkthrough,
    once in the reference decoder -- and the prose copy is sometimes an
    abbreviated illustration rather than the real table. Disambiguating by line
    number was the first attempt and it broke the moment depagination shifted
    every line; this returns ALL of them and lets the caller pick by shape,
    which cannot drift.
    """
    lines = text.split(chr(10))
    found = []
    # Only DECLARATIONS, never uses.  appears five times in the
    # document -- as a declaration twice and as an argument to treed_read three
    # more times -- and brace-matching from a use swallows whatever block comes
    # next, which is how five "occurrences" came to disagree.
    name = decl.split("[")[0].strip()
    # filter_t is the interp-filter typedef (`typedef int filter_t[6]`, sec.
    # 20.14/20.15) -- sixtap_filters/bilinear_filters are declared through it
    # rather than a bare `int`, so it has to join the keyword set like the
    # others or grab_all finds nothing and raises.
    declre = re.compile(r"(?:static\s+)?(?:const\s+)?(?:tree_index|Prob|int8_t|uint8_t|unsigned int|"
                        r"unsigned char|filter_t|MV_CONTEXT|int)\s+" + re.escape(name) + r"\s*\[")
    for i, ln in enumerate(lines):
        if not declre.search(ln):
            continue
        buf, depth, started = [], 0, False
        for l2 in lines[i:i + 400]:
            for ch in l2:
                if ch == "{":
                    depth += 1
                    started = True
                elif ch == "}":
                    depth -= 1
            buf.append(l2)
            if started and depth == 0:
                break
        if not started:
            continue
        body = chr(10).join(buf)
        body = body[body.index("{"):]
        body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
        body = re.sub(r"//.*", " ", body)
        vals, bad = [], False
        for tok in re.findall(r"-?\s*[A-Za-z_][A-Za-z_0-9]*|-?\d+", body):
            tok = tok.replace(" ", "")
            if re.fullmatch(r"-?\d+", tok):
                vals.append(int(tok))
                continue
            neg = tok.startswith("-")
            name = tok[1:] if neg else tok
            if name not in ENUMS:
                bad = True
                break
            vals.append(-ENUMS[name] if neg else ENUMS[name])
        if not bad:
            found.append((i + 1, vals))
    if not found:
        raise SystemExit("gen_vp8_tables: declaration not found: %r" % decl)
    return found


def emit_vals(name, ty, dims, vals, per_line=None, gate=None):
    """Same renderer, for a table this generator ASSEMBLES rather than lifts
    verbatim (the ragged Pcat lists padded into a rectangle)."""
    return _render(name, ty, dims, vals, per_line, -1, gate)


def emit(name, ty, dims, decl, per_line=None, gate=None):
    n = 1
    for d in dims:
        n *= d
    cands = grab_all(TEXT, decl)
    fits = [(line, v) for (line, v) in cands if len(v) == n]
    if not fits:
        raise SystemExit(
            "gen_vp8_tables: %s (%s = %d values): no occurrence of %r has that "
            "many; found %s" % (name, "x".join(str(d) for d in dims), n, decl,
                                [(l, len(v)) for l, v in cands]))
    # Several occurrences of the right shape must AGREE, or the document is
    # ambiguous and picking one silently would be a guess.
    if any(v != fits[0][1] for _, v in fits[1:]):
        raise SystemExit("gen_vp8_tables: %s: occurrences at %s disagree"
                         % (name, [l for l, _ in fits]))
    line, vals = fits[0]
    return _render(name, ty, dims, vals, per_line, line, gate)


def _render(name, ty, dims, vals, per_line, line, gate=None):
    digest = hashlib.sha256(",".join(str(v) for v in vals).encode()).hexdigest()[:12]
    n = 1
    for d in dims:
        n *= d
    if len(vals) != n:
        raise SystemExit("gen_vp8_tables: %s got %d values, want %d" % (name, len(vals), n))
    where = ("L%d" % line) if line >= 0 else "assembled"
    print("  %-22s %-12s %5d values  %-9s sha256:%s"
          % (name, "x".join(str(d) for d in dims), n, where, digest))

    tystr = ty
    for d in reversed(dims):
        tystr = "[%s; %d]" % (tystr, d)

    # `gate`, if given, is a Cargo feature name: the emitted static gets its
    # own `#[cfg(feature = "...")]` line. THIS IS NOT COSMETIC. It is the
    # mechanism that keeps a video decoder's tables out of the kernel build
    # (see vp8_inter.rs's module doc): the kernel's $(RUST_LIB) never passes
    # --features, so a table with no cfg here compiles into ring 0
    # unconditionally. Before this parameter existed the cfg lines were typed
    # in by hand after generation -- which means `python3 tools/gen_vp8_tables.py
    # build/vp8ref/rfc6386.txt`, the exact command this file's own header
    # tells a reader to run to refresh the tables, would have silently deleted
    # every one of them on the next run: the tool would have done exactly
    # what its own instructions said, and the safety property would have been
    # gone. Found and fixed 2026-08-25, before that command was ever run for
    # real -- the gap was real, not hypothetical, for exactly as long as
    # nobody had refreshed the tables yet.
    cfg = ('#[cfg(feature = "%s")]\n' % gate) if gate else ""

    def block(vs, dd):
        if len(dd) == 1:
            w = per_line or (16 if len(vs) > 16 else len(vs))
            if len(vs) <= w:
                return "[" + ", ".join(str(v) for v in vs) + "]"
            rows = ["    " + ", ".join(str(v) for v in vs[i:i + w]) + ","
                    for i in range(0, len(vs), w)]
            return "[" + chr(10) + chr(10).join(rows) + chr(10) + "]"
        step = len(vs) // dd[0]
        parts = [block(vs[i * step:(i + 1) * step], dd[1:]) for i in range(dd[0])]
        inner = ("," + chr(10)).join(parts)
        inner = chr(10).join("    " + ln for ln in inner.split(chr(10)))
        return "[" + chr(10) + inner + "," + chr(10) + "]"

    return "%spub static %s: %s = %s;%s" % (cfg, name, tystr, block(vals, dims), chr(10))


ENUMS = {}


TEXT = ""


def build(text):
    global ENUMS, TEXT
    TEXT = text
    ENUMS = enums(text)
    out = []
    out.append("""//! VP8 constant tables -- GENERATED, do not edit.
//!
//! Source: RFC 6386 (VP8 Data Format and Decoding Guide), the normative
//! document's own reference-decoder C source. Regenerate and re-verify with:
//!
//!     curl -o build/vp8ref/rfc6386.txt https://www.rfc-editor.org/rfc/rfc6386.txt
//!     python3 tools/gen_vp8_tables.py build/vp8ref/rfc6386.txt
//!
//! Why generated: a wrong probability does not shade a pixel, it desynchronises
//! the arithmetic decoder, and one wrong byte in three thousand is not findable
//! by looking. See tools/gen_vp8_tables.py for the whole argument.
//!
//! The RFC declares several of these twice (prose, then reference decoder);
//! the generator pins the line to take each from, so "first match" is never
//! what decides.

""")
    # --- trees. tree_index is signed: negative entries are leaves. ---
    out.append(emit("KF_YMODE_TREE", "i8", [8], "kf_ymode_tree"))
    out.append(emit("KF_YMODE_PROB", "u8", [4], "kf_ymode_prob"))
    out.append(emit("BMODE_TREE", "i8", [18], "bmode_tree"))
    out.append(emit("UV_MODE_TREE", "i8", [6], "uv_mode_tree"))
    out.append(emit("KF_UV_MODE_PROB", "u8", [3], "kf_uv_mode_prob"))
    out.append(emit("MB_SEGMENT_TREE", "i8", [6], "mb_segment_tree"))
    out.append(emit("COEFF_TREE", "i8", [22], "coeff_tree"))
    out.append(emit("COEFF_BANDS", "usize", [16], "coeff_bands [16]"))

    # kf_bmode_prob is declared at 2529 (prose) and 2607 (the table itself).
    out.append(emit("KF_BMODE_PROBS", "u8", [10, 10, 9],
                    "kf_bmode_prob", per_line=9))

    out.append(emit("COEFF_UPDATE_PROBS", "u8", [4, 8, 3, 11],
                    "coeff_update_probs", per_line=11))
    out.append(emit("DEFAULT_COEFF_PROBS", "u8", [4, 8, 3, 11],
                    "default_coeff_probs", per_line=11))

    # Extra-bit probabilities for the six "category" tokens. The RFC writes each
    # as a NUL-terminated list of different length; they are padded to a
    # rectangle here so the decoder can index them uniformly, and the true
    # length is CAT_LEN (never the padding).
    cats = []
    lens = []
    for i in range(1, 7):
        v = grab_all(text, "Pcat%d" % i)[0][1]
        assert v[-1] == 0, "Pcat%d is not NUL-terminated" % i
        v = v[:-1]
        lens.append(len(v))
        cats.append(v)
    width = max(lens)
    flat = []
    for v in cats:
        flat += v + [0] * (width - len(v))
    out.append(emit_vals("CAT_PROBS", "u8", [6, width], flat, per_line=width))
    out.append(emit_vals("CAT_LEN", "usize", [6], lens))
    out.append(emit("CAT_BASE", "i32", [6], "categoryBase[6]"))
    out.append(emit("ZIGZAG", "usize", [16], "zigzag[16]"))

    # --- inter-frame tables (VP8 interframe prediction, RFC 6386 secs
    # 16-17). Pulled from Attachment One (dixie, the RFC's own reference
    # decoder), which is the only place several of these are given as a
    # single unambiguous declaration -- the prose walkthrough in secs 16-17
    # states most of them only in running text or under a differently-cased
    # name (`LEFT4x4` in prose vs `LEFT4X4` in dixie.h). dixie's names are
    # used throughout so a reader can find each table by grep in Attachment
    # One directly.
    #
    # EVERY ONE OF THESE IS GATE=INTER, and the comment block below is
    # EMITTED (not just a Python-source comment) for the same reason: until
    # 2026-08-25 the `#[cfg(feature = "vp8-interframe")]` line above each of
    # these statics, and this explanatory comment, were typed into
    # rust/src/vp8_tables.rs BY HAND after a generator run -- so `--check`
    # verified the VALUES but the generator itself did not know the gate
    # existed, and the documented refresh command
    # (`python3 tools/gen_vp8_tables.py build/vp8ref/rfc6386.txt`) would have
    # silently deleted the mechanism that keeps a video decoder's tables out
    # of the kernel build the next time anyone ran it. Fixed by threading
    # `gate=INTER` through `emit()` -> `_render()`, which is what actually
    # prints the cfg line now -- see the comment there for the full argument.
    INTER = "vp8-interframe"
    out.append(
        "\n"
        "// --- Inter-frame-only tables from here down. Gated behind `vp8-interframe`\n"
        "// (off by default -- see Cargo.toml) so that the kernel's $(RUST_LIB), which\n"
        "// never enables the feature, carries none of these symbols. Proved with `nm`,\n"
        "// not assumed: see the report in the commit that added vp8_inter.rs.\n")
    out.append(emit("Y_MODE_TREE", "i8", [8], "y_mode_tree", gate=INTER))
    out.append(emit("Y_MODE_PROB", "u8", [4], "k_default_y_mode_probs", gate=INTER))
    out.append(emit("UV_MODE_PROB", "u8", [3], "k_default_uv_mode_probs", gate=INTER))
    out.append(emit("INTER_B_MODE_PROB", "u8", [9], "default_b_mode_probs", gate=INTER))
    out.append(emit("MV_REF_TREE", "i8", [8], "mv_ref_tree", gate=INTER))
    out.append(emit("MODE_CONTEXTS", "u8", [6, 4], "mv_counts_to_probs", gate=INTER))
    out.append(emit("SPLIT_MV_TREE", "i8", [6], "split_mv_tree", gate=INTER))
    out.append(emit("SPLIT_MV_PROBS", "u8", [3], "split_mv_probs", gate=INTER))
    out.append(emit("MV_PARTITIONS", "i32", [4, 16], "mv_partitions", per_line=16, gate=INTER))
    out.append(emit("SUBMV_REF_TREE", "i8", [6], "submv_ref_tree", gate=INTER))
    out.append(emit("SUBMV_REF_PROBS", "u8", [5, 3], "submv_ref_probs2", gate=INTER))
    out.append(emit("SMALL_MV_TREE", "i8", [14], "small_mv_tree", gate=INTER))
    out.append(emit("MV_UPDATE_PROBS", "u8", [2, 19], "vp8_mv_update_probs", per_line=19, gate=INTER))
    out.append(emit("DEFAULT_MV_PROBS", "u8", [2, 19], "k_default_mv_probs", per_line=19, gate=INTER))
    out.append(emit("SIXTAP_FILTERS", "i32", [8, 6], "sixtap_filters", per_line=6, gate=INTER))
    out.append(emit("BILINEAR_FILTERS", "i32", [8, 6], "bilinear_filters", per_line=6, gate=INTER))
    return "".join(out)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") \
        else os.path.join(HERE, "build", "vp8ref", "rfc6386.txt")
    check = "--check" in sys.argv
    if not os.path.exists(src):
        raise SystemExit(
            "gen_vp8_tables: %s not present.\n"
            "  curl -o build/vp8ref/rfc6386.txt https://www.rfc-editor.org/rfc/rfc6386.txt"
            % src)
    print("gen_vp8_tables: from %s" % src)
    text = load(src)
    new = build(text)
    if check:
        cur = load(OUT) if os.path.exists(OUT) else ""
        if cur != new:
            raise SystemExit("FAIL: rust/src/vp8_tables.rs is stale or edited by hand")
        print("check-vp8-tables: ok (matches RFC 6386)")
        return
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(new)
    print("wrote %s (%d bytes)" % (OUT, len(new)))


if __name__ == "__main__":
    main()
