#!/usr/bin/env python3
"""Build Aether's redistributable UI and terminal font subsets.

The checked-in sources are SIL-OFL Noto variable TrueType fonts.  This tool
pins them to regular instances, subsets them to the character inventory used by
Aether, and gives the modified fonts distinct internal names.  Output remains
plain TrueType (glyf outlines) so c/kernel/gui/ttf.c can parse it.

Usage: mkfont.py [--ui-src FONT] [--mono-src FONT] <ui.ttf> <mono.ttf>
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


def _set_names(font, family, description, copyright_text, license_text,
               license_url):
    if "name" not in font:
        font["name"] = newTable("name")
    names = font["name"]
    names.names = []
    postscript = family.replace(" ", "") + "-Regular"
    values = {
        0: copyright_text,
        1: family,
        2: "Regular",
        3: f"Aether OS:{family}:1.0",
        4: f"{family} Regular",
        5: "Version 1.0; Aether OS subset",
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


def subset(src, face, unicodes, out, family, source_family, axes):
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
        f"Aether OS character subset derived from {source_family}.",
        copyright_text,
        license_text,
        license_url,
    )
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
    args = parser.parse_args()

    ui_set = gb2312_unicodes() | latin_punct()
    subset(args.ui_src, args.ui_face, ui_set, args.out_ui,
           "Aether UI", "Noto Sans SC", {"wght": 400})
    subset(args.mono_src, args.mono_face,
           set(range(0x20, 0x7F)) | {0x00A0}, args.out_mono,
           "Aether Mono", "Noto Sans Mono", {"wght": 400, "wdth": 100})
    print(f"ui:   {args.out_ui}  {os.path.getsize(args.out_ui)//1024} KiB  ({len(ui_set)} codepoints)",
          file=sys.stderr)
    print(f"mono: {args.out_mono}  {os.path.getsize(args.out_mono)//1024} KiB", file=sys.stderr)

if __name__ == "__main__":
    main()
