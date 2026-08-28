#!/usr/bin/env python3
"""Build Logit's redistributable UI and terminal font subsets.

The checked-in sources are SIL-OFL Noto variable TrueType fonts.  This tool
pins them to fixed instances, subsets them to the character inventory used by
Logit, and gives the modified fonts distinct internal names.  Output remains
plain TrueType (glyf outlines) so c/kernel/gui/ttf.c can parse it.

FOUR OUTPUTS, TWO WEIGHTS, AND WHY THERE IS NO ITALIC.  Both sources are
variable fonts whose only axes are `wght` (and `wdth` on Mono).  Measured:

    NotoSansSC-VF.ttf   wght 100..900 (default 100)
    NotoSansMono-VF.ttf wght 100..900 (default 400), wdth 62.5..100

So a Bold face is an INSTANCE of a source already vendored -- same file, same
OFL licence, same pipeline -- and it is generated here at wght=700 rather than
being a second download.  There is no `ital` and no `slnt` axis in either
source, so an italic face is NOT derivable from what is vendored: it would need
a separate upstream file (Noto Sans SC ships no italic at all; Noto Sans Mono
ships none either).  Synthesising one by shearing the regular outlines is
deliberately not done -- a shear is not what a designer draws, it breaks every
vertical stem's contrast and, on this machine specifically, it would have to
happen inside the glyph rasteriser (c/lib/text/glyphras.c) whose output is
scored against an independent oracle by `make test-glyph-agree`; a sheared
glyph is by construction a mismatch against that oracle, so the one instrument
that says the rasteriser is correct would have to be turned off to ship it.

Usage: mkfont.py [--ui-src FONT] [--mono-src FONT]
                 <ui.ttf> <mono.ttf> <ui-bold.ttf> <mono-bold.ttf>
"""
import argparse
import os
import sys
from pathlib import Path

from fontTools.ttLib import TTFont, newTable
from fontTools.subset import Subsetter, Options
from fontTools.varLib.instancer import instantiateVariableFont

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_UI_SRC = ROOT / "third_party/fonts/NotoSansSC-VF.ttf"
DEFAULT_MONO_SRC = ROOT / "third_party/fonts/NotoSansMono-VF.ttf"

def gb2312_unicodes():
    cps = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                cps.add(ord(bytes([hi, lo]).decode("gb2312")))
            except Exception:
                pass
    return cps

def latin_punct():
    cps = set(range(0x20, 0x7F))                 # ASCII printable
    cps |= set(range(0x3000, 0x3040))            # CJK symbols & punctuation
    cps |= set(range(0xFF01, 0xFFA0))            # fullwidth + halfwidth kana
    cps |= set(range(0xFFE0, 0xFFE7))            # fullwidth currency/symbols
    cps |= {0x00A0, 0x00B7, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026, 0x2022}
    return cps

def _name_values(font, name_id):
    values = []
    if "name" not in font:
        return values
    for record in font["name"].names:
        if record.nameID != name_id:
            continue
        try:
            value = record.toUnicode().strip()
        except UnicodeDecodeError:
            continue
        if value and value not in values:
            values.append(value)
    return values


def _set_names(font, family, style, description, copyright_text, license_text,
               license_url):
    if "name" not in font:
        font["name"] = newTable("name")
    names = font["name"]
    names.names = []
    postscript = family.replace(" ", "") + "-" + style
    values = {
        0: copyright_text,
        1: family,
        2: style,
        # The unique ID carries the style only when there is one to
        # distinguish, so the Regular pair's bytes -- and therefore the hashes
        # `make verify-fonts` checks and the 2.2 MB blob in git -- are exactly
        # what they were before this file learned about weights.
        3: f"Logit OS:{family}:1.0" if style == "Regular"
           else f"Logit OS:{family} {style}:1.0",
        4: f"{family} {style}",
        5: "Version 1.0; Logit OS subset",
        6: postscript,
        10: description,
        13: license_text,
        14: license_url,
    }
    # Windows Unicode and Unicode-platform records keep the metadata visible to
    # common inspection tools without reusing an upstream family name.
    for name_id, value in values.items():
        names.setName(value, name_id, 3, 1, 0x409)
        names.setName(value, name_id, 0, 3, 0)


def _set_weight(font, style, weight_class):
    """Make the face's own tables agree with the name table about its weight.

    The kernel reads neither OS/2 nor head.macStyle -- c/kernel/gui/text.c picks
    a face by file path, and c/lib/text/ttf.c reads hhea for metrics and glyf
    for outlines.  These are set anyway because every INSPECTION tool does read
    them: `make test-font` scores our outlines against FreeType, and a file
    whose name table says Bold while OS/2 says 400 is the kind of disagreement
    that gets diagnosed as a bug in the parser rather than in the asset.
    """
    if "OS/2" in font:
        os2 = font["OS/2"]
        os2.usWeightClass = weight_class
        # fsSelection bit 5 = BOLD, bit 6 = REGULAR; they are mutually exclusive.
        os2.fsSelection = (os2.fsSelection & ~((1 << 5) | (1 << 6)))
        os2.fsSelection |= (1 << 5) if style == "Bold" else (1 << 6)
    if "head" in font:
        # head.macStyle bit 0 = bold.
        font["head"].macStyle = (font["head"].macStyle & ~1) | (1 if style == "Bold" else 0)


def subset(src, face, unicodes, out, family, style, weight_class,
           source_family, axes):
    if not os.path.exists(src):
        raise SystemExit(
            f"ERROR: source font not found: {src}\n"
            "       restore third_party/fonts or pass --ui-src/--mono-src"
        )
    f = TTFont(src, fontNumber=face, recalcTimestamp=False)
    copyright_text = " / ".join(_name_values(f, 0))
    license_text = " / ".join(_name_values(f, 13))
    license_url = " / ".join(_name_values(f, 14))
    if "fvar" in f:
        location = {
            axis.axisTag: axes.get(axis.axisTag, axis.defaultValue)
            for axis in f["fvar"].axes
        }
        instantiateVariableFont(f, location, inplace=True)

    available = set((f.getBestCmap() or {}).keys())
    missing = unicodes - available
    if missing:
        sample = ", ".join(f"U+{cp:04X}" for cp in sorted(missing)[:8])
        raise SystemExit(
            f"ERROR: {src} lacks {len(missing)} requested codepoints ({sample})"
        )

    opt = Options()
    opt.glyph_names = False
    opt.recalc_bounds = True
    opt.drop_tables += [
        "BASE", "GDEF", "GPOS", "GSUB", "DSIG", "HVAR", "MVAR", "STAT",
        "avar", "feat", "fvar", "gvar", "kerx", "morx", "vhea", "vmtx",
    ]
    opt.name_IDs = []
    ss = Subsetter(options=opt)
    ss.populate(unicodes=sorted(unicodes))
    ss.subset(f)
    if "glyf" not in f:
        raise SystemExit(
            f"ERROR: {src} face {face} is not glyf-based (CFF/OTF unsupported)"
        )
    _set_names(
        f,
        family,
        style,
        f"Logit OS character subset derived from {source_family}." if style == "Regular"
        else f"Logit OS character subset derived from {source_family} {style}.",
        copyright_text,
        license_text,
        license_url,
    )
    _set_weight(f, style, weight_class)
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    f.save(out)
    return out

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--ui-src", default=os.environ.get("MKFONT_UI_SRC", DEFAULT_UI_SRC)
    )
    parser.add_argument(
        "--mono-src", default=os.environ.get("MKFONT_MONO_SRC", DEFAULT_MONO_SRC)
    )
    parser.add_argument(
        "--ui-face", type=int, default=int(os.environ.get("MKFONT_UI_FACE", "0"))
    )
    parser.add_argument(
        "--mono-face", type=int,
        default=int(os.environ.get("MKFONT_MONO_FACE", "0"))
    )
    parser.add_argument("out_ui")
    parser.add_argument("out_mono")
    parser.add_argument("out_ui_bold")
    parser.add_argument("out_mono_bold")
    args = parser.parse_args()

    # BOTH WEIGHTS SUBSET TO THE SAME CODEPOINTS, on purpose. A bold face that
    # covered less than the regular one would make <strong>中文</strong> fall
    # back per character, so half a run would be bold and half would not -- a
    # failure that looks like a shaping bug and is an asset bug.
    ui_set = gb2312_unicodes() | latin_punct()
    mono_set = set(range(0x20, 0x7F)) | {0x00A0}
    outs = [
        (args.ui_src, args.ui_face, ui_set, args.out_ui, "Logit UI",
         "Regular", 400, "Noto Sans SC", {"wght": 400}),
        (args.mono_src, args.mono_face, mono_set, args.out_mono, "Logit Mono",
         "Regular", 400, "Noto Sans Mono", {"wght": 400, "wdth": 100}),
        (args.ui_src, args.ui_face, ui_set, args.out_ui_bold, "Logit UI",
         "Bold", 700, "Noto Sans SC", {"wght": 700}),
        (args.mono_src, args.mono_face, mono_set, args.out_mono_bold,
         "Logit Mono", "Bold", 700, "Noto Sans Mono",
         {"wght": 700, "wdth": 100}),
    ]
    for src, face, cps, out, family, style, wc, source, axes in outs:
        subset(src, face, cps, out, family, style, wc, source, axes)
        print(f"{family} {style}: {out}  {os.path.getsize(out)//1024} KiB"
              f"  ({len(cps)} codepoints)", file=sys.stderr)

if __name__ == "__main__":
    main()
