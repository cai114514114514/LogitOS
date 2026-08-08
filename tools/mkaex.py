#!/usr/bin/env python3
"""Build a .aex -- the Logit native executable.

Two modes.

  WRAP (the default, and what the Makefile calls ~30 times):
      mkaex.py <in.elf> <out.aex> <name> [ext] [icon] [r] [g] [b] [options]
  The positional form is byte-for-byte the v1 command line, on purpose: every
  call site in the Makefile keeps working and the options below are additive.

  EMIT (`--emit`), for a COMPILER rather than a linker:
      mkaex.py --emit <out.aex> <name> --base 0x50000000 --text code.bin
               [--rodata ro.bin] [--data d.bin] [--bss BYTES] [--entry-off N]
  Builds a minimal, valid ELF64 out of flat blobs and wraps it. This exists for
  docs/superpowers/specs/...-aetherscript-2 §P4/M30 -- `as -c --native` emitting
  a real ring-3 binary. See the note above emit_elf() for why the answer to
  "design the format so a compiler can emit it" is a tool that emits an ELF and
  not a container with its own segment table; the short version is that an ELF
  from a from-scratch code generator can be read by readelf and diffed against
  a known-good one, and a private format cannot.

The file:  [ 64-byte fixed header ][ TLV metadata ][ ELF64 image ]
c/kernel/exec/aex.h is the definition site; this mirrors it, and
tests/unit/exec_test.c asserts the two agree against a real file.
"""
import sys, struct, os, zlib, argparse

AEX_VERSION = 2
HDR_FIXED   = 64
F_GUI, F_CLI = 0x0001, 0x0002
ARCH_X86_64, ABI_LOGIT1 = 1, 1
CATS = {"none": 0, "system": 1, "media": 2, "dev": 3, "net": 4, "util": 5, "test": 6}
T_CRC32 = 0x43524341   # "ACRC"
T_APPID = 0x44495841   # "AXID"
T_TYPES = 0x50595441   # "ATYP"

# The CLI programs all link here (see the CLI_RULE note in the Makefile); a GUI
# app gets its own base below it. That is the only signal in the file that says
# which kind of program this is, and it is a real one -- it is where the thing
# was actually linked, not a label somebody typed.
CLI_BASE = 0x50000000


def die(msg):
    sys.exit("mkaex: " + msg)


def u8arg(s, what):
    v = int(s, 0)
    if not 0 <= v <= 255:
        die(f"{what}={v} out of range 0..255")
    return v


# --- the icon colour ---------------------------------------------------------
# (0,0,0) means "pick from a 7-entry palette by scan order" in c/kernel/gui/wm.c,
# and scan order is the order the files landed on the disk -- so adding an app
# CHANGES THE COLOUR OF THE ONES AFTER IT. wm.c belongs to another line, so the
# fix here is to stop producing the input that triggers it: a file that does not
# name a colour gets one derived from its identity, which is stable for as long
# as the app is called what it is called. (0,0,0) still means the palette, for a
# foreign .aex this tool did not build.
PALETTE = [(80, 140, 255), (55, 200, 120), (255, 92, 92), (255, 170, 40),
           (170, 110, 255), (40, 200, 220), (255, 120, 170)]


def stable_colour(app_id):
    h = 2166136261
    for c in app_id.encode():
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return PALETTE[h % len(PALETTE)]


def tlv(tag, payload):
    rec = struct.pack("<II", tag, len(payload)) + payload
    return rec + b"\0" * ((-len(rec)) % 8)


def elf_entry_and_shape(data):
    """(entry, lowest user-region PT_LOAD vaddr) of an ELF64 image."""
    if len(data) < 64 or data[:4] != b"\x7fELF":
        die("input is not an ELF64 image")
    entry, phoff = struct.unpack_from("<QQ", data, 24)
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    base = None
    for i in range(phnum):
        pt, pf, po, pv, pp, pfs, pms, pa = struct.unpack_from("<II6Q", data, phoff + i * phentsize)
        if pt == 1 and pv >= 0x40000000:
            base = pv if base is None else min(base, pv)
    return entry, base


def build(elf_bytes, name, ext, icon, rgb, opts):
    entry, base = elf_entry_and_shape(elf_bytes)
    if base is None:
        die("the ELF has no PT_LOAD in the private user region (0x40000000+)")

    app_id = opts.id or ("os.logit." + os.path.basename(opts.out).rsplit(".", 1)[0])

    if opts.gui:
        flags = F_GUI
    elif opts.cli:
        flags = F_CLI
    else:
        flags = F_CLI if base == CLI_BASE else F_GUI

    if opts.category:
        category = CATS[opts.category]
    else:
        category = CATS["util"] if (flags & F_CLI) else CATS["none"]

    r, g, b = rgb
    if (r, g, b) == (0, 0, 0):
        r, g, b = stable_colour(app_id)

    body = tlv(T_CRC32, struct.pack("<I", zlib.crc32(elf_bytes) & 0xFFFFFFFF))
    body += tlv(T_APPID, app_id.encode() + b"\0")
    if opts.types:
        ids = [int(t, 0) for t in opts.types.split(",") if t.strip()]
        body += tlv(T_TYPES, struct.pack("<%dH" % len(ids), *ids))
    hdr_size = HDR_FIXED + len(body)
    if hdr_size > 4096:
        die("the metadata region is larger than the 4096-byte cap the loader reads")

    hdr = bytearray(HDR_FIXED)
    hdr[0:4] = b"AEX1"
    struct.pack_into("<HH", hdr, 4, AEX_VERSION, flags)
    # Truncate to 31 bytes on a UTF-8 character boundary (decode + drop the
    # partial tail sequence), never mid-codepoint.
    nb = name.encode()[:31].decode("utf-8", "ignore").encode()
    hdr[8:8 + len(nb)] = nb
    eb = ext.encode()[:7]
    hdr[40:40 + len(eb)] = eb
    hdr[48] = ord(icon[0]) if icon else 0
    hdr[49], hdr[50], hdr[51] = r, g, b
    struct.pack_into("<HH", hdr, 52, hdr_size, opts.stack_pages)
    hdr[56], hdr[57], hdr[58], hdr[59] = ARCH_X86_64, ABI_LOGIT1, category, opts.sort
    struct.pack_into("<I", hdr, 60, len(elf_bytes))

    if opts.v1:
        # The NEGATIVE CONTROL's input, and nothing else builds one. v1 is the
        # 64-byte wrapper with pad[12] zeroed and an ELF at a hardcoded +64:
        # tests/exec.mk requires the loader to accept it DELIBERATELY -- with
        # the line on the log that says it carries no integrity record -- rather
        # than either refusing a stale disk or loading it as if it were current.
        hdr = bytearray(HDR_FIXED)
        hdr[0:4] = b"AEX1"
        struct.pack_into("<HH", hdr, 4, 1, 0)
        hdr[8:8 + len(nb)] = nb
        hdr[40:40 + len(eb)] = eb
        hdr[48] = ord(icon[0]) if icon else 0
        hdr[49], hdr[50], hdr[51] = r, g, b
        return bytes(hdr) + elf_bytes, entry, base, app_id, flags, hdr_size

    return bytes(hdr) + body + elf_bytes, entry, base, app_id, flags, hdr_size


# --- EMIT: a minimal ELF64 from flat blobs ----------------------------------
# WHY THIS IS AN ELF AND NOT AN AEX-NATIVE SEGMENT TABLE. The AetherScript spec
# asks for `as -c --native` to emit a real .aex, "legitimate precisely because
# we own the ABI, the ELF/aex loader, and the page tables". Owning the format
# would make emitting it about equally easy -- three structs either way -- so
# that ask does not settle the question. What settles it is the day after: an
# ELF a compiler emitted can be run through readelf, disassembled by objdump,
# and diffed against the output of a linker that is known to work. A private
# container can be read by exactly one program, which is the one under
# suspicion. So the compiler's contract is: hand over flat code, flat rodata,
# flat data, a bss size and an entry offset, and get a debuggable binary back.
#
# The image this builds is deliberately the SIMPLEST thing the loader accepts,
# so it is also the loader's own smoke test -- tests/exec.mk emits one and runs
# it on the machine.
PT_LOAD, PT_GNU_STACK = 1, 0x6474e551
PF_X, PF_W, PF_R = 1, 2, 4


def emit_elf(base, entry_off, text, rodata, data, bss):
    segs = []                     # (flags, vaddr, bytes, memsz)
    va = base
    segs.append((PF_R | PF_X, va, text, len(text)))
    va = (va + len(text) + 0xFFF) & ~0xFFF
    if rodata:
        segs.append((PF_R, va, rodata, len(rodata)))
        va = (va + len(rodata) + 0xFFF) & ~0xFFF
    if data or bss:
        segs.append((PF_R | PF_W, va, data, len(data) + bss))

    nph = len(segs) + 1           # + PT_GNU_STACK
    # Segment file offsets start at 0x1000 and stay congruent to their vaddrs
    # modulo the page size, which is what the ELF spec requires and what the
    # loader now checks. Every vaddr here is page aligned, so page-aligned file
    # offsets satisfy it.
    off = 0x1000
    placed = []
    for flags, vaddr, blob, memsz in segs:
        placed.append((flags, vaddr, off, blob, memsz))
        off = (off + len(blob) + 0xFFF) & ~0xFFF

    eh = bytearray(64)
    eh[0:16] = b"\x7fELF\x02\x01\x01\x00" + b"\0" * 8
    struct.pack_into("<HHI", eh, 16, 2, 62, 1)              # ET_EXEC, EM_X86_64, EV_CURRENT
    struct.pack_into("<QQQ", eh, 24, base + entry_off, 64, 0)
    struct.pack_into("<I", eh, 48, 0)                       # e_flags: none exist on x86-64
    struct.pack_into("<6H", eh, 52, 64, 56, nph, 0, 0, 0)

    ph = b""
    for flags, vaddr, foff, blob, memsz in placed:
        ph += struct.pack("<II6Q", PT_LOAD, flags, foff, vaddr, vaddr,
                          len(blob), memsz, 0x1000)
    ph += struct.pack("<II6Q", PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 0, 0)

    img = bytearray(bytes(eh) + ph)
    for flags, vaddr, foff, blob, memsz in placed:
        if len(img) < foff:
            img += b"\0" * (foff - len(img))
        img[foff:foff + len(blob)] = blob
    return bytes(img)


def main():
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--v1", action="store_true", help="emit a v1 file (the negative control)")
    ap.add_argument("--gui", action="store_true")
    ap.add_argument("--cli", action="store_true")
    ap.add_argument("--id", default=None, help="stable app id, e.g. os.logit.browser")
    ap.add_argument("--category", choices=sorted(CATS), default=None)
    ap.add_argument("--sort", type=int, default=0)
    ap.add_argument("--stack-pages", dest="stack_pages", type=int, default=0)
    ap.add_argument("--types", default=None, help="comma-separated logit_sniff SN_* ids")
    # --emit only
    ap.add_argument("--base", default="0x50000000")
    ap.add_argument("--entry-off", dest="entry_off", default="0")
    ap.add_argument("--text", default=None)
    ap.add_argument("--rodata", default=None)
    ap.add_argument("--data", default=None)
    ap.add_argument("--bss", default="0")
    ap.add_argument("rest", nargs="*")
    opts = ap.parse_args()

    if opts.emit:
        if len(opts.rest) < 2:
            die("--emit needs <out.aex> <name>")
        if not opts.text:
            die("--emit needs --text <file>")
        out, tail = opts.rest[0], opts.rest[1:]
        rd = open(opts.rodata, "rb").read() if opts.rodata else b""
        dt = open(opts.data, "rb").read() if opts.data else b""
        elf = emit_elf(int(opts.base, 0), int(opts.entry_off, 0),
                       open(opts.text, "rb").read(), rd, dt, int(opts.bss, 0))
    else:
        if len(opts.rest) < 3:
            die("usage: mkaex.py <in.elf> <out.aex> <name> [ext] [icon] [r] [g] [b]")
        out, tail = opts.rest[1], opts.rest[2:]
        elf = open(opts.rest[0], "rb").read()

    # From here both modes see the same tail: name [ext] [icon] [r] [g] [b].
    def at(i, dflt=""):
        return tail[i] if len(tail) > i else dflt
    name = at(0)
    ext = at(1)
    icon = at(2)
    if ext == "-":                       # sentinel for "no extension"
        ext = ""
    r = u8arg(at(3, "0"), "r")
    g = u8arg(at(4, "0"), "g")
    b = u8arg(at(5, "0"), "b")

    opts.out = out
    blob, entry, base, app_id, flags, hdr_size = build(elf, name, ext, icon, (r, g, b), opts)
    with open(out, "wb") as f:
        f.write(blob)
    kind = "cli" if flags & F_CLI else "gui"
    print("mkaex: %s  v%d %s '%s' ext='%s' id=%s base=0x%x hdr=%d elf=%d"
          % (out, 1 if opts.v1 else AEX_VERSION, kind, name, ext, app_id,
             base, hdr_size if not opts.v1 else 64, len(elf)))


if __name__ == "__main__":
    main()
