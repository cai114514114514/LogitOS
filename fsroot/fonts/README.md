# Fonts

`ui.ttf` and `mono.ttf` are **generated** by `tools/mkfont.py` (subsets of macOS
Heiti SC + Menlo, GB2312 + Latin). They are Apple-proprietary and therefore
**.gitignored** — the build regenerates them. For redistribution, point
`tools/mkfont.py --ui` at an SIL-OFL font (e.g. Noto Sans SC, TTF).
