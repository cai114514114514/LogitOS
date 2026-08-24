#!/usr/bin/env python3
"""license_audit.py -- one gate for four separate claims this tree makes about
licensing, each checked against the actual files rather than trusted from a
document. Exits nonzero on the FIRST missing thing, and names it -- the same
rule CLAUDE.md states for every other gate in this tree ("Exit nonzero with
the FIRST missing thing named").

Four checks, run in this order:

  1. third_party -- every third_party/<name> directory has a license file
     beside it (checked at that directory and, for a directory that only
     groups several vendored libraries, one level into its subdirectories)
     AND is named by THIRD_PARTY.md.
  2. LICENSES/ -- every license THIRD_PARTY.md names has a full text file in
     LICENSES/. This is a CLOSED alias table (LICENSE_ALIASES below), not a
     free-text scanner: a license phrase this file does not already know
     about is a license this check cannot see, which is a real limitation,
     stated here rather than hidden behind a check that looks general and
     silently is not.
  3. tests/fixtures/* -- every fixture directory has a PROVENANCE.md, and
     every entry at that directory's TOP LEVEL (a file or a grouping
     subdirectory) is named somewhere in it. "Named" allows the two patterns
     this tree's own PROVENANCE.md files actually use for a family of
     similarly-named files -- brace alternation (`open-{a,b,c}.as`) and a
     glob (`laced-*.mkv`) -- expanded and matched against the real filename,
     not merely a plain substring search, because a substring search flags
     both of those as "missing" and it would be wrong to.
  4. /licenses on a built disk image -- every `SRC:/licenses/DEST` pair in
     the $(DISK) rule's mkfs.py invocation (Makefile, parsed with line
     continuations joined FIRST, per CLAUDE.md's own warning about reading
     this Makefile one physical line at a time) is checked two ways:
       a. SRC exists on the host. This runs always, no build needed.
       b. If a disk image already exists at $(BUILD)/disk.img, it is opened
          with a small read-only LogitFS v4 reader (below; format constants
          cross-checked against c/fs/logitfs_fmt.h, which is read-only to
          this file, never imported or duplicated as an editable copy) and
          DEST is required to exist AND match SRC byte for byte. This is the
          same "byte-for-byte, never a length check" standard the storage
          section of CLAUDE.md holds the filesystem tests to, applied to a
          license notice the same way it is applied to a user's file.

Usage:
  license_audit.py                 run all four checks in order
  license_audit.py --check NAME    run just one (third_party | licenses |
                                    fixtures | disk) -- for exercising a
                                    negative control without the earlier
                                    checks masking it
"""
import fnmatch
import itertools
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def p(*parts):
    return os.path.join(ROOT, *parts)


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def fail(msg):
    print(f"license-audit: FAIL: {msg}")
    sys.exit(1)


def ok(msg):
    print(f"license-audit: ok: {msg}")


# ---------------------------------------------------------------------------
# Check 1: third_party/<name> has a license file beside it, and a
# THIRD_PARTY.md row naming it.
# ---------------------------------------------------------------------------

LICENSE_FILE_RE = re.compile(r"^(COPYING|COPYRIGHT|LICEN[CS]E)([._-].*)?$", re.IGNORECASE)


def _has_license_file(dirpath):
    """A license-like file directly in dirpath, or one level into any of its
    subdirectories -- third_party/css has no license file of its own, only
    its three vendored libraries (libcss/, libparserutils/, libwapcaplet/)
    do, and THIRD_PARTY.md documents them as three separate components under
    one grouping directory, not one component."""
    try:
        entries = sorted(os.listdir(dirpath))
    except OSError:
        return False
    for e in entries:
        if LICENSE_FILE_RE.match(e):
            return True
    for e in entries:
        sub = os.path.join(dirpath, e)
        if os.path.isdir(sub):
            try:
                sub_entries = os.listdir(sub)
            except OSError:
                continue
            for se in sub_entries:
                if LICENSE_FILE_RE.match(se):
                    return True
    return False


def check_third_party():
    tp_dir = p("third_party")
    third_party_md = read_text(p("THIRD_PARTY.md"))
    names = sorted(
        n for n in os.listdir(tp_dir) if os.path.isdir(os.path.join(tp_dir, n))
    )
    if not names:
        fail("third_party/ has no subdirectories -- the check has nothing to look at "
             "(this almost certainly means it is being run from the wrong cwd)")
    for name in names:
        d = os.path.join(tp_dir, name)
        if not _has_license_file(d):
            fail(f"third_party/{name} has no license file beside it -- checked "
                 f"third_party/{name}/ and its immediate subdirectories for a "
                 f"name matching COPYING*/COPYRIGHT*/LICENSE*/LICENCE*")
        needle = f"third_party/{name}/"
        if needle not in third_party_md:
            fail(f"THIRD_PARTY.md does not name third_party/{name}/ -- no line "
                 f"contains the substring '{needle}'")
    ok(f"third_party: {len(names)} directories, each has a license file and a "
       f"THIRD_PARTY.md row: {', '.join(names)}")


# ---------------------------------------------------------------------------
# Check 2: every license THIRD_PARTY.md names has a full text in LICENSES/.
#
# Closed alias table by design -- see the module docstring. Each entry is
# (regex-or-plain-phrase actually used in THIRD_PARTY.md today, expected
# LICENSES/ basename).
# ---------------------------------------------------------------------------

LICENSE_ALIASES = [
    (r"\bMIT\b", "MIT.txt"),
    (r"MPL[ -]2\.0|Mozilla Public License 2\.0", "MPL-2.0.txt"),
    (r"SIL OFL 1\.1|SIL Open Font License 1\.1|\bOFL\b", "OFL-1.1.txt"),
    (r"Bitstream Vera Fonts License", "Bitstream-Vera.txt"),
    (r"BSD-3-Clause", "BSD-3-Clause.txt"),
    (r"GPLv3 or later|GPL-3\.0-or-later|GNU GPL version 3 or later",
     "GPL-3.0-or-later.txt"),
    (r"GPL-2\.0-or-later", "GPL-2.0-or-later.txt"),
    (r"Apache-2\.0", "Apache-2.0.txt"),
    (r"LGPL-2\.1|GNU Lesser General Public License[^\n]{0,40}2\.1",
     "LGPL-2.1.txt"),
]


def check_licenses():
    third_party_md = read_text(p("THIRD_PARTY.md"))
    licensing_md = read_text(p("LICENSING.md"))
    combined = third_party_md + "\n" + licensing_md
    licenses_dir = p("LICENSES")
    have = set(os.listdir(licenses_dir))
    named = []
    for pattern, basename in LICENSE_ALIASES:
        if re.search(pattern, combined):
            named.append((pattern, basename))
            if basename not in have:
                fail(f"THIRD_PARTY.md/LICENSING.md names a license matching "
                     f"/{pattern}/ but LICENSES/{basename} does not exist "
                     f"(LICENSES/ has: {', '.join(sorted(have)) or '(empty)'})")
    ok(f"LICENSES/: {len(named)} license(s) named in THIRD_PARTY.md/LICENSING.md, "
       f"each has a full text present: {', '.join(b for _, b in named)}")


# ---------------------------------------------------------------------------
# Check 3: tests/fixtures/* -- PROVENANCE.md exists and names every top-level
# entry, allowing brace-alternation and glob patterns as this tree's own
# PROVENANCE.md files actually write them.
# ---------------------------------------------------------------------------

BACKTICK_RE = re.compile(r"`([^`\n]+)`")


def _expand_braces(s):
    m = re.search(r"\{([^{}]*)\}", s)
    if not m:
        return [s]
    out = []
    for opt in m.group(1).split(","):
        out.extend(_expand_braces(s[: m.start()] + opt + s[m.end() :]))
    return out


def _token_patterns(token):
    """A backtick-quoted token from a PROVENANCE.md, expanded into concrete
    fnmatch patterns: brace groups {a,b,c} are expanded (recursively, so more
    than one group in a token works), and a bare placeholder letter used as a
    number stand-in (`video-N.m4s`) becomes a `*` wildcard alongside any `*`
    already in the token."""
    variants = _expand_braces(token)
    out = []
    for v in variants:
        v = re.sub(r"(?<![A-Za-z0-9])[N](?![A-Za-z0-9])", "*", v)
        out.append(v)
    return out


def _entry_covered(name, raw_text, patterns):
    if name in raw_text:
        return True
    for pat in patterns:
        if "*" in pat:
            if fnmatch.fnmatchcase(name, pat):
                return True
        elif pat == name:
            return True
    return False


def check_fixtures():
    fixtures_dir = p("tests", "fixtures")
    names = sorted(
        n for n in os.listdir(fixtures_dir)
        if os.path.isdir(os.path.join(fixtures_dir, n))
    )
    if not names:
        fail("tests/fixtures/ has no subdirectories -- the check has nothing "
             "to look at (wrong cwd?)")
    for name in names:
        d = os.path.join(fixtures_dir, name)
        prov_path = os.path.join(d, "PROVENANCE.md")
        if not os.path.isfile(prov_path):
            fail(f"tests/fixtures/{name}/ has no PROVENANCE.md")
        raw = read_text(prov_path)
        patterns = []
        for tok in BACKTICK_RE.findall(raw):
            if any(c.isspace() for c in tok) or "/" in tok:
                continue
            patterns.extend(_token_patterns(tok))
        entries = sorted(e for e in os.listdir(d) if e != "PROVENANCE.md")
        missing = [e for e in entries if not _entry_covered(e, raw, patterns)]
        if missing:
            fail(f"tests/fixtures/{name}/PROVENANCE.md does not name: "
                 f"{', '.join(missing)} (checked literal substring, backtick "
                 f"tokens, brace-alternation and glob patterns)")
    ok(f"tests/fixtures/: {len(names)} directories, each has a PROVENANCE.md "
       f"naming every top-level entry: {', '.join(names)}")


# ---------------------------------------------------------------------------
# Check 4: the $(DISK) rule's /licenses pairs. Source-side always; on-disk
# only if a disk image is already built.
# ---------------------------------------------------------------------------


def _joined_makefile():
    text = read_text(p("Makefile"))
    # CLAUDE.md: "If you write anything that reads the Makefile, join the
    # continuations first" -- four of the five tools that didn't were wrong.
    return re.sub(r"\\\r?\n[ \t]*", " ", text)


def _disk_license_pairs():
    joined = _joined_makefile()
    m = re.search(r"python3 tools/mkfs\.py \$\(DISK\)[^\n]*", joined)
    if not m:
        fail("could not find the `python3 tools/mkfs.py $(DISK)` recipe line "
             "in Makefile -- the $(DISK) rule this check parses may have "
             "moved or been renamed")
    pairs = re.findall(r"([^\s:]+):(/licenses/\S+)", m.group(0))
    if not pairs:
        fail("found the $(DISK) recipe but it packs nothing to /licenses/ -- "
             "either the rule changed shape or every /licenses/ line was "
             "removed")
    return pairs


def _release_notices():
    """RELEASE_NOTICES (Makefile:38-40), with its one variable reference
    ($(FONT_NOTICES)) resolved textually -- both are inside the one part of
    the Makefile this file is allowed to edit, and they are checked against
    each other for exactly that reason: a file packed to /licenses/ by the
    recipe but missing from RELEASE_NOTICES is not tracked as a $(DISK)
    prerequisite, so editing it would not trigger a disk rebuild."""
    joined = _joined_makefile()
    m_fn = re.search(r"^FONT_NOTICES\s*:=\s*(.*)$", joined, re.MULTILINE)
    font_notices = m_fn.group(1).split() if m_fn else []
    m_rn = re.search(r"^RELEASE_NOTICES\s*:=\s*(.*)$", joined, re.MULTILINE)
    if not m_rn:
        fail("could not find `RELEASE_NOTICES := ...` in Makefile")
    raw = m_rn.group(1).replace("$(FONT_NOTICES)", " ".join(font_notices))
    return set(raw.split())


# --- a minimal read-only LogitFS v4 reader, just enough to walk a path and
# read a file's bytes. Constants cross-checked by hand against
# c/fs/logitfs_fmt.h (read-only reference, not imported) and verified against
# a real build/disk.img: root /licenses listed 6 entries, /licenses/fonts 4,
# and two files (THIRD_PARTY.md, LICENSE->README.txt) read back byte-for-byte
# identical to their host originals before this script was finalized.

_BS = 4096
_DIRENT_SZ = 64
_NAME_MAX = 60
_INODE_SIZE = 128
_IPB = _BS // _INODE_SIZE
_PPB = _BS // 4
_T_FILE, _T_DIR = 1, 2
_MAGIC = 0x4C4F4749


class _LogitFS:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0)
        data = self.f.read(_BS)
        keys = [
            "magic", "version", "block_size", "total_blocks", "inode_count",
            "bitmap_start", "bitmap_blocks", "inode_start", "inode_blocks",
            "data_start", "root_ino", "log_start", "log_blocks",
        ]
        self.sb = dict(zip(keys, struct.unpack_from("<13I", data, 0)))
        if self.sb["magic"] != _MAGIC:
            raise ValueError(
                f"bad LogitFS magic {self.sb['magic']:#x}, expected {_MAGIC:#x}"
            )

    def _block(self, blkno):
        self.f.seek(blkno * _BS)
        return self.f.read(_BS)

    def _inode(self, ino):
        idx = ino  # inode numbers are 0-based (tools/mkfs.py: next_ino starts at 0)
        blk = self.sb["inode_start"] + idx // _IPB
        off = (idx % _IPB) * _INODE_SIZE
        chunk = self._block(blk)[off : off + _INODE_SIZE]
        itype, _pad, size = struct.unpack_from("<HHI", chunk, 0)
        direct = struct.unpack_from("<12I", chunk, 8)
        indirect, dindirect = struct.unpack_from("<II", chunk, 8 + 48)
        return {
            "type": itype, "size": size, "direct": direct,
            "indirect": indirect, "double_indirect": dindirect,
        }

    def _blocks(self, inode):
        for b in inode["direct"]:
            if b:
                yield b
        if inode["indirect"]:
            ptrs = struct.unpack_from(f"<{_PPB}I", self._block(inode["indirect"]), 0)
            for b in ptrs:
                if b:
                    yield b
        if inode["double_indirect"]:
            outer = struct.unpack_from(
                f"<{_PPB}I", self._block(inode["double_indirect"]), 0
            )
            for p1 in outer:
                if not p1:
                    continue
                inner = struct.unpack_from(f"<{_PPB}I", self._block(p1), 0)
                for b in inner:
                    if b:
                        yield b

    def _readdir(self, ino):
        inode = self._inode(ino)
        if inode["type"] != _T_DIR:
            raise ValueError(f"inode {ino} is not a directory (type={inode['type']})")
        remain = inode["size"]
        entries = {}
        for b in self._blocks(inode):
            if remain <= 0:
                break
            data = self._block(b)
            n = min(_BS, remain) // _DIRENT_SZ
            for i in range(n):
                chunk = data[i * _DIRENT_SZ : (i + 1) * _DIRENT_SZ]
                eino = struct.unpack_from("<I", chunk, 0)[0]
                name = chunk[4 : 4 + _NAME_MAX].split(b"\x00", 1)[0].decode(
                    "utf-8", "replace"
                )
                if eino and name and name not in (".", ".."):
                    entries[name] = eino
            remain -= min(_BS, remain)
        return entries

    def lookup(self, path):
        cur = self.sb["root_ino"]
        for part in [x for x in path.strip("/").split("/") if x]:
            entries = self._readdir(cur)
            if part not in entries:
                return None
            cur = entries[part]
        return cur

    def read_file(self, ino):
        inode = self._inode(ino)
        if inode["type"] != _T_FILE:
            raise ValueError(f"inode {ino} is not a file (type={inode['type']})")
        remain = inode["size"]
        out = bytearray()
        for b in self._blocks(inode):
            if remain <= 0:
                break
            take = min(_BS, remain)
            out += self._block(b)[:take]
            remain -= take
        return bytes(out)

    def close(self):
        self.f.close()


def check_disk():
    pairs = _disk_license_pairs()
    for src, dest in pairs:
        src_path = p(src)
        if not os.path.isfile(src_path):
            fail(f"the $(DISK) rule packs {src} to {dest}, but {src} does not "
                 f"exist on the host")
    ok(f"disk /licenses source files: {len(pairs)} pairs, every SRC exists on "
       f"the host: {', '.join(s for s, _ in pairs)}")

    notices = _release_notices()
    for src, dest in pairs:
        if src not in notices:
            fail(f"the $(DISK) rule packs {src} to {dest}, but {src} is not "
                 f"listed in RELEASE_NOTICES -- editing it would not trigger "
                 f"a disk rebuild (RELEASE_NOTICES has: "
                 f"{', '.join(sorted(notices))})")
    ok(f"RELEASE_NOTICES: every packed /licenses/ source is a tracked "
       f"$(DISK) prerequisite")

    disk_img = p("build", "disk.img")
    if not os.path.isfile(disk_img):
        print("license-audit: skip: build/disk.img not present -- "
              "the on-disk /licenses check needs a built image "
              "(`make build/disk.img`); this is not a failure, "
              "the same way an absent WPT corpus is not one.")
        return

    fs = _LogitFS(disk_img)
    try:
        for src, dest in pairs:
            ino = fs.lookup(dest)
            if ino is None:
                fail(f"build/disk.img has no file at {dest} (packed from {src} "
                     f"by the $(DISK) rule)")
            on_disk = fs.read_file(ino)
            with open(p(src), "rb") as hf:
                host = hf.read()
            if on_disk != host:
                fail(f"build/disk.img's {dest} does not match {src} byte for "
                     f"byte ({len(on_disk)} bytes on disk vs {len(host)} on "
                     f"host) -- the image is stale, rebuild it")
    finally:
        fs.close()
    ok(f"build/disk.img: {len(pairs)} /licenses files present and byte-for-byte "
       f"identical to their host source")


CHECKS = [
    ("third_party", check_third_party),
    ("licenses", check_licenses),
    ("fixtures", check_fixtures),
    ("disk", check_disk),
]


def main(argv):
    only = None
    if len(argv) >= 2 and argv[1] == "--check":
        if len(argv) < 3:
            sys.exit("usage: license_audit.py [--check third_party|licenses|fixtures|disk]")
        only = argv[2]
    ran = 0
    for name, fn in CHECKS:
        if only and name != only:
            continue
        fn()
        ran += 1
    if only and ran == 0:
        sys.exit(f"license_audit.py: no such check '{only}' -- choices: "
                  f"{', '.join(n for n, _ in CHECKS)}")
    print(f"license-audit: PASS ({ran} check{'s' if ran != 1 else ''})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
