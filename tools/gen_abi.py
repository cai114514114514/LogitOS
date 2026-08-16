#!/usr/bin/env python3
"""Generate AetherScript's view of the kernel ABI from include/abi/logit_abi.h.

AetherScript reaches the kernel through syscall(), and the syscall NUMBERS are
already an identity rather than a copy: as_native.c includes logit_abi.h and
does as_define_int("SYS_WRITE", SYS_WRITE), so they cannot drift. The struct
LAYOUTS were the half that was still a copy -- `peek32(a + 12)` for the hour
field is a number someone counted out of a header comment, and a struct the
kernel reorders turns that into a script quietly reading the wrong field.

Two outputs, and the second one is the whole point:

  fsroot/as/lib/abi.as        what AetherScript sees (layout objects)
  c/apps/as/abi_layout.inc    _Static_asserts, included by as_native.c

The offsets here are computed with LP64 rules -- but nothing is asked to trust
that. Every number is emitted twice: once for the script, and once as an
assertion that the compiler building /bin/as has to agree with. So the numbers
the script uses are not this tool's opinion of the layout, they are the target
compiler's, and a disagreement is a build failure rather than a wild read.

That leaves exactly one thing the asserts cannot catch -- a field RENAMED at the
same offset -- which is what `--check` is for: it regenerates and diffs, so the
names have to match too.

  python3 tools/gen_abi.py --write    regenerate both files
  python3 tools/gen_abi.py --check    verify they are current (read-only)
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "abi", "logit_abi.h")
CALLS = os.path.join(ROOT, "include", "abi", "logit_calls.abi")
OUT_AS = os.path.join(ROOT, "fsroot", "as", "lib", "abi.as")
OUT_INC = os.path.join(ROOT, "c", "apps", "as", "abi_layout.inc")
OUT_PACK = os.path.join(ROOT, "include", "abi", "logit_pack.h")

# LP64 (x86_64): (size, alignment, AetherScript slot kind).
#   i = signed, sign-extended on read      u = unsigned, raw bits
#   p = pointer, raw bits                  s = fixed byte span, reads as a string
SCALARS = {
    "char":               (1, 1, "i"),
    "signed char":        (1, 1, "i"),
    "unsigned char":      (1, 1, "u"),
    "short":              (2, 2, "i"),
    "unsigned short":     (2, 2, "u"),
    "int":                (4, 4, "i"),
    "unsigned":           (4, 4, "u"),
    "unsigned int":       (4, 4, "u"),
    "long":               (8, 8, "i"),
    "unsigned long":      (8, 8, "u"),
    "long long":          (8, 8, "i"),
    "unsigned long long": (8, 8, "u"),
}
PTR = (8, 8, "p")


class Unsupported(Exception):
    """A member this tool will not guess at. Refusing beats emitting a number
    nobody checked -- the whole point is that the offsets are not guesses."""


def norm_type(t):
    t = re.sub(r"\b(const|volatile|struct|register)\b", " ", t)
    return " ".join(t.split())


# The only type words this tool recognises. A member built from anything else --
# a typedef, a nested struct -- has no base type here and is refused rather than
# guessed at.
TYPE_WORDS = ("char", "signed", "unsigned", "short", "int", "long")


def parse_members(struct_name, body):
    """-> [(field, size, align, kind)] in declaration order."""
    out = []
    for decl in body.split(";"):
        decl = norm_type(decl)          # also drops const/volatile/struct
        if not decl:
            continue
        # A member is a run of type words followed by declarators:
        #   "int x, y, px, mono"      -> base "int",           rest "x, y, px, mono"
        #   "unsigned char *rgba"     -> base "unsigned char", rest "*rgba"
        #   "unsigned char mac[6]"    -> base "unsigned char", rest "mac[6]"
        toks = decl.split()
        i = 0
        while i < len(toks) and toks[i] in TYPE_WORDS:
            i += 1
        if i == 0 or i == len(toks):
            raise Unsupported("%s: cannot parse member %r" % (struct_name, decl))
        base, rest = " ".join(toks[:i]), " ".join(toks[i:])

        for d in rest.split(","):
            d = d.strip()
            if not d:
                continue
            stars = d.count("*")
            d = d.replace("*", "").strip()
            am = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*(\d+)\s*\]$", d)
            if am:
                name, n = am.group(1), int(am.group(2))
                if stars:
                    raise Unsupported("%s.%s: pointer arrays are not supported" % (struct_name, name))
                if base not in ("char", "signed char", "unsigned char"):
                    raise Unsupported("%s.%s: only byte arrays are supported (got %s[])"
                                      % (struct_name, name, base))
                out.append((name, n, 1, "s"))
                continue
            if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", d):
                raise Unsupported("%s: cannot parse declarator %r" % (struct_name, d))
            if stars:
                out.append((d, PTR[0], PTR[1], PTR[2]))
            elif base in SCALARS:
                sz, al, kind = SCALARS[base]
                out.append((d, sz, al, kind))
            else:
                raise Unsupported("%s.%s: unsupported type %r" % (struct_name, d, base))
    return out


def lay_out(members):
    """Standard C layout: pad each member up to its alignment, round the struct
    up to the widest member's alignment."""
    off, salign = 0, 1
    fields = []
    for name, size, align, kind in members:
        off = (off + align - 1) // align * align
        fields.append((name, off, size, kind))
        off += size
        salign = max(salign, align)
    return fields, (off + salign - 1) // salign * salign


def as_name(cname):
    """logit_time -> Time. Mechanical, so there is no mapping table to drift."""
    base = cname[len("logit_"):] if cname.startswith("logit_") else cname
    return base[:1].upper() + base[1:]


def read_structs():
    src = open(HEADER, encoding="utf-8").read()
    out = []
    for m in re.finditer(r"\bstruct\s+(logit_[A-Za-z0-9_]+)\s*\{([^{}]*)\}\s*;", src, re.S):
        name, body = m.group(1), m.group(2)
        body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
        body = re.sub(r"//[^\n]*", " ", body)
        fields, size = lay_out(parse_members(name, body))
        out.append((name, fields, size))
    if not out:
        raise SystemExit("gen_abi.py: no structs found in %s" % HEADER)
    return out


# --- calling conventions (include/abi/logit_calls.abi) ---------------------
# The header can only say what a syscall's arguments MEAN in a comment. This
# reads the description that says it in a checkable form, and emits both sides
# of it: the script packs, the kernel unpacks, from the same three numbers.

def parse_calls():
    """-> [(as_name, sys_symbol, [arg])] where arg is
       ('int'|'str'|'buf', name) | ('lit', literal) | ('pack', [(f, shift, width)])"""
    if not os.path.exists(CALLS):
        return []
    out = []
    for lineno, raw in enumerate(open(CALLS, encoding="utf-8"), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        # Split on top-level whitespace only: pack(a:1:2, b:0:1) has spaces inside.
        toks, depth, cur = [], 0, ""
        for ch in line:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            if ch.isspace() and depth == 0:
                if cur:
                    toks.append(cur)
                cur = ""
            else:
                cur += ch
        if cur:
            toks.append(cur)

        if toks[0] == "wait":
            if len(toks) < 4:
                raise Unsupported("%s:%d: expected `wait <name> <START> <POLL> <arg>... pending(...)`"
                                  % (CALLS, lineno))
            name, start_sym, poll_sym, rest = toks[1], toks[2], toks[3], toks[4:]
            for sym in (start_sym, poll_sym):
                if not sym.startswith("SYS_"):
                    raise Unsupported("%s:%d: %r is not a SYS_ symbol" % (CALLS, lineno, sym))
            pending, fail, args_toks = None, None, []
            for a in rest:
                m = re.match(r"^(pending|fail)\((eq|lt):(-?(?:0x)?[0-9A-Fa-f]+)\)$", a)
                if m:
                    which, op, val = m.group(1), m.group(2), int(m.group(3), 0)
                    if which == "pending":
                        pending = (op, val)
                    else:
                        fail = (op, val)
                else:
                    args_toks.append(a)
            if not pending:
                raise Unsupported("%s:%d: %s has no pending(...): nothing says when the wait is over"
                                  % (CALLS, lineno, name))
            out.append(("wait", name, start_sym, poll_sym, parse_args(CALLS, lineno, args_toks),
                        pending, fail))
            continue

        if len(toks) < 3 or toks[0] != "call":
            raise Unsupported("%s:%d: expected `call <name> <SYS_SYMBOL> <arg>...`" % (CALLS, lineno))
        name, sym, args = toks[1], toks[2], parse_args(CALLS, lineno, toks[3:])
        if not sym.startswith("SYS_"):
            raise Unsupported("%s:%d: %r is not a SYS_ symbol" % (CALLS, lineno, sym))
        if len(args) > 3:
            raise Unsupported("%s:%d: %s takes %d arguments; the kernel takes at most 3"
                              % (CALLS, lineno, name, len(args)))
        out.append(("call", name, sym, args))
    return out


def parse_args(where, lineno, toks):
    args = []
    if True:
        for a in toks:
            m = re.match(r"^(int|str|buf|lit|pack)\((.*)\)$", a)
            if not m:
                raise Unsupported("%s:%d: cannot parse argument %r" % (where, lineno, a))
            kind, body = m.group(1), m.group(2).strip()
            if kind == "pack":
                fields = []
                for f in body.split(","):
                    fm = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(\d+)\s*:\s*(\d+)\s*$", f)
                    if not fm:
                        raise Unsupported("%s:%d: pack field %r is not name:shift:width" % (where, lineno, f))
                    fields.append((fm.group(1), int(fm.group(2)), int(fm.group(3))))
                for fname, sh, w in fields:
                    if sh + w > 64:
                        raise Unsupported("%s:%d: field %s runs past bit 64" % (where, lineno, fname))
                out_fields = sorted(fields, key=lambda t: -t[1])
                for i in range(len(out_fields) - 1):           # overlapping fields = silent corruption
                    fa, sa, wa = out_fields[i]
                    fb, sb, wb = out_fields[i + 1]
                    if sb + wb > sa:
                        raise Unsupported("%s:%d: fields %s and %s overlap" % (where, lineno, fa, fb))
                args.append(("pack", fields))
            else:
                args.append((kind, body))
    return args


def call_params(args):
    """The wrapper's parameter list, in the order the fields appear."""
    ps = []
    for kind, body in args:
        if kind == "pack":
            ps.extend(f for f, _, _ in body)
        elif kind != "lit":
            ps.append(body)
    return ps


def as_arg_expr(kind, body):
    if kind == "int":
        return body
    if kind == "lit":
        return body
    if kind in ("str", "buf"):
        return "addr(%s)" % body
    # pack: mask each field to its width so an out-of-range value corrupts only
    # itself instead of the field above it (the kernel masks on the way out too).
    parts = []
    for fname, sh, w in sorted(body, key=lambda t: -t[1]):
        mask = (1 << w) - 1
        term = "(%s & 0x%X)" % (fname, mask)
        parts.append("%s << %d" % (term, sh) if sh else term)
    return "(" + ") | (".join(parts) + ")" if len(parts) > 1 else parts[0]


BANNER_PACK = """\
/* GENERATED by tools/gen_abi.py from include/abi/logit_calls.abi -- DO NOT EDIT.
 *
 * The kernel side of the packed syscall arguments. fsroot/as/lib/abi.as packs
 * these fields; these macros unpack them, and both come from the same three
 * numbers in the .abi file -- so where a field lives is written once.
 * c/kernel/gui/wm.c unpacks through these macros, which makes the convention
 * single-sourced end to end rather than merely generated on one side. */
#ifndef LOGIT_PACK_H
#define LOGIT_PACK_H
"""


def render_pack(entries):
    c = [BANNER_PACK]
    for e in entries:
        if e[0] != "call":
            continue                       # a wait's start/poll take plain arguments
        _, name, sym, args = e
        letters = "abc"
        for i, (kind, body) in enumerate(args):
            if kind != "pack":
                continue
            c.append("")
            desc = " | ".join("%s<<%d" % (f, sh) if sh else f
                              for f, sh, _ in sorted(body, key=lambda t: -t[1]))
            c.append("/* %s arg %s: %s */" % (sym, letters[i], desc))
            for fname, sh, w in sorted(body, key=lambda t: -t[1]):
                c.append("#define %s_%s_%s(v) ((int)(((unsigned long long)(v) >> %d) & 0x%XULL))"
                         % (sym.replace("SYS_", "LOGIT_"), letters[i].upper(), fname.upper(), sh, (1 << w) - 1))
    c.append("")
    c.append("#endif /* LOGIT_PACK_H */")
    c.append("")
    return "\n".join(c)


# Emitted verbatim when the description has any `wait`. The loop is written once
# here rather than generated per operation: what differs between operations is
# the protocol (which syscall polls, what pending looks like, what failure looks
# like), and that is exactly what the .abi states.
WAIT_PRELUDE = """
# ---- waiting ----
# Logit exposes these as start + poll: there is no in-kernel wait queue for them,
# so the caller spins and yields. Each call site used to write that loop itself,
# with its own retry count, and answer the same value for "failed" and "gave up".

_wait_t = Time()

# Seconds since midnight, off the RTC -- the WALL clock, hence the midnight
# wrap fixup below. monotonic_ms() exists now and is the better clock for an
# interval, but this loop deliberately stays on the wall clock: the host test
# (make test-as) runs the real abi.as with syscalls stubbed to -1, where a
# monotonic reading is a CONSTANT -1 and "now - start" never reaches the
# timeout -- the wait would hang instead of failing. A timeout loop has to
# still terminate when its clock is broken.
def now_s():
    get_time(_wait_t)
    return (_wait_t.hour * 60 + _wait_t.minute) * 60 + _wait_t.second

# The deadline is real seconds, not a retry count -- a count means a different
# amount of time on every machine and every load. The three outcomes stay
# distinct: a value is returned, a failure raises, a timeout raises something
# else. Conflating the last two is what made sys.dns() answer 0 both when a name
# did not resolve and when the loop simply ran out.
def await_(poll, pending_neg, pending_val, has_fail, fail_neg, fail_val, timeout_s, label):
    start = now_s()
    while true:
        v = poll()
        pending = v < 0 if pending_neg else v == pending_val
        if not pending:
            failed = false
            if has_fail:
                failed = v < 0 if fail_neg else v == fail_val
            if failed:
                raise label + ": failed"
            return v
        now = now_s()
        if now < start:
            now = now + 86400            # the RTC clock wrapped past midnight
        if now - start >= timeout_s:
            raise label + ": timed out after " + str(timeout_s) + "s"
        sys_yield()
"""


BANNER_AS = """\
# GENERATED by tools/gen_abi.py from include/abi/logit_abi.h -- DO NOT EDIT.
#
# The kernel's structs, as AetherScript layouts. `t = Time()` allocates one the
# collector owns; `addr(t)` is exactly the pointer a syscall expects; `t.hour`
# reads the field at the offset below.
#
# These offsets are not this file's opinion of the layout. Each one is also
# emitted as a _Static_assert in c/apps/as/abi_layout.inc, which as_native.c
# includes -- so the compiler that builds /bin/as checks every number here
# against the real struct, and a kernel that reorders a field breaks the build
# instead of leaving a script reading the wrong bytes.
#
# The call wrappers below come from include/abi/logit_calls.abi, which is the one
# place the calling convention is written: the same field positions generate the
# kernel-side unpack macros in include/abi/logit_pack.h. `(x << 16) | y` used to
# live in a header comment, in gui.as, and in wm.c -- three copies of a rule
# nothing checked.
#
# Regenerate:  python3 tools/gen_abi.py --write
# Verify:      make check-abi
"""

BANNER_INC = """\
/* GENERATED by tools/gen_abi.py from include/abi/logit_abi.h -- DO NOT EDIT.
 *
 * Every offset and size that fsroot/as/lib/abi.as hands to layout(), asserted
 * against the real struct. Included by as_native.c, which already includes
 * logit_abi.h, so this is checked by BOTH the host build and the x86_64-elf
 * target build of /bin/as.
 *
 * A failure here means abi.as is stale, not that the kernel is wrong:
 *     python3 tools/gen_abi.py --write
 *
 * (A field renamed at the same offset passes these; `make check-abi` catches
 * that by regenerating and diffing the names.) */
"""


def render():
    structs = read_structs()
    calls = parse_calls()

    a = [BANNER_AS]
    for cname, fields, size in structs:
        a.append("")
        a.append("%s = layout(\"%s\", %d, [" % (as_name(cname), cname, size))
        for i, (fname, off, fsize, kind) in enumerate(fields):
            comma = "," if i + 1 < len(fields) else ""
            a.append("    [\"%s\", %d, %d, \"%s\"]%s" % (fname, off, fsize, kind, comma))
        a.append("])")

    plain = [e for e in calls if e[0] == "call"]
    waits = [e for e in calls if e[0] == "wait"]

    if plain:
        a.append("")
        a.append("# ---- calls (include/abi/logit_calls.abi) ----")
        for _, name, sym, args in plain:
            a.append("")
            a.append("def %s(%s):" % (name, ", ".join(call_params(args))))
            exprs = [sym] + [as_arg_expr(k, b) for k, b in args]
            a.append("    return syscall(%s)" % ", ".join(exprs))

    if waits:
        a.append(WAIT_PRELUDE.rstrip("\n"))
        for _, name, start_sym, poll_sym, args, pending, fail in waits:
            params = call_params(args)
            a.append("")
            a.append("def %s_start(%s):" % (name, ", ".join(params)))
            exprs = [start_sym] + [as_arg_expr(k, b) for k, b in args]
            a.append("    return syscall(%s)" % ", ".join(exprs))
            a.append("")
            a.append("def %s_poll():" % name)
            a.append("    return syscall(%s)" % poll_sym)
            a.append("")
            a.append("def wait_%s(%s):" % (name, ", ".join(params + ["timeout_s"])))
            a.append("    if %s_start(%s) < 0:" % (name, ", ".join(params)))
            a.append("        raise \"%s: cannot start\"" % name)
            a.append("    return await_(%s_poll, %s, %d, %s, %s, %d, timeout_s, \"%s\")"
                     % (name,
                        "true" if pending[0] == "lt" else "false", pending[1],
                        "true" if fail is not None else "false",
                        "true" if fail is not None and fail[0] == "lt" else "false",
                        fail[1] if fail is not None else 0,
                        name))
    a.append("")

    c = [BANNER_INC]
    for cname, fields, size in structs:
        c.append("")
        c.append("_Static_assert(sizeof(struct %s) == %d,\n"
                 "               \"abi.as is stale: sizeof(struct %s) changed -- run tools/gen_abi.py --write\");"
                 % (cname, size, cname))
        for fname, off, fsize, kind in fields:
            c.append("_Static_assert(offsetof(struct %s, %s) == %d,\n"
                     "               \"abi.as is stale: %s.%s moved -- run tools/gen_abi.py --write\");"
                     % (cname, fname, off, cname, fname))
            c.append("_Static_assert(sizeof(((struct %s *)0)->%s) == %d,\n"
                     "               \"abi.as is stale: %s.%s changed width -- run tools/gen_abi.py --write\");"
                     % (cname, fname, fsize, cname, fname))
    c.append("")
    return "\n".join(a), "\n".join(c), render_pack(calls)


OUT_NATIVE = os.path.join(ROOT, "c", "apps", "as", "as_native.c")


def undefined_sys_symbols(text_as):
    """SYS_* names a generated wrapper calls that as_native.c never defines.

    THE HOLE THIS CLOSES, and it was open for eighteen calls. A wrapper in
    abi.as is `syscall(SYS_SOCK_OPEN, ...)`, and SYS_SOCK_OPEN is an ordinary
    AetherScript GLOBAL that as_native.c installs with as_define_int(). That
    list is hand-maintained -- its own comment says "keep this list in step
    with include/abi/logit_calls.abi" -- and a `call` line added here without
    the matching define compiles, links, ships, and then fails at the FIRST
    CALL with `undefined variable 'SYS_SOCK_OPEN'`, which is exactly what a
    misspelled variable in the script looks like. Nothing between writing the
    call and running it says a word.

    So the two lists are diffed here instead. This is a NAME check, not an
    offset check: the numbers were never the risk (as_native.c hands the
    kernel's own SYS_* through, which is the identity the header comment
    describes) -- the risk is a name that has no binding at all.

    Deliberately one-directional. as_native.c defining a symbol no wrapper
    uses is fine and common (SYS_LSEEK, SYS_CLOSE and friends are there for
    scripts that call syscall() by hand), so an extra define is not an error.
    A wrapper naming a symbol nobody defines always is."""
    used = sorted(set(re.findall(r"\bSYS_[A-Z0-9_]+", text_as)))
    try:
        nat = open(OUT_NATIVE, encoding="utf-8", newline="").read()
    except OSError:
        return []                       # no as_native.c to check against
    have = set(re.findall(r'as_define_int\(\s*"(SYS_[A-Z0-9_]+)"', nat))
    return [u for u in used if u not in have]


def main(argv):
    mode = argv[1] if len(argv) > 1 else "--check"
    if mode not in ("--check", "--write"):
        raise SystemExit(__doc__)
    try:
        text_as, text_inc, text_pack = render()
    except Unsupported as e:
        raise SystemExit("gen_abi.py: %s\n  (refusing to guess -- teach the generator or "
                         "change the struct)" % e)

    # Reported on --write too, and not only on --check: the person who just
    # added a `call` line is the one who can fix it in ten seconds, and they
    # are standing right here. --write still succeeds (the files it wrote are
    # correct); --check below is where it is fatal.
    missing = undefined_sys_symbols(text_as)

    if mode == "--write":
        for path, text in ((OUT_AS, text_as), (OUT_INC, text_inc), (OUT_PACK, text_pack)):
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            print("gen_abi.py: wrote %s" % os.path.relpath(path, ROOT))
        if missing:
            print("gen_abi.py: WARNING -- %d wrapper(s) name a SYS_* that "
                  "as_native.c never defines:" % len(missing))
            print("  " + " ".join(missing))
            print("  each one is an 'undefined variable' at its FIRST CALL, not a build error.")
            print("  add as_define_int(\"NAME\", NAME) in as_install_indirection().")
        return 0

    bad = []
    for path, text in ((OUT_AS, text_as), (OUT_INC, text_inc), (OUT_PACK, text_pack)):
        cur = open(path, encoding="utf-8", newline="").read() if os.path.exists(path) else None
        if cur != text:
            bad.append(os.path.relpath(path, ROOT))
    if bad:
        print("check-abi: STALE -- %s does not match include/abi/{logit_abi.h,logit_calls.abi}"
              % ", ".join(bad))
        print("  regenerate with: python3 tools/gen_abi.py --write")
        return 1
    if missing:
        print("check-abi: UNBOUND -- %d generated wrapper(s) call a SYS_* that "
              "c/apps/as/as_native.c never defines as a script global:"
              % len(missing))
        print("  " + " ".join(missing))
        print("  These COMPILE and SHIP. The failure is at the wrapper's first")
        print("  call, and it reads 'undefined variable' -- indistinguishable")
        print("  from a typo in the calling script.")
        print("  fix: as_define_int(\"NAME\", NAME) in as_install_indirection().")
        return 1
    print("check-abi: ok (%d kernel structs, %d calls, %d SYS_* names bound; "
          "offsets also asserted at compile time)"
          % (len(read_structs()), len(parse_calls()),
             len(set(re.findall(r"\bSYS_[A-Z0-9_]+", text_as)))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
