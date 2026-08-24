# Provenance: tests/fixtures/fonts

`.gitattributes` in this directory is repository hygiene, not vendored data:
it marks `*.otf`/`*.ttf` as `binary` so Git's CRLF handling never touches the
font fixtures below (see the project's own CRLF-worktree caution). It has no
provenance of its own to record.

## Classification: B — third-party fonts under permissive licences, all already named (with varying accuracy) in the directory's own `README.md`

None of these are captures of a live site; all five are (subsets of) real,
publicly released font files, modified only by `pyftsubset` to drop glyphs
(the README: "Subsets were produced with `pyftsubset --layout-features='*'
--no-hinting --notdef-outline`, which keeps the layout tables and the
outline flavour intact while dropping glyphs; the resulting files are
modified font software and carry their upstream name/licence records.").
None ship on the LogitOS disk image (the README: "None of these ship on the
disk image").

## What travels with the files today

Only prose in `README.md`, which quotes no licence text and — as this pass
found — is wrong about one licence. No `LICENSE`/`OFL.txt` file exists
anywhere under `tests/fixtures/` (`find tests/fixtures -iname LICENSE*`
returns nothing).

## Verified against each upstream source (not recalled)

### `SourceSans3-Regular.otf` — SIL OFL 1.1, correctly named

Source: Adobe's `adobe-fonts/source-sans`, `release` branch,
`LICENSE.md` (`https://raw.githubusercontent.com/adobe-fonts/source-sans/
release/LICENSE.md`). Header:

```
Copyright 2010-2024 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'. All Rights Reserved. Source is a trademark of Adobe in the United States and/or other countries.

This Font Software is licensed under the SIL Open Font License, Version 1.1.
```

### `cid-cff-subset.otf` — SIL OFL 1.1, correctly named

Source: subset of `adobe-fonts/source-han-sans`, `release` branch,
`LICENSE.txt`. Header:

```
Copyright 2014-2025 Adobe (http://www.adobe.com/), with Reserved Font
Name 'Source'. Source is a trademark of Adobe in the United States
and/or other countries.

This Font Software is licensed under the SIL Open Font License,
Version 1.1.
```

### `cbdt-emoji-subset.ttf` — SIL OFL 1.1, correctly named

Source: subset of `googlefonts/noto-emoji`, `main` branch, `LICENSE`.
Header: `Copyright 2013 Google LLC` / `This Font Software is licensed under
the SIL Open Font License, Version 1.1.`

### The full SIL OFL 1.1 body (covers all three files above)

Source: `https://raw.githubusercontent.com/adobe-fonts/source-sans/release/
LICENSE.md` (the body text is identical across all three upstream projects —
it is the standard OFL 1.1, reproduced once here rather than three times):

```
-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

### `kern-subset.ttf` — DejaVu licence (Bitstream Vera + Arev), correctly named

Source: `dejavu-fonts/dejavu-fonts`, `master` branch, `LICENSE`
(`https://raw.githubusercontent.com/dejavu-fonts/dejavu-fonts/master/
LICENSE`), reproduced in full:

```
Fonts are (c) Bitstream (see below). DejaVu changes are in public domain.
Glyphs imported from Arev fonts are (c) Tavmjong Bah (see below)


Bitstream Vera Fonts Copyright
------------------------------

Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is
a trademark of Bitstream, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of the fonts accompanying this license ("Fonts") and associated
documentation files (the "Font Software"), to reproduce and distribute the
Font Software, including without limitation the rights to use, copy, merge,
publish, distribute, and/or sell copies of the Font Software, and to permit
persons to whom the Font Software is furnished to do so, subject to the
following conditions:

The above copyright and trademark notices and this permission notice shall
be included in all copies of one or more of the Font Software typefaces.

The Font Software may be modified, altered, or added to, and in particular
the designs of glyphs or characters in the Fonts may be modified and
additional glyphs or characters may be added to the Fonts, only if the fonts
are renamed to names not containing either the words "Bitstream" or the word
"Vera".

This License becomes null and void to the extent applicable to Fonts or Font
Software that has been modified and is distributed under the "Bitstream
Vera" names.

The Font Software may be sold as part of a larger software package but no
copy of one or more of the Font Software typefaces may be sold by itself.

THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF COPYRIGHT, PATENT,
TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL BITSTREAM OR THE GNOME
FOUNDATION BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, INCLUDING
ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM OTHER DEALINGS IN
THE FONT SOFTWARE.

Except as contained in this notice, the names of GNOME, the GNOME Foundation,
and Bitstream Inc., shall not be used in advertising or otherwise to promote
the sale, use or other dealings in this Font Software without prior written
authorization from the GNOME Foundation or Bitstream Inc., respectively. For
further information, contact: fonts at gnome dot org.
```
(the file continues with the AMSFonts/Arev notices, omitted here as this
project's `kern-subset.ttf` is a DejaVu Sans subset and does not carry those
glyphs — see the fonts README's own "Derived from" column).

## A finding: `colr-emoji-subset.ttf`'s licence, AS NAMED IN THE EXISTING README, IS WRONG

`tests/fixtures/fonts/README.md`'s table currently reads (for
`colr-emoji-subset.ttf`): **"Font build MIT; the emoji artwork is Twemoji,
CC-BY 4.0."** The **CC-BY 4.0 half is correct**; the **MIT half is not**.

Checked against the actual upstream source
(`mozilla/twemoji-colr`, the repository the README itself cites as the
"Derived from" column): its `package.json` states
`"license": "Apache-2.0"`, and its `LICENSE.md`
(`https://raw.githubusercontent.com/mozilla/twemoji-colr/master/LICENSE.md`)
says, in full:

```
## License for the Code

Copyright 2016-2018, Mozilla Foundation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.



## License for the Visual Design

The Emoji art in the twe-svg.zip archive comes from [Twemoji]
(https://twitter.github.io/twemoji),
and is used and redistributed under the CC-BY-4.0 [license terms]
(https://github.com/twitter/twemoji#license)
offered by the Twemoji project.
```

**This is Apache License 2.0, not MIT.** Apache-2.0 and MIT are both
permissive and both already satisfied by this pass's remediation in spirit
(the actual text now travels with the fixture, in this file), but they are
not the same licence — Apache-2.0 carries an express patent grant and
different notice-file requirements MIT does not have — and a project's own
`THIRD_PARTY.md`/audit that records this file as "MIT" would be recording a
different licence than the one the code was actually released under.
**This PROVENANCE.md is not the place to silently correct
`README.md`** (this task's write authority is scoped to new `PROVENANCE.md`
files; `README.md` is pre-existing and outside that scope) — it is
recorded here as a finding for the file's owner to fix.

## Twemoji artwork licence — CC BY 4.0, correctly named

Full text (`https://raw.githubusercontent.com/twitter/twemoji/master/
LICENSE-GRAPHICS`) is the standard Creative Commons Attribution 4.0
International Public License, ~380 lines; not reproduced in full here (it is
long and entirely boilerplate legal text with no per-project fill-ins to
verify), but its identity was confirmed by direct fetch and its human-readable
summary is: attribution required, commercial use and modification permitted,
no additional restrictions — consistent with the README's characterisation.
Canonical text: `https://creativecommons.org/licenses/by/4.0/legalcode`.

## Consuming gates

None of `test-font`, `test-font-otl`, `test-font-color`, `test-font-fuzz`,
`test-font-control` were touched by this pass: these files are class B
(permissively licensed, redistribution already permitted), not class A, so
they were out of the removal/skip mandate. `test-font-fuzz` mutates copies of
these files in memory at test time and does not depend on the directory's
existence beyond the initial read, which is unconditional (the fonts are a
required, not optional, part of that test's own gate).

## History

First added: commit `ff1ca9bcd` ("text: read CFF outlines, OpenType Layout,
and colour tables"), 2026-08-07.
