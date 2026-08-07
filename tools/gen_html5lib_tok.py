#!/usr/bin/env python3
"""gen_html5lib_tok.py -- turn the vendored html5lib tokenizer cases into a C table.

The cases live in third_party/html5lib-tests/tokenizer/*.test as JSON.  Parsing
JSON in the test runner would mean writing a JSON parser for a C host test, so
the conversion happens here instead and the runner just walks an array.

Each emitted case carries:
  file, description           for the failure report
  input  (bytes + length)     UTF-8; embedded NULs are why the length is explicit
  state                       html_tok_set_state() argument
  last_start_tag              or NULL
  expect                      the expected token stream, already serialised in
                              the same textual form html_tok_test.c serialises
                              OUR tokens into, so comparison is one strcmp

SERIALISATION FORMAT (both sides produce it, so it only has to be unambiguous)

    D|name|pubid|sysid|quirks     doctype; missing ids are the escape %00
    S|name|k=v,k=v|selfclosing    start tag; attributes SORTED by name
    E|name                        end tag
    C|data                        comment
    T|data                        characters

Every field is escaped so that '|', ',', '=', '%' and any byte outside
printable ASCII become %XX.  That makes the format injective -- an attribute
value of "a|b" cannot be confused with two attributes -- which matters because
this suite is full of exactly that kind of adversarial input.

TWO SUITE RULES THE CONVERSION HAS TO HONOUR
  * adjacent Character tokens are coalesced (the suite says so explicitly; a
    tokenizer is free to split a text run across tokens)
  * "doubleEscaped" cases have their input and output strings escaped a SECOND
    time, so \\uXXXX must be unescaped again before encoding

Usage:  python3 tools/gen_html5lib_tok.py <tests-dir> <out.inc>
"""

import glob
import json
import os
import re
import sys

# html_tok_set_state() arguments -- must match enum in html_tokenizer.h
STATES = {
    "Data state": 0,
    "RCDATA state": 1,
    "RAWTEXT state": 2,
    "Script data state": 3,
    "PLAINTEXT state": 4,
    "CDATA section state": 5,
}

DBL = re.compile(r"\\u([0-9A-Fa-f]{4})")


def undouble(s):
    return DBL.sub(lambda m: chr(int(m.group(1), 16)), s)


def enc(s):
    """Text -> the bytes our tokenizer works on.  surrogatepass is defensive:
    the vendored corpus has no lone surrogates today, but a future sync could
    add one and a UnicodeEncodeError at generate time is a worse failure than
    three bytes the tokenizer passes through untouched."""
    return s.encode("utf-8", "surrogatepass")


def esc(b: bytes) -> str:
    out = []
    for ch in b:
        if ch in b"|,=%" or ch < 0x20 or ch >= 0x7F:
            out.append("%%%02X" % ch)
        else:
            out.append(chr(ch))
    return "".join(out)


NONE = "%00"          # "field absent", distinct from an empty field


def serialize(tokens):
    """html5lib output list -> our textual form, with characters coalesced."""
    out = []
    pending = None                     # accumulated Character text

    def flush():
        nonlocal pending
        if pending is not None:
            out.append("T|" + esc(enc(pending)))
            pending = None

    for tok in tokens:
        kind = tok[0]
        if kind == "Character":
            pending = (pending or "") + tok[1]
            continue
        flush()
        if kind == "DOCTYPE":
            _, name, pub, sysid, correct = tok
            out.append("D|%s|%s|%s|%d" % (
                NONE if name is None else esc(enc(name)),
                NONE if pub is None else esc(enc(pub)),
                NONE if sysid is None else esc(enc(sysid)),
                0 if correct else 1))
        elif kind == "StartTag":
            name, attrs = tok[1], tok[2]
            self_closing = 1 if (len(tok) > 3 and tok[3]) else 0
            # The suite models attributes as a set, so ordering carries no
            # information; sorting both sides is what makes that true here.
            a = ",".join("%s=%s" % (esc(enc(k)), esc(enc(v)))
                         for k, v in sorted(attrs.items()))
            out.append("S|%s|%s|%d" % (esc(enc(name)), a, self_closing))
        elif kind == "EndTag":
            out.append("E|" + esc(enc(tok[1])))
        elif kind == "Comment":
            out.append("C|" + esc(enc(tok[1])))
        else:
            raise SystemExit("unknown token kind %r" % kind)
    flush()
    return "\n".join(out) + ("\n" if out else "")


def cstr(b: bytes) -> str:
    out = []
    for ch in b:
        if ch == 0x22:
            out.append('\\"')
        elif ch == 0x5C:
            out.append("\\\\")
        elif 0x20 <= ch < 0x7F:
            if out and out[-1].startswith("\\x") and chr(ch) in "0123456789abcdefABCDEF":
                out.append('" "')
            out.append(chr(ch))
        elif ch == 0x0A:
            out.append("\\n")
        else:
            out.append("\\x%02x" % ch)
    return '"' + "".join(out) + '"'


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: gen_html5lib_tok.py <tests-dir> <out.inc>")
    tdir, outp = sys.argv[1], sys.argv[2]

    cases = []
    for path in sorted(glob.glob(os.path.join(tdir, "*.test"))):
        base = os.path.basename(path)
        with open(path, encoding="utf-8") as f:
            doc = json.load(f)
        # xmlViolationTests are a different output mode (they ask the tokenizer
        # to mangle characters so the result is well-formed XML).  We do not
        # implement that mode, so they are not ours to pass or fail.
        if "tests" not in doc:
            continue
        for t in doc["tests"]:
            inp, outp_toks = t["input"], t["output"]
            if t.get("doubleEscaped"):
                inp = undouble(inp)
                outp_toks = [[undouble(x) if isinstance(x, str) else
                              ({undouble(k): undouble(v) for k, v in x.items()}
                               if isinstance(x, dict) else x)
                              for x in tok]
                             for tok in outp_toks]
            states = t.get("initialStates") or ["Data state"]
            for st in states:
                if st not in STATES:
                    raise SystemExit("unknown initial state %r in %s" % (st, base))
                cases.append({
                    "file": base,
                    "desc": t["description"],
                    "input": enc(inp),
                    "state": STATES[st],
                    "last": t.get("lastStartTag"),
                    "expect": serialize(outp_toks),
                })

    with open(outp, "w", newline="\n") as f:
        f.write("/* GENERATED by tools/gen_html5lib_tok.py from the .test files\n"
                " * in third_party/html5lib-tests/tokenizer.  Do not edit.\n"
                " * %d cases (one per initialStates entry). */\n" % len(cases))
        f.write("struct tokcase {\n"
                "    const char *file, *desc;\n"
                "    const char *input; int inputlen;\n"
                "    int state;\n"
                "    const char *last_start_tag;\n"
                "    const char *expect;\n"
                "};\n\n")
        f.write("static const struct tokcase tokcases[] = {\n")
        for c in cases:
            f.write("{%s,%s,%s,%d,%d,%s,%s},\n" % (
                cstr(c["file"].encode()),
                cstr(c["desc"].encode("utf-8", "surrogatepass")),
                cstr(c["input"]), len(c["input"]),
                c["state"],
                cstr(c["last"].encode()) if c["last"] else "0",
                cstr(c["expect"].encode("utf-8", "surrogatepass"))))
        f.write("};\n")
        f.write("#define NTOKCASES %d\n" % len(cases))
    print("gen_html5lib_tok: %d cases -> %s" % (len(cases), outp))


if __name__ == "__main__":
    main()
