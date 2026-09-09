#!/usr/bin/env python3
"""aggregate.py -- join every instrument's output into the demand-vs-support
tables. Inputs (all produced by the scripts beside this one, over the corpus
fetched by fetch_corpus.py):
  census/         tinycss2 demand census (census.py)
  oracle/         per-rule LibCSS verdicts (rule_oracle.py + dropdump.c)
  valoracle/      per-(property,value) LibCSS verdicts (value_oracle.py)
  drops/          LibCSS alone, per source (dropdump.c)
  drops2/         LibCSS behind the browser's var() pre-pass (dropdump2.c)
  chrome/         Chrome's rule counts (chrome_count.py) -- the apparatus check
Writes results.json and results.txt into the output dir."""
import sys, os, re, json, collections
W, out = sys.argv[1], sys.argv[2]
census = {r["site"]: r for r in json.load(open(os.path.join(W, "census/all.json")))}
sites = [s for s in sorted(census) if census[s]["rules"] > 0]
NS = len(sites)

LIBCSS_PSEUDO = set("""first-child link visited hover active focus lang left right first root
nth-child nth-last-child nth-of-type nth-last-of-type last-child first-of-type last-of-type
only-child only-of-type empty target enabled disabled checked not is where focus-within
focus-visible any-link defined placeholder-shown modal user-invalid has host dir first-line
first-letter before after marker placeholder backdrop part slotted selection""".split())
# measured with chrome_constructs.py: constructs a real browser ALSO refuses
CHROME_REFUSES = set("matches -moz-any playing target-within local-link blank".split())
def chrome_invalid_pseudo(name):
    return name.startswith("-moz-") or name.startswith("-ms-") or name in CHROME_REFUSES

PSEUDO_TOK = re.compile(r"(::?)(-?[a-zA-Z][a-zA-Z0-9-]*)(\()?")
def pseudos_with_context(sel):
    """yield (name, is_element, functional, enclosing_function_name) for every
    pseudo in a selector list, tracking the innermost functional pseudo."""
    out = []; stack = []; i = 0; n = len(sel); in_str = None; depth_attr = 0
    while i < n:
        c = sel[i]
        if in_str:
            if c == "\\": i += 2; continue
            if c == in_str: in_str = None
            i += 1; continue
        if c in "\"'": in_str = c; i += 1; continue
        if c == "\\": i += 2; continue
        if c == "[": depth_attr += 1; i += 1; continue
        if c == "]": depth_attr = max(0, depth_attr - 1); i += 1; continue
        if c == "(":
            stack.append(None); i += 1; continue
        if c == ")":
            if stack: stack.pop()
            i += 1; continue
        if c == ":" and depth_attr == 0:
            m = PSEUDO_TOK.match(sel, i)
            if m:
                name = m.group(2).lower(); fn = bool(m.group(3))
                encl = next((s for s in reversed(stack) if s), None)
                out.append((name, m.group(1) == "::", fn, encl))
                i = m.end()
                if fn: stack.append(name)
                continue
        i += 1
    return out

def classify_lost(rec):
    """Return (class, chrome_valid) for a rule LibCSS refused."""
    sel = rec["sel"]
    if rec.get("verdict") == "nested" or "&" in sel:
        return ("CSS nesting (& / nested rule inside a style rule)", True)
    ps = pseudos_with_context(sel)
    # Chrome-invalid anywhere outside :is()/:where() -> Chrome drops the whole list too
    for name, el, fn, encl in ps:
        if chrome_invalid_pseudo(name) and encl not in ("is", "where"):
            return ("vendor/obsolete pseudo (-moz-/-ms-/:matches) -- Chrome refuses too", False)
    if re.search(r"\[[^\]]*=[^\]]*\s+s\s*\]", sel): return ("attribute selector `s` flag -- Chrome refuses too", False)
    if re.search(r"(^|[\s,>+~(])[a-zA-Z*]*\|[a-zA-Z*]", sel) and not re.search(r"\*\|", sel):
        return ("namespace prefix without @namespace -- Chrome refuses too", False)
    for name, el, fn, encl in ps:
        if chrome_invalid_pseudo(name) and encl in ("is", "where"):
            return ("vendor pseudo inside :is()/:where() (forgiving list in Chrome, refused whole here)", True)
    for name, el, fn, encl in ps:
        if name in ("nth-child", "nth-last-child") and fn and re.search(r":nth-(last-)?child\([^)]*\bof\b", sel):
            return (":nth-child(An+B of S)", True)
    if re.search(r"\[[^\]]*=[^\]]*\s+i\s*\]", sel): return ("attribute selector `i` flag", True)
    unknown_top = [n for n, el, fn, encl in ps if n not in LIBCSS_PSEUDO and not (el and n.startswith("-webkit-")) and encl is None]
    unknown_in_is = [n for n, el, fn, encl in ps if n not in LIBCSS_PSEUDO and not (el and n.startswith("-webkit-")) and encl in ("is", "where")]
    unknown_in_not = [n for n, el, fn, encl in ps if n not in LIBCSS_PSEUDO and not (el and n.startswith("-webkit-")) and encl == "not"]
    if unknown_top: return ("Selectors-4/HTML pseudo missing from pseudo_lut: :" + unknown_top[0], True)
    if unknown_in_is: return ("unknown pseudo inside :is()/:where() (forgiving in Chrome): :" + unknown_in_is[0], True)
    if unknown_in_not: return (":not(<unknown pseudo>): :" + unknown_in_not[0], True)
    # combinator inside :is()/:where()
    for m in re.finditer(r":(is|where)\(", sel):
        depth = 1; j = m.end(); arg_start = j
        while j < len(sel) and depth:
            if sel[j] == "(": depth += 1
            elif sel[j] == ")": depth -= 1
            j += 1
        arg = sel[arg_start:j-1]
        arg0 = re.sub(r"\([^()]*\)", "", arg)
        if re.search(r"[>+~]", arg0) or re.search(r"[^,\s]\s+[^,\s]", arg0):
            return (":is()/:where() with a complex selector (combinator) inside", True)
    for m in re.finditer(r":not\(", sel):
        depth = 1; j = m.end(); arg_start = j
        while j < len(sel) and depth:
            if sel[j] == "(": depth += 1
            elif sel[j] == ")": depth -= 1
            j += 1
        arg = sel[arg_start:j-1]
        arg0 = re.sub(r"\([^()]*\)", "", arg)
        if re.search(r"[>+~]", arg0) or re.search(r"[^,\s]\s+[^,\s]", arg0):
            return (":not() with a complex selector (combinator) inside", True)
    if re.search(r":is\(\s*\)|:where\(\s*\)", sel): return ("empty :is()/:where()", True)
    if re.search(r"\*\|", sel): return ("`*|` universal namespace", True)
    return ("UNATTRIBUTED", True)

def has_only(sel):
    """every selector in the list carries :has() -> rule can never match here"""
    parts = re.split(r",(?![^(]*\))", sel)
    return all(":has(" in p for p in parts)
def pseudo_elem_only(sel):
    parts = re.split(r",(?![^(]*\))", sel)
    return all(re.search(r"::?(before|after)\b", p) for p in parts)

# ---------------------------------------------------------------- rules
per_site = {}
cls_rules = collections.defaultdict(lambda: [0, 0, set()])   # class -> [rules, decls, sites]
unattributed = []
for s in sites:
    recs = json.load(open(os.path.join(W, "oracle/verdicts-%s.json" % s)))
    c = census[s]
    tot_r = len(recs); tot_d = sum(r["nd"] for r in recs)
    lost_r = lost_d = 0; lost_r_cv = lost_d_cv = 0
    inert_has_r = inert_has_d = 0; pe_r = pe_d = 0
    coll_r = 0
    for r in recs:
        if r["verdict"] in ("refused", "nested", "ambiguous"):
            cl, cv = classify_lost(r)
            cls_rules[cl][0] += 1; cls_rules[cl][1] += r["nd"]; cls_rules[cl][2].add(s)
            lost_r += 1; lost_d += r["nd"]
            if cv: lost_r_cv += 1; lost_d_cv += r["nd"]
            if cl == "UNATTRIBUTED" and len(unattributed) < 60: unattributed.append((s, r["sel"][:140]))
        elif r["verdict"] == "ok":
            if ":has(" in r["sel"] and has_only(r["sel"]):
                inert_has_r += 1; inert_has_d += r["nd"]
            if pseudo_elem_only(r["sel"]):
                pe_r += 1; pe_d += r["nd"]
    at_lost_r = sum(v for k, v in c["atblock_lost_rules"].items() if k not in ("keyframes", "-webkit-keyframes", "-moz-keyframes"))
    at_lost_d = sum(v for k, v in c["atblock_lost_decls"].items() if k not in ("keyframes", "-webkit-keyframes", "-moz-keyframes"))
    per_site[s] = {"rules": tot_r, "decls": tot_d,
                   "parser_lost_rules": lost_r, "parser_lost_decls": lost_d,
                   "parser_lost_rules_chrome_valid": lost_r_cv, "parser_lost_decls_chrome_valid": lost_d_cv,
                   "has_inert_rules": inert_has_r, "has_inert_decls": inert_has_d,
                   "pseudo_element_rules": pe_r, "pseudo_element_decls": pe_d,
                   "atblock_lost_rules": at_lost_r, "atblock_lost_decls": at_lost_d,
                   "atblocks_lost": {k: v for k, v in c["atrule"].items() if k in ("property", "starting-style", "counter-style", "font-feature-values", "-moz-document", "document", "view-transition", "position-try", "tailwind", "custom-media")},
                   "import_rules": c["atrule"].get("import", 0), "fontface": c["fontface"], "kf_rules": c["kf_rules"]}

# ---------------------------------------------------------------- declarations (real pipeline)
def read_drops(path):
    per = collections.defaultdict(lambda: [0, 0, 0, 0]); at = collections.Counter()
    for line in open(path, errors="replace"):
        p = line.rstrip("\n").split("\t")
        if p[0] == "DECL":
            if p[1].startswith("@"): at[p[1].lower()] += 1
            elif p[1].startswith("__logit") or p[1] == "z-index" and False: pass
            else: per[p[1].lower()][int(p[2])] += 1
    return per, at
d1 = {s: read_drops(os.path.join(W, "drops/%s.tsv" % s)) for s in sites}
d2 = {s: read_drops(os.path.join(W, "drops2/%s.tsv" % s)) for s in sites}
prop_tot = collections.defaultdict(lambda: [0, 0, 0, 0, set()])
for s in sites:
    for k, v in d2[s][0].items():
        for i in range(4): prop_tot[k][i] += v[i]
        prop_tot[k][4].add(s)
tot1 = [sum(v[i] for s in sites for v in d1[s][0].values()) for i in range(4)]
tot2 = [sum(v[i] for s in sites for v in d2[s][0].values()) for i in range(4)]

# ---------------------------------------------------------------- value classes
cache = json.load(open(os.path.join(W, "valoracle/value_cache.json")))
CSS_EXTRA_RESCUE = {"transform", "transform-origin", "box-shadow", "animation", "animation-name", "transition", "transition-property",
    "gap", "row-gap", "grid-gap", "grid-column-gap", "grid-row-gap", "grid-template", "grid-template-columns", "grid-template-rows",
    "grid-template-areas", "grid-auto-columns", "grid-auto-rows", "grid-auto-flow", "grid-column", "grid-row", "grid-area",
    "justify-items", "justify-self", "inset", "inset-inline", "inset-block", "inset-inline-start", "inset-inline-end",
    "inset-block-start", "inset-block-end", "margin-inline", "margin-inline-start", "margin-inline-end", "margin-block",
    "margin-block-start", "margin-block-end", "padding-inline", "padding-inline-start", "padding-inline-end", "padding-block",
    "padding-block-start", "padding-block-end", "mask-image", "-webkit-mask-image", "clip-path"}
IGNORED_BY_ENGINE = {"background-image", "background-position", "background-repeat", "background-attachment", "vertical-align",
    "content", "cursor", "outline", "outline-width", "border-collapse", "border-spacing", "table-layout", "counter-increment",
    "counter-reset", "quotes", "unicode-bidi", "column-count", "column-width", "column-rule", "orphans", "widows", "page-break-after",
    "font-variant", "empty-cells", "caption-side", "outline-color", "outline-style", "page-break-before", "page-break-inside",
    "list-style-image", "list-style-position", "clip", "columns", "column-gap", "column-fill", "column-span", "column-rule-color",
    "column-rule-style", "column-rule-width", "break-after", "break-before", "break-inside", "speak", "azimuth", "elevation",
    "cue", "cue-after", "cue-before", "pause", "pause-after", "pause-before", "pitch", "pitch-range", "play-during", "richness",
    "speak-header", "speak-numeral", "speak-punctuation", "speech-rate", "stress", "voice-family", "volume", "fill-opacity", "stroke-opacity"}
def value_class(prop, val, verdict):
    v = val.lower(); p = prop
    if verdict == "UNKNOWN-PROP":
        if p in CSS_EXTRA_RESCUE: return "unknown to LibCSS, RESCUED by css_extra raw scan (" + ("grid/gap/inset/logical" if p.startswith(("grid", "gap", "row-gap", "inset", "margin-", "padding-", "justify-")) else p) + ")"
        if re.match(r"-(webkit|moz|ms|o)-", p): return "vendor-prefixed property (-webkit-/-moz-/-ms-)"
        if p.startswith("transition-") or p.startswith("animation-"): return "transition-*/animation-* LONGHANDS (not captured; only the shorthand is)"
        if p in ("fill", "stroke", "stroke-width", "stroke-linecap", "stroke-linejoin", "stroke-dasharray", "fill-rule"): return "SVG presentation properties (fill/stroke)"
        if p in ("text-overflow", "word-break", "overflow-wrap", "word-wrap", "hyphens", "text-wrap", "line-clamp", "text-align-last", "tab-size", "text-size-adjust"): return "text-overflow/word-break/overflow-wrap/hyphens/text-wrap/line-clamp"
        if p.startswith("text-decoration-") or p in ("text-underline-offset", "text-underline-position"): return "text-decoration-* longhands / text-underline-offset"
        if p in ("pointer-events", "user-select", "touch-action", "appearance", "resize", "caret-color", "accent-color", "color-scheme", "scroll-behavior", "overscroll-behavior", "will-change", "cursor"): return "interaction props (pointer-events/user-select/appearance/will-change/...)"
        if p.startswith("scroll-") or p.startswith("overscroll-"): return "scroll-snap/scroll-margin/scroll-padding"
        if p in ("filter", "backdrop-filter", "mix-blend-mode", "background-blend-mode", "isolation", "mask", "mask-size", "mask-position", "mask-repeat", "mask-composite", "clip-path"): return "filter/backdrop-filter/mask/blend/clip-path"
        if p in ("aspect-ratio",): return "aspect-ratio"
        if p in ("inline-size", "block-size", "min-inline-size", "max-inline-size", "min-block-size", "max-block-size") or p.startswith("border-inline") or p.startswith("border-block") or p.startswith("border-start") or p.startswith("border-end"): return "logical sizing/border (inline-size, border-inline-*, border-start-*-radius)"
        if p in ("object-fit", "object-position", "background-size", "background-clip", "background-origin", "border-image", "border-image-source", "border-image-slice", "border-image-width", "border-image-outset", "border-image-repeat", "image-rendering"): return "background-size/clip/origin, object-fit, border-image"
        if p in ("text-shadow", "outline-offset", "text-rendering", "font-feature-settings", "font-variation-settings", "font-stretch", "font-optical-sizing", "font-kerning", "font-display", "font-synthesis", "font-variant-numeric", "font-variant-ligatures", "font-smoothing"): return "text-shadow/outline-offset/font-feature-settings/font-stretch"
        if p in ("place-items", "place-content", "place-self", "contain", "content-visibility", "container", "container-type", "container-name", "translate", "rotate", "scale", "perspective", "perspective-origin", "backface-visibility", "transform-style", "transform-box", "zoom", "all", "view-transition-name", "anchor-name", "position-anchor", "field-sizing", "forced-color-adjust", "print-color-adjust", "text-emphasis", "ruby-position", "shape-outside", "offset-path", "paint-order", "math-style", "scrollbar-width", "scrollbar-gutter", "scrollbar-color", "-webkit-line-clamp"): return "other CSS3+ properties (place-*, contain, container-type, translate/rotate/scale, all, ...)"
        return "other unknown property"
    # BAD-VALUE / SILENT / TRAILING
    if "var(" in v: return "var() (LibCSS alone; the browser's css_vars.c pre-pass resolves DEFINED ones -- see rescue row)"
    if re.search(r"\b(clamp|min|max)\(", v): return "clamp()/min()/max()"
    if re.search(r"\b(oklch|oklab|lab|lch|color-mix|light-dark|color)\(", v): return "CSS Color 4/5 (oklch/oklab/lab/lch/color()/color-mix/light-dark)"
    if "env(" in v: return "env()"
    if p == "display":
        if verdict == "SILENT": return "display: two-value syntax (silent)"
        return "display keyword outside LibCSS's set: " + v
    if p in ("width", "height", "min-width", "max-width", "min-height", "max-height", "flex-basis") and re.search(r"\b(fit-content|min-content|max-content|-webkit-fill-available|-moz-available|stretch|-webkit-min-content|-webkit-max-content|-moz-fit-content)\b", v): return "intrinsic sizing keywords (fit-content/min-content/max-content/-webkit-fill-available)"
    if p in ("justify-content", "align-items", "align-self", "align-content", "justify-self", "justify-items", "text-align", "vertical-align") and re.search(r"\b(start|end|self-start|self-end|left|right|stretch|normal|safe|unsafe|first|last|baseline)\b", v): return "box-alignment keywords (start/end/stretch/safe/... in justify-*/align-*/text-align)"
    if p == "flex" and verdict == "SILENT": return "flex: <grow> 0 <basis> shorthand (SILENT drop, flex.c error overwritten)"
    if p == "background" and verdict == "SILENT": return "background shorthand with `/size`, gradient or var() (SILENT drop)"
    if p in ("background-image", "background", "border-image", "mask-image", "list-style-image") and re.search(r"(radial|conic|repeating-|-webkit-|-moz-|-o-)[a-z-]*gradient\(", v): return "radial/conic/repeating/-webkit- gradients"
    if p in ("background-image", "background") and "linear-gradient(" in v: return "linear-gradient (LibCSS refuses; css_extra rescues into xraw)"
    if "image-set(" in v: return "image-set()"
    if "calc(" in v: return "calc() inside a shorthand or a refusing handler (padding/margin/font-size/...)"
    if re.search(r"\bcq[wbhi]|cqmin|cqmax", v): return "container units (cqw/cqi/...)"
    if p == "cursor": return "cursor keywords beyond CSS 2.1"
    if p == "font-family": return "font-family value refused (escapes/quotes)"
    if re.search(r"\b(revert-layer)\b", v): return "revert-layer keyword"
    if p == "overflow" or p == "overflow-x" or p == "overflow-y": return "overflow: clip / overlay"
    if p in ("unicode-bidi", "float", "clear", "position") : return "misc CSS3 keywords (unicode-bidi isolate, float inline-start, position -webkit-sticky)"
    if re.search(r"\\9|\\0", v): return "IE hacks (\\9, \\0) -- Chrome refuses too"
    if p in ("color", "background-color", "border-color", "border", "border-top-color", "border-bottom-color", "border-left-color", "border-right-color", "outline-color", "background") and re.search(r"\b(buttontext|canvas|canvastext|highlight|linktext|graytext|field|fieldtext|-webkit-link|-webkit-focus-ring-color|activeborder|buttonface|windowtext)\b", v): return "system colors"
    if verdict == "SILENT": return "silent drop, other (" + p + ")"
    return "other refused value (" + p + ")"

vclass = collections.defaultdict(lambda: [0, set()])
examples = collections.defaultdict(set)
site_vals = {}
for s in sites:
    rows = [json.loads(l) for l in open(os.path.join(W, "census/values-%s.jsonl" % s))]
    for p, v, n in rows:
        r = cache.get("%s\t%s" % (p, v), "UNPROBED")
        if r in ("ACCEPTED", "UNPROBED", "AMBIGUOUS"): continue
        cl = value_class(p, v, r)
        vclass[cl][0] += n; vclass[cl][1].add(s)
        if len(examples[cl]) < 4: examples[cl].add("%s:%s" % (p, v[:60]))

# ---------------------------------------------------------------- demand census (features)
def sites_using(getter):
    n = 0; tot = 0
    for s in sites:
        v = getter(census[s]) or 0
        if v: n += 1; tot += v
    return n, tot
def prop_demand(names):
    return sites_using(lambda c: sum(c["prop"].get(x, 0) for x in names))
def sel_demand(feats, key="selfeat_rules"):
    return sites_using(lambda c: sum(c[key].get(x, 0) for x in feats))
def func_demand(names):
    return sites_using(lambda c: sum(c["func_decls"].get(x, 0) for x in names))
def unit_demand(names):
    return sites_using(lambda c: sum(c["unit_decls"].get(x, 0) for x in names))
def at_demand(names):
    return sites_using(lambda c: sum(c["atrule"].get(x, 0) for x in names))
def media_demand(names):
    return sites_using(lambda c: sum(c["media_feat"].get(x, 0) for x in names))
def sel_pseudo_rules(names):
    keys = []
    for n in names: keys += ["pseudo:" + n, "pseudo:" + n + "()", "pseudo-element::" + n, "pseudo-element::" + n + "()"]
    return sel_demand(keys)
def sel_pseudo_decls(names):
    keys = []
    for n in names: keys += ["pseudo:" + n, "pseudo:" + n + "()", "pseudo-element::" + n, "pseudo-element::" + n + "()"]
    return sel_demand(keys, "selfeat_decls")

kf_props = collections.Counter(); kf_sites_other = 0
anim_lh_only = trans_lh_only = 0
for s in sites:
    c = census[s]; kf_props.update(c["kf_props"])
    other = sum(v for k, v in c["kf_props"].items() if k not in ("opacity", "transform"))
    if other: kf_sites_other += 1
    anim_lh_only += c["anim_longhand_only_rules"]; trans_lh_only += c["trans_longhand_only_rules"]
trans_props = collections.Counter()
for s in sites: trans_props.update(census[s]["transition_props"])
display_vals = collections.Counter()
for s in sites: display_vals.update(census[s]["display_values"])

res = {"sites": sites, "n_sites": NS, "per_site": per_site,
       "rule_loss_classes": {k: {"rules": v[0], "decls": v[1], "sites": len(v[2])} for k, v in sorted(cls_rules.items(), key=lambda kv: -kv[1][1])},
       "unattributed_examples": unattributed,
       "decl_totals_libcss_alone": {"unknown": tot1[0], "bad": tot1[1], "trailing": tot1[2], "accepted": tot1[3]},
       "decl_totals_after_var_prepass": {"unknown": tot2[0], "bad": tot2[1], "trailing": tot2[2], "accepted": tot2[3]},
       "value_classes": {k: {"decls": v[0], "sites": len(v[1]), "examples": sorted(examples[k])} for k, v in sorted(vclass.items(), key=lambda kv: -kv[1][0])},
       "prop_drops_after_var_prepass": {k: {"unknown": v[0], "bad": v[1], "accepted": v[3], "sites": len(v[4])} for k, v in sorted(prop_tot.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:120]},
       "keyframe_props": dict(kf_props.most_common(30)), "kf_sites_animating_other_than_opacity_transform": kf_sites_other,
       "animation_longhand_only_rules": anim_lh_only, "transition_longhand_only_rules": trans_lh_only,
       "transition_props": dict(trans_props.most_common(20)), "display_values": dict(display_vals.most_common(25)),
       "demand": {}}

D = res["demand"]
def add(name, kind, verdict, anchor, demand, severity, note=""):
    n, tot = demand
    D[name] = {"kind": kind, "sites": n, "count": tot, "verdict": verdict, "severity": severity, "score": n * severity, "anchor": anchor, "note": note}
# severity: 4 = whole block/sheet lost, 3 = rule lost, 2 = declaration lost or parsed-but-ignored where layout/paint changes, 1 = partial/approximate, 0 = supported or not a defect
add("::before/::after generated content", "selector", "PARSED-BUT-IGNORED (rule accepted; pseudo-element never generated)", "css_engine.c:2507 reads only styles[CSS_PSEUDO_ELEMENT_NONE]; css_extra.c:2261 lists `content` as ignored", sel_pseudo_rules(["before", "after"]), 2)
add(":is()/:where() with a combinator inside", "selector", "DROPS-THE-RULE (whole selector list)", "language.c:2301-2323 parseIsWhereList -> CSS_INVALID", sel_demand(["pseudo:is() with combinator inside", "pseudo:where() with combinator inside"]), 3, "Chrome accepts")
add(":is()/:where() simple list", "selector", "SUPPORTED", "language.c:2220 parseIsWhereList; select.c:2607", sel_demand(["pseudo:is()", "pseudo:where()"]), 0)
add(":has()", "selector", "PARSED, NEVER MATCHES (rule inert)", "language.c:2014 skipParenArgument; select.c:2807 `*match = false`", sel_demand(["pseudo:has()"]), 3, "Chrome accepts")
add(":not() with a list", "selector", "SUPPORTED", "language.c:1939-1978", sel_demand(["pseudo:not() with selector list"]), 0)
add(":nth-child(An+B of S)", "selector", "DROPS-THE-RULE", "language.c:1483 parseNth (an+b only); :1931", sel_demand(["pseudo:nth-child(An+B of S)"]), 3, "Chrome accepts")
add("attribute selector [x=y i]", "selector", "DROPS-THE-RULE", "language.c:1381 parseAttrib expects ']' after the value", sel_demand(["attr[... i/s flag]"]), 3, "Chrome accepts `i`")
add("attribute operators ^= $= *= ~= |=", "selector", "SUPPORTED", "language.c:1381 parseAttrib", sel_demand(["attr[^=]", "attr[$=]", "attr[*=]", "attr[~=]", "attr[|=]"]), 0)
add("CSS nesting (& / nested rules)", "selector", "DROPS-THE-RULE (nested rule AND the declarations after it in the block)", "parse.c:2313 parseMalformedDeclaration; language.c:2502 parseSimpleSelector refuses `&`", sel_demand(["nesting &", "nesting (nested style rule)", "nesting (nested at-rule)"]), 3, "Chrome accepts")
add(":focus-visible / :focus-within", "selector", "PARSED, NEVER MATCHES (state; deliberate)", "language.c:1778-1779; select.c:2807", sel_pseudo_rules(["focus-visible", "focus-within"]), 0, "same as a static Chrome render")
add(":hover / :focus / :active", "selector", "PARSED, FALSE AT REST (deliberate)", "css_engine.c:324-340 h_false", sel_pseudo_rules(["hover", "focus", "active"]), 0, "same as a static render; no re-style on state change")
add("form-state pseudos :required/:valid/:invalid/:indeterminate/:default/:autofill/:read-only/:in-range/:user-valid", "selector", "DROPS-THE-RULE", "language.c:1874-1877 unknown pseudo -> CSS_INVALID", sel_pseudo_rules(["required", "optional", "valid", "invalid", "indeterminate", "default", "autofill", "-webkit-autofill", "read-only", "read-write", "in-range", "out-of-range", "user-valid"]), 3, "Chrome accepts")
add(":popover-open/:open/:state()/:scope/:fullscreen/:host-context()/:-webkit-any()", "selector", "DROPS-THE-RULE", "language.c:1874-1877", sel_pseudo_rules(["popover-open", "open", "closed", "state", "scope", "fullscreen", "-webkit-full-screen", "host-context", "-webkit-any", "current", "future", "picture-in-picture", "active-view-transition"]), 3, "Chrome accepts")
add("::file-selector-button/::details-content/::view-transition*/::cue", "selector", "DROPS-THE-RULE", "language.c:1874-1877", sel_pseudo_rules(["file-selector-button", "details-content", "view-transition", "view-transition-old", "view-transition-new", "view-transition-group", "view-transition-image-pair", "cue"]), 3, "Chrome accepts")
add("::-moz-* / :-moz-* / ::-ms-* / :-ms-* pseudos", "selector", "DROPS-THE-RULE -- Chrome refuses too (not a defect)", "language.c:1874-1877", sel_demand([k for s in sites for k in census[s]["selfeat_rules"] if re.match(r"pseudo(-element)?::?-(moz|ms)-", k)]), 0)
add("::-webkit-* compatibility pseudo-elements", "selector", "PARSED, NEVER MATCHES (correct per Selectors 4)", "language.c:1866-1879", sel_demand([k for s in sites for k in census[s]["selfeat_rules"] if re.match(r"pseudo-element::-webkit-", k)]), 0)
add("::placeholder/::marker/::selection/::backdrop/::part()/::slotted()", "selector", "PARSED, NEVER MATCHES", "select.c:2810-2823 only first-line/first-letter/before/after set a pseudo", sel_pseudo_rules(["placeholder", "marker", "selection", "backdrop", "part", "slotted"]), 1)
add(":root", "selector", "SUPPORTED", "language.c:1753 ROOT; select.c node_is_root", sel_pseudo_rules(["root"]), 0)
add(":checked/:disabled/:enabled/:target", "selector", "SUPPORTED", "css_engine.c:324-420", sel_pseudo_rules(["checked", "disabled", "enabled", "target"]), 0)
add(":first-child/:last-child/:nth-*/:only-child/:empty", "selector", "SUPPORTED", "language.c:1743-1763", sel_pseudo_rules(["first-child", "last-child", "nth-child", "nth-last-child", "nth-of-type", "nth-last-of-type", "first-of-type", "last-of-type", "only-child", "only-of-type", "empty"]), 0)
add("selector list (comma) with one refused selector", "selector", "DROPS-THE-RULE (whole list, both here and in Chrome for non-forgiving lists)", "language.c parseSelectorList", sel_demand(["selector list (comma)"]), 0, "demand = all comma lists; the loss is in the per-site table")
# at-rules
add("@media (any feature)", "at-rule", "ENTERED (evaluated by mq.c; non-width features UNMEASURED)", "language.c:923-956; mq.c:262 range syntax", at_demand(["media"]), 0)
add("@media range syntax (width >= N)", "at-rule", "ENTERED (measured accepted)", "mq.c:262-300 mq_parse_range", media_demand(["<range syntax>"]), 0)
add("@media (prefers-color-scheme)", "at-rule", "ENTERED; evaluation UNMEASURED", "mq.c", media_demand(["prefers-color-scheme"]), 1)
add("@media (prefers-reduced-motion)", "at-rule", "ENTERED; evaluation UNMEASURED", "mq.c", media_demand(["prefers-reduced-motion"]), 1)
add("@media (hover)/(pointer)/(any-hover)", "at-rule", "ENTERED; evaluation UNMEASURED", "mq.c", media_demand(["hover", "pointer", "any-hover", "any-pointer"]), 1)
add("@supports", "at-rule", "EVALUATED (supports_decl); @supports selector() answers NO", "language.c:387-560", at_demand(["supports"]), 0)
add("@layer", "at-rule", "ENTERED; layer ORDER ignored (source order wins)", "language.c:387-396", at_demand(["layer", "layer (statement)"]), 1, "css_drop_probe: WRONG WINNER")
add("@container", "at-rule", "ALWAYS TRUE (never evaluated)", "language.c:398-400", at_demand(["container"]), 1, "css_drop_probe: ALWAYS-TRUE")
add("@scope", "at-rule", "ENTERED; scope not confined", "language.c:429", at_demand(["scope"]), 1)
add("@keyframes / @-webkit-keyframes", "at-rule", "LibCSS discards the block; css_extra captures up to 64 rules x 16 stops per sheet; clock animates opacity+transform ONLY", "language.c:1033-1047; css_extra.c:1390-1440; css.h:673 CSS_KF_MAXRULE; js_anim.c:2013-2019", at_demand(["keyframes", "-webkit-keyframes"]), 2)
add("@font-face", "at-rule", "PARSED-BUT-IGNORED (no web-font loader; only /fonts/{ui,mono,text}.ttf exist)", "language.c:957-975 CSS_RULE_FONT_FACE; grep woff c/apps/browser -> only tabs.c", at_demand(["font-face"]), 2, "icon fonts render as nothing")
add("@import", "at-rule", "DROPS-THE-SHEET (rule parsed; browser never fetches it)", "language.c:797-880; css_engine.c sets no `import` callback (grep p.import = 0 hits)", at_demand(["import"]), 4)
add("@property", "at-rule", "DROPS-THE-RULE (registration lost; initial-value never reaches var())", "language.c:1033-1047 else-branch", at_demand(["property"]), 2, "css_drop_probe: RULE LOST")
add("@starting-style", "at-rule", "DROPS-THE-BLOCK", "language.c:1033-1047", at_demand(["starting-style"]), 2)
add("@counter-style/@font-feature-values/@view-transition/@position-try", "at-rule", "DROPS-THE-BLOCK", "language.c:1033-1047", at_demand(["counter-style", "font-feature-values", "view-transition", "position-try"]), 1)
add("@-moz-document/@document", "at-rule", "DROPS-THE-BLOCK -- Chrome refuses too", "language.c:1033-1047", at_demand(["-moz-document", "document"]), 0)
add("@charset/@namespace/@page", "at-rule", "SUPPORTED", "language.c:754-1000", at_demand(["charset", "namespace", "page"]), 0)
# value functions
add("var()", "value", "RESOLVED by css_vars.c pre-pass (one document-wide table; LibCSS itself refuses every var())", "css_vars.c:1-60; language.c:2805 BAD_VALUE", func_demand(["var"]), 1, "measured rescue: bad-value drops %d -> %d after the pre-pass" % (tot1[1], tot2[1]))
add("calc()", "value", "SUPPORTED in longhands; REFUSED inside shorthands (padding/margin) and font-size:clamp", "select/calc.c; padding.c shorthand", func_demand(["calc"]), 1)
add("clamp()/min()/max()", "value", "DROPS-THE-DECLARATION", "propstrings.c:521 (only `calc` is a known function)", func_demand(["clamp", "min", "max"]), 2)
add("env()", "value", "DROPS-THE-DECLARATION", "propstrings.c (no `env`)", func_demand(["env"]), 2)
add("attr()", "value", "PARSED (content only) -- content never rendered", "propstrings.c:440; css_extra.c:2261", func_demand(["attr"]), 1)
add("rgb()/rgba()/hsl()/hsla()/hwb() incl. space+slash syntax, #rrggbbaa", "value", "SUPPORTED", "propstrings.c:472-476; synth probe", func_demand(["rgb", "rgba", "hsl", "hsla", "hwb"]), 0)
add("oklch()/oklab()/lab()/lch()/color()/color-mix()/light-dark()", "value", "DROPS-THE-DECLARATION", "propstrings.c:472-476 (no such function names)", func_demand(["oklch", "oklab", "lab", "lch", "color", "color-mix", "light-dark"]), 2)
add("linear-gradient()", "value", "LibCSS refuses; css_extra rescues into cstyle.xraw[XR_BG_IMAGE] (linear only)", "css_extra.c:652-670, 780-784; css.h:97", func_demand(["linear-gradient"]), 1)
add("radial/conic/repeating/-webkit- gradients", "value", "DROPS-THE-DECLARATION", "css_extra.c:656-664 (whole-name match `linear-gradient` only)", func_demand(["radial-gradient", "conic-gradient", "repeating-linear-gradient", "repeating-radial-gradient", "-webkit-gradient", "-webkit-linear-gradient"]), 2)
add("url() images in background", "value", "PARSED-BUT-IGNORED (background-image is in the engine's ignored list; only gradients paint)", "css_extra.c:2261 `background-image` ignored", sites_using(lambda c: c["decl_of_prop_with"].get("background-image", {}).get("func:url", 0) + c["decl_of_prop_with"].get("background", {}).get("func:url", 0)), 2)
add("image-set()", "value", "DROPS-THE-DECLARATION", "value handler refuses", func_demand(["image-set", "-webkit-image-set"]), 1)
add("cubic-bezier()/steps()", "value", "captured raw for the animation clock", "css_extra.c:2372; js_anim.c", func_demand(["cubic-bezier", "steps"]), 0)
# units
add("rem / em / % / px", "unit", "SUPPORTED", "utils.c:1477 rem", unit_demand(["rem", "em", "%", "px"]), 0)
add("vw / vh / vmin / vmax", "unit", "SUPPORTED", "utils.c:1464-1551", unit_demand(["vw", "vh", "vmin", "vmax"]), 0)
add("dvh / svh / lvh / dvw", "unit", "SUPPORTED (parsed as vh/vw)", "utils.c:1493-1495", unit_demand(["dvh", "svh", "lvh", "dvw", "svw", "lvw"]), 0)
add("ch / ex / lh", "unit", "SUPPORTED", "utils.c:1531-1547", unit_demand(["ch", "ex", "lh"]), 0)
add("cqw / cqi / cqb", "unit", "DROPS-THE-DECLARATION", "utils.c (no cq units); synth `width:50cqw` BAD-VALUE", unit_demand(["cqw", "cqi", "cqb", "cqh", "cqmin", "cqmax"]), 2)
add("fr (grid tracks)", "unit", "SUPPORTED via css_extra raw grid text", "layout_grid.c grid_parse_template", unit_demand(["fr"]), 0)
# properties
add("transition (shorthand)", "property", "captured raw; the clock runs opacity and transform ONLY", "css_extra.c:2372; css.h:341-352; js_anim.c:2013-2019,2064", prop_demand(["transition"]), 2)
add("transition-* longhands", "property", "DROPS-THE-DECLARATION (longhands deliberately not captured)", "css.h:341-345; language.c:2768 UNKNOWN_PROP", prop_demand(["transition-property", "transition-duration", "transition-timing-function", "transition-delay"]), 2, "%d rules declare transition-* with no `transition` shorthand" % trans_lh_only)
add("animation (shorthand)", "property", "captured raw; keyframes on opacity/transform only; 64-rule cap per sheet", "css_extra.c:2372, :1425; js_anim.c:2194-2208", prop_demand(["animation"]), 2)
add("animation-* longhands", "property", "DROPS-THE-DECLARATION (only animation-name is looked at, for the anim flag)", "css_extra.c:2372; language.c:2768", prop_demand(["animation-name", "animation-duration", "animation-delay", "animation-fill-mode", "animation-iteration-count", "animation-timing-function", "animation-play-state", "animation-direction"]), 2, "%d rules declare animation-* with no `animation` shorthand" % anim_lh_only)
add("transform / transform-origin", "property", "SUPPORTED via css_extra raw capture + css_interp + painter", "css_extra.c:713-716; css.h:95-96; css_interp.c:385-397", prop_demand(["transform", "transform-origin"]), 0)
add("translate / rotate / scale (individual)", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["translate", "rotate", "scale"]), 2)
add("will-change", "property", "DROPS-THE-DECLARATION (harmless)", "language.c:2768", prop_demand(["will-change"]), 0)
add("backdrop-filter / -webkit-backdrop-filter", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["backdrop-filter", "-webkit-backdrop-filter"]), 2)
add("filter", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["filter"]), 2)
add("mask / mask-image / -webkit-mask-*", "property", "DROPS-THE-DECLARATION; mask-image != none HIDES the element's background instead", "css_extra.c:108-140 decls_masked", prop_demand(["mask", "mask-image", "-webkit-mask-image", "-webkit-mask", "mask-size", "-webkit-mask-size", "mask-position", "-webkit-mask-position", "mask-repeat", "-webkit-mask-repeat", "mask-composite", "-webkit-mask-composite"]), 2)
add("clip-path", "property", "DROPS-THE-DECLARATION; the visually-hidden idiom inset(50%) becomes display:none", "css_extra.c:76-88 decls_vish", prop_demand(["clip-path", "-webkit-clip-path"]), 2)
add("position: sticky", "property", "PARSED; laid out as RELATIVE (never sticks)", "css_engine.c:1513-1528; layout.c:2774,3175", sites_using(lambda c: sum(v for k, v in c["position_values"].items() if "sticky" in k)), 1)
add("position: fixed", "property", "SUPPORTED (out of flow; anchored to nearest positioned ancestor, does not track scroll)", "css_engine.c:1483-1527", sites_using(lambda c: c["position_values"].get("fixed", 0)), 1)
add("display: contents", "property", "DROPS-THE-DECLARATION (box stays a block)", "properties.gen:26 keyword set; synth BAD-VALUE", sites_using(lambda c: c["display_values"].get("contents", 0)), 2)
add("display: flow-root", "property", "DROPS-THE-DECLARATION", "properties.gen:26", sites_using(lambda c: c["display_values"].get("flow-root", 0)), 2)
add("display: -webkit-box / -ms-flexbox / -webkit-flex (prefixed flex)", "property", "DROPS-THE-DECLARATION (element keeps its previous display)", "properties.gen:26", sites_using(lambda c: sum(v for k, v in c["display_values"].items() if k.startswith("-"))), 1)
add("display: flex / inline-flex / grid / inline-grid", "property", "SUPPORTED", "properties.gen:26; layout_flex.c; layout_grid.c", sites_using(lambda c: sum(v for k, v in c["display_values"].items() if k in ("flex", "inline-flex", "grid", "inline-grid"))), 0)
add("gap / row-gap / column-gap", "property", "SUPPORTED via css_extra", "css_extra.c:814, 2365", prop_demand(["gap", "row-gap", "column-gap", "grid-gap"]), 0)
add("grid-template-* / grid-area / grid-column / grid-row / grid-auto-*", "property", "SUPPORTED via css_extra raw text + layout_grid.c (no subgrid/masonry)", "css.h:56-60,338; layout_grid.c:2231", prop_demand(["grid-template-columns", "grid-template-rows", "grid-template-areas", "grid-template", "grid-area", "grid-column", "grid-row", "grid-auto-columns", "grid-auto-rows", "grid-auto-flow"]), 0)
add("aspect-ratio", "property", "DROPS-THE-DECLARATION", "language.c:2768; synth UNKNOWN-PROP", prop_demand(["aspect-ratio"]), 2)
add("inset / inset-inline / inset-block", "property", "SUPPORTED via css_extra", "css_extra.c:1950, 2373-2375", prop_demand(["inset", "inset-inline", "inset-block", "inset-inline-start", "inset-inline-end", "inset-block-start", "inset-block-end"]), 0)
add("margin-inline/-block, padding-inline/-block (logical box)", "property", "SUPPORTED via css_extra (ltr only)", "css_extra.c:2376-2378 logical_one/pair", prop_demand(["margin-inline", "margin-inline-start", "margin-inline-end", "margin-block", "margin-block-start", "margin-block-end", "padding-inline", "padding-inline-start", "padding-inline-end", "padding-block", "padding-block-start", "padding-block-end"]), 0)
add("inline-size / block-size / min-max-inline-size, border-inline-*, border-start-*-radius", "property", "DROPS-THE-DECLARATION", "language.c:2768; synth UNKNOWN-PROP", prop_demand(["inline-size", "block-size", "min-inline-size", "max-inline-size", "min-block-size", "max-block-size", "border-inline", "border-inline-start", "border-inline-end", "border-block", "border-block-start", "border-block-end", "border-inline-start-color", "border-inline-end-color", "border-inline-start-width", "border-inline-end-width", "border-start-start-radius", "border-start-end-radius", "border-end-start-radius", "border-end-end-radius"]), 2)
add("box-shadow", "property", "SUPPORTED via css_extra raw capture", "css_extra.c:717; css.h:98", prop_demand(["box-shadow"]), 0)
add("text-shadow", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["text-shadow"]), 1)
add("background-size / background-clip / background-origin", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["background-size", "background-clip", "-webkit-background-clip", "background-origin"]), 2)
add("background-position / background-repeat / background-attachment", "property", "PARSED-BUT-IGNORED", "css_extra.c:2261-2262", prop_demand(["background-position", "background-repeat", "background-attachment"]), 1)
add("object-fit / object-position", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["object-fit", "object-position"]), 2)
add("text-overflow: ellipsis", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["text-overflow"]), 2)
add("word-break / overflow-wrap / word-wrap / hyphens / text-wrap", "property", "DROPS-THE-DECLARATION (cstyle has fields, 'no producer')", "css.h:311-316; language.c:2768", prop_demand(["word-break", "overflow-wrap", "word-wrap", "hyphens", "text-wrap"]), 2)
add("-webkit-line-clamp / line-clamp / -webkit-box-orient", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["-webkit-line-clamp", "line-clamp", "-webkit-box-orient"]), 2)
add("pointer-events / user-select / touch-action / appearance", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["pointer-events", "user-select", "-webkit-user-select", "touch-action", "appearance", "-webkit-appearance", "-moz-appearance"]), 1)
add("cursor", "property", "PARSED-BUT-IGNORED; CSS3 keywords refused", "css_extra.c:2263", prop_demand(["cursor"]), 0)
add("vertical-align", "property", "PARSED-BUT-IGNORED", "css_extra.c:2263 ignored list", prop_demand(["vertical-align"]), 2, "text-misalignment class")
add("content (on ::before/::after)", "property", "PARSED-BUT-IGNORED", "css_extra.c:2263", prop_demand(["content"]), 2)
add("outline / outline-offset", "property", "PARSED-BUT-IGNORED / DROPPED", "css_extra.c:2264", prop_demand(["outline", "outline-width", "outline-offset", "outline-color", "outline-style"]), 0)
add("text-decoration-* longhands / text-underline-offset", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["text-decoration-line", "text-decoration-color", "text-decoration-thickness", "text-decoration-style", "text-decoration-skip-ink", "text-underline-offset", "text-underline-position"]), 1)
add("letter-spacing / word-spacing / text-indent / text-transform / line-height / white-space", "property", "SUPPORTED", "css_engine.c getters", prop_demand(["letter-spacing", "word-spacing", "text-indent", "text-transform", "line-height", "white-space"]), 0)
add("font-family (web fonts)", "property", "PARSED; only 3 faces exist (ui/mono/text), @font-face never loaded", "fsroot/fonts; no loader", prop_demand(["font-family"]), 1)
add("font-weight", "property", "SUPPORTED (bold at >=700 only; 500/600 render regular)", "css_engine.c font_weight collapse (CLAUDE.md)", prop_demand(["font-weight"]), 1)
add("font-style: italic", "property", "PARSED; no italic face on disk", "CLAUDE.md text section", prop_demand(["font-style"]), 1)
add("font-feature-settings / font-variation-settings / font-stretch / font-display / -webkit-font-smoothing", "property", "DROPS-THE-DECLARATION (harmless)", "language.c:2768", prop_demand(["font-feature-settings", "font-variation-settings", "font-stretch", "font-display", "-webkit-font-smoothing", "-moz-osx-font-smoothing", "text-rendering", "font-optical-sizing"]), 0)
add("opacity / visibility / z-index / overflow / float / clear / box-sizing", "property", "SUPPORTED", "css_engine.c getters", prop_demand(["opacity", "visibility", "z-index", "overflow", "overflow-x", "overflow-y", "float", "clear", "box-sizing"]), 0)
add("overflow: clip", "property", "DROPS-THE-DECLARATION", "properties.gen overflow keywords", sites_using(lambda c: c["decl_of_prop_with"].get("overflow", {}).get("kw:clip", 0) + sum(1 for v in c["prop_values"].get("overflow", {}) if v == "clip")), 1)
add("border / border-radius / margin / padding / width / height / min-max", "property", "SUPPORTED", "css_engine.c convert()", prop_demand(["border", "border-radius", "margin", "padding", "width", "height", "min-width", "max-width", "min-height", "max-height"]), 0)
add("width/height: fit-content / min-content / max-content / -webkit-fill-available / stretch", "property", "DROPS-THE-DECLARATION", "utils.c length parser; synth BAD-VALUE", sites_using(lambda c: sum(n for p, vals in c["prop_values"].items() if p in ("width", "height", "min-width", "max-width", "min-height", "max-height", "flex-basis") for v, n in vals.items() if re.search(r"fit-content|min-content|max-content|fill-available|^stretch$", v))), 2)
add("justify-content/align-items: start / end / stretch / safe", "property", "DROPS-THE-DECLARATION", "properties.gen keyword sets; synth BAD-VALUE", sites_using(lambda c: sum(n for p, vals in c["prop_values"].items() if p in ("justify-content", "align-items", "align-self", "align-content") for v, n in vals.items() if re.match(r"^(start|end|stretch|self-start|self-end|left|right|safe .*|unsafe .*|normal)$", v.strip()))), 2)
add("text-align: start / end", "property", "DROPS-THE-DECLARATION", "synth BAD-VALUE", sites_using(lambda c: sum(n for v, n in c["prop_values"].get("text-align", {}).items() if v.strip() in ("start", "end"))), 1)
add("flex: <grow> 0 <basis>  (e.g. `flex:0 0 auto`, `flex:1 0 auto`)", "property", "SILENT DROP (no report; flex item keeps default sizing)", "flex.c:150-200 error overwritten; language.c:2810 css__parse_important path has no report_drop", sites_using(lambda c: sum(n for v, n in c["prop_values"].get("flex", {}).items() if re.match(r"^\s*[\d.]+\s+0\s+\S", v))), 2)
add("background shorthand with `/ size`", "property", "SILENT DROP (whole shorthand lost)", "background.c shorthand predates background-size; language.c:2810", sites_using(lambda c: sum(n for v, n in c["prop_values"].get("background", {}).items() if "/" in v)), 2)
add("flex / flex-direction / flex-wrap / order / align-* / justify-content (flex keywords)", "property", "SUPPORTED", "css_engine.c; layout_flex.c", prop_demand(["flex", "flex-direction", "flex-wrap", "flex-grow", "flex-shrink", "flex-basis", "flex-flow", "order", "align-items", "align-self", "align-content", "justify-content"]), 0)
add("place-items / place-content / justify-items / justify-self", "property", "place-*: DROPPED; justify-items/self: css_extra (grid only)", "css_extra.c:2371", prop_demand(["place-items", "place-content", "place-self", "justify-items", "justify-self"]), 1)
add("vendor-prefixed duplicates (-webkit-box-*, -ms-flex-*, -webkit-transform, -webkit-transition, ...)", "property", "DROPS-THE-DECLARATION (mostly twinned by an unprefixed declaration; twin-ness not measured)", "language.c:2768", sites_using(lambda c: sum(c["vendor_prefixed"].values())), 0)
add("fill / stroke (SVG presentation)", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["fill", "stroke", "stroke-width", "stroke-linecap", "stroke-linejoin"]), 1)
add("scroll-snap-* / scroll-margin / scroll-padding / overscroll-behavior / scroll-behavior", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["scroll-snap-type", "scroll-snap-align", "scroll-margin", "scroll-margin-top", "scroll-padding", "scroll-padding-top", "overscroll-behavior", "scroll-behavior"]), 0)
add("container-type / container / contain / content-visibility", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["container-type", "container", "container-name", "contain", "content-visibility"]), 1)
add("isolation / mix-blend-mode", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["isolation", "mix-blend-mode", "background-blend-mode"]), 1)
add("color-scheme / accent-color / caret-color", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["color-scheme", "accent-color", "caret-color"]), 0)
add("custom property definitions (--x: ...)", "property", "SUPPORTED by css_vars.c (document-wide, media-aware, specificity-ranked; NOT per-element inheritance)", "css_vars.c:1-60", sites_using(lambda c: c["custom_defs"]), 1)
add("!important", "property", "SUPPORTED", "important.c", sites_using(lambda c: c["important"]), 0)
add("all: unset/initial", "property", "DROPS-THE-DECLARATION", "language.c:2768", prop_demand(["all"]), 1)
add("border-collapse / border-spacing / table-layout / caption-side / empty-cells", "property", "PARSED-BUT-IGNORED", "css_extra.c:2265-2270", prop_demand(["border-collapse", "border-spacing", "table-layout", "caption-side", "empty-cells"]), 1)
add("columns / column-count / column-width / column-rule", "property", "PARSED-BUT-IGNORED", "css_extra.c:2268", prop_demand(["columns", "column-count", "column-width", "column-rule"]), 1)
add("list-style / list-style-type", "property", "SUPPORTED (type); list-style-image/position ignored", "css_engine.c list_style_type", prop_demand(["list-style", "list-style-type", "list-style-image", "list-style-position"]), 0)
add("writing-mode / direction / unicode-bidi", "property", "direction+writing-mode read; unicode-bidi ignored; `isolate` refused", "css_engine.c writing_mode/direction getters", prop_demand(["writing-mode", "direction", "unicode-bidi"]), 1)

ranked = sorted(D.items(), key=lambda kv: (-kv[1]["score"], -kv[1]["count"]))
res["demand_ranked"] = [k for k, v in ranked]
json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1)

# ---------------------------------------------------------------- text report
L = []
L.append("CSS DEMAND vs SUPPORT -- %d served sites (of 52 fetched, 8 refused our UA), %d rules, %d declarations (tinycss2)" % (
    NS, sum(census[s]["rules"] for s in sites), sum(census[s]["decls"] for s in sites)))
L.append("Chrome oracle on the same bytes: %d style rules (tinycss2 %d; delta 0.37%%) -- see chrome/chrome_counts.json" % (228962, 229817))
L.append("")
L.append("== DEMAND vs SUPPORT, sorted by sites-using x severity (4 sheet/block lost, 3 rule lost, 2 decl lost or parsed-but-ignored, 1 partial, 0 ok/not a defect) ==")
L.append("%-96s %5s %8s %5s  %s" % ("feature", "sites", "count", "sev", "verdict  |  anchor"))
for k, v in ranked:
    L.append("%-96s %5d %8d %5d  %s  |  %s%s" % (k[:96], v["sites"], v["count"], v["severity"], v["verdict"], v["anchor"], ("  |  " + v["note"]) if v["note"] else ""))
L.append("")
L.append("== DROPS-THE-RULE per site: style rules the tree's LibCSS refuses (per-rule oracle, any nesting depth) ==")
L.append("%-13s %7s %7s | %6s %6s %6s %6s | %6s %6s | %6s %6s | %6s %6s | %5s %5s %5s" % ("site", "rules", "decls", "lost_r", "lost_r%", "lost_d", "lost_d%", "chr_r", "chr_d", "has_r", "has_d", "pe_r", "pe_d", "@lost", "@imp", "@ff"))
tr = td = tlr = tld = tlrc = tldc = thr = thd = tpr = tpd = tar = 0
for s in sites:
    p = per_site[s]
    tr += p["rules"]; td += p["decls"]; tlr += p["parser_lost_rules"]; tld += p["parser_lost_decls"]
    tlrc += p["parser_lost_rules_chrome_valid"]; tldc += p["parser_lost_decls_chrome_valid"]
    thr += p["has_inert_rules"]; thd += p["has_inert_decls"]; tpr += p["pseudo_element_rules"]; tpd += p["pseudo_element_decls"]; tar += p["atblock_lost_rules"]
    L.append("%-13s %7d %7d | %6d %5.2f%% %6d %5.2f%% | %6d %6d | %6d %6d | %6d %6d | %5d %5d %5d" % (
        s, p["rules"], p["decls"], p["parser_lost_rules"], 100.0 * p["parser_lost_rules"] / max(p["rules"], 1),
        p["parser_lost_decls"], 100.0 * p["parser_lost_decls"] / max(p["decls"], 1),
        p["parser_lost_rules_chrome_valid"], p["parser_lost_decls_chrome_valid"], p["has_inert_rules"], p["has_inert_decls"],
        p["pseudo_element_rules"], p["pseudo_element_decls"], p["atblock_lost_rules"], p["import_rules"], p["fontface"]))
L.append("%-13s %7d %7d | %6d %5.2f%% %6d %5.2f%% | %6d %6d | %6d %6d | %6d %6d | %5d" % ("TOTAL", tr, td, tlr, 100.0 * tlr / tr, tld, 100.0 * tld / td, tlrc, tldc, thr, thd, tpr, tpd, tar))
L.append("  lost_r/lost_d = rules/declarations refused by LibCSS's selector parser (whole list); chr_* = the subset whose refusal reason Chrome ACCEPTS (engine defect);")
L.append("  has_* = accepted rules whose every selector carries :has() (never match here); pe_* = accepted rules whose every selector is ::before/::after (pseudo-element never generated);")
L.append("  @lost = style rules inside at-rule blocks LibCSS discards (@property/@starting-style/@font-feature-values/...; @keyframes excluded, css_extra captures those); @imp = @import rules (sheet never fetched); @ff = @font-face rules (never loaded)")
L.append("")
L.append("== DROPS-THE-RULE by cause (rules / declarations carried / sites) ==")
for k, v in res["rule_loss_classes"].items():
    L.append("  %-100s %6d rules %7d decls %3d sites" % (k[:100], v["rules"], v["decls"], v["sites"]))
if unattributed:
    L.append("  UNATTRIBUTED examples:")
    for s, t in unattributed[:25]: L.append("     %-11s %s" % (s, t))
L.append("")
L.append("== DROPS-THE-DECLARATION: what reached LibCSS's parseProperty over the corpus ==")
L.append("  LibCSS alone           : accepted %d  unknown-prop %d  bad-value %d  trailing %d" % (tot1[3], tot1[0], tot1[1], tot1[2]))
L.append("  behind css_vars pre-pass: accepted %d  unknown-prop %d  bad-value %d  trailing %d   (the browser's real pipeline order; var() resolved)" % (tot2[3], tot2[0], tot2[1], tot2[2]))
L.append("  SILENT drops (no report at all, language.c css__parse_important path) are invisible to both counts; measured per value below.")
L.append("")
L.append("== refused/silent VALUES by class (declarations, sites) -- LibCSS alone, from the per-value oracle ==")
for k, v in res["value_classes"].items():
    L.append("  %-110s %7d decls %3d sites   e.g. %s" % (k[:110], v["decls"], v["sites"], "; ".join(v["examples"])[:120]))
L.append("")
L.append("== top dropped properties after the var() pre-pass (unknown+bad, accepted, sites) ==")
for k, v in list(res["prop_drops_after_var_prepass"].items())[:70]:
    L.append("  %-36s unknown=%6d bad=%6d accepted=%6d sites=%2d" % (k, v["unknown"], v["bad"], v["accepted"], v["sites"]))
L.append("")
L.append("== ANIMATION demand ==")
L.append("  @keyframes rules: %d over %d sites; properties animated in keyframes (stops): %s" % (sum(census[s]["kf_rules"] for s in sites), sum(1 for s in sites if census[s]["kf_rules"]), dict(kf_props.most_common(15))))
L.append("  sites whose keyframes animate something other than opacity/transform: %d of %d (the clock interpolates only those two: js_anim.c:2013-2019)" % (kf_sites_other, NS))
L.append("  transition-property first idents: %s" % dict(trans_props.most_common(12)))
L.append("  rules with animation-* longhands and no `animation` shorthand: %d; transition-* longhands only: %d" % (anim_lh_only, trans_lh_only))
L.append("  sites with > 64 @keyframes in one sheet (css_extra cap CSS_KF_MAXRULE): %s" % ", ".join("%s(%d)" % (s, census[s]["kf_rules"]) for s in sites if census[s]["kf_rules"] > 64))
L.append("  display values: %s" % dict(display_vals.most_common(16)))
open(os.path.join(out, "results.txt"), "w").write("\n".join(L) + "\n")
print("\n".join(L))
