#!/usr/bin/env python3
"""census.py -- CSS FEATURE DEMAND CENSUS over a fetched corpus, with tinycss2
(a real CSS Syntax Level 3 tokenizer/parser), joined to what the tree's OWN
LibCSS refused (dropdump.c output: SELDROP / DECL lines per source).

  census.py <corpus-dir> <drops-dir> <out-dir>

Per site it writes <out>/<site>.json; and <out>/aggregate.json across sites.
Everything counted is a NUMBER PRODUCED HERE; nothing is inferred from source
reading. What a feature *means* for the engine is decided elsewhere (the
support map); this file only counts demand and joins the refusals.

Units of measure:
  rule  = one style rule (a selector list + a block), nested rules included,
          counted where they appear (inside @media etc.)
  decl  = one declaration inside a style rule (keyframe stops and @font-face
          descriptors are counted separately, not as decls)
"""
import os, re, sys, json, collections
import tinycss2
from tinycss2 import ast

GROUP_AT = {"media", "supports", "layer", "container", "scope", "starting-style",
            "document", "-moz-document"}
# what THIS LibCSS knows how to enter (language.c handleStartAtRule + the
# at_rule_is_group patch): everything else is discarded WITH ITS BLOCK.
LIBCSS_AT = {"charset", "import", "namespace", "media", "font-face", "page",
             "layer", "container", "scope", "supports"}
LIBCSS_PSEUDO = set("""first-child link visited hover active focus lang left right first root
nth-child nth-last-child nth-of-type nth-last-of-type last-child first-of-type last-of-type
only-child only-of-type empty target enabled disabled checked not is where focus-within
focus-visible any-link defined placeholder-shown modal user-invalid has host dir first-line
first-letter before after marker placeholder backdrop part slotted selection""".split())

VALUE_FUNCS = ["var", "calc", "clamp", "min", "max", "env", "attr", "color-mix", "rgb", "rgba",
               "hsl", "hsla", "hwb", "oklch", "oklab", "lab", "lch", "color", "url", "image-set",
               "linear-gradient", "radial-gradient", "conic-gradient", "repeating-linear-gradient",
               "repeating-radial-gradient", "repeat", "minmax", "fit-content", "translate", "translatex",
               "translatey", "translate3d", "scale", "rotate", "matrix", "blur", "drop-shadow",
               "inset", "polygon", "circle", "cubic-bezier", "steps", "counter", "counters",
               "format", "local", "light-dark", "round", "abs", "sign", "mod", "rem", "pow", "sqrt",
               "anchor", "path", "ray", "-webkit-gradient", "-webkit-image-set", "cross-fade",
               "element", "layer"]
UNITS = ["px", "em", "rem", "%", "vw", "vh", "vmin", "vmax", "dvh", "svh", "lvh", "dvw", "svw", "lvw",
         "ch", "ex", "lh", "rlh", "cqw", "cqh", "cqi", "cqb", "cqmin", "cqmax", "pt", "cm", "mm", "in",
         "pc", "q", "deg", "rad", "grad", "turn", "s", "ms", "fr", "dpi", "dppx", "x", "cap", "ic", "vi", "vb"]

def norm_key(s):
    return re.sub(r"\s+", "", s).replace("\\", "").lower()

def ser(tokens):
    return tinycss2.serialize(tokens)

def split_commas(tokens):
    out, cur = [], []
    for t in tokens:
        if t.type == "literal" and t.value == ",":
            out.append(cur); cur = []
        else:
            cur.append(t)
    out.append(cur)
    return out

# ---------------------------------------------------------------- selectors
PSEUDO_RE = re.compile(r"::?([a-zA-Z-]+)(\()?")
def selector_features(sel_tokens, feats):
    """Record selector-level features of ONE selector (no commas) into feats.
    Returns the set of feature names found (for rule-level attribution)."""
    found = set()
    text = ser(sel_tokens)
    # pseudo-classes / elements via tokens: ':' literal followed by ident/function
    i = 0
    toks = [t for t in sel_tokens if t.type != "comment"]
    depth_attr = 0
    while i < len(toks):
        t = toks[i]
        if t.type == "literal" and t.value == ":":
            dbl = False
            j = i + 1
            if j < len(toks) and toks[j].type == "literal" and toks[j].value == ":":
                dbl = True; j += 1
            if j < len(toks):
                n = toks[j]
                if n.type == "ident":
                    name = n.lower_value
                    found.add(("pseudo-element::" if dbl else "pseudo:") + name)
                    if name in ("before", "after") and not dbl: found.add("pseudo-element::" + name)
                elif n.type == "function":
                    name = n.lower_name
                    found.add(("pseudo-element::" if dbl else "pseudo:") + name + "()")
                    inner = n.arguments
                    itext = ser(inner)
                    if name in ("nth-child", "nth-last-child") and re.search(r"\bof\b", itext):
                        found.add("pseudo:nth-child(An+B of S)")
                    if name in ("is", "where", "not", "has"):
                        # combinator inside?
                        inner_nows = [x for x in inner if x.type != "whitespace" and x.type != "comment"]
                        has_comb = any(x.type == "literal" and x.value in (">", "+", "~") for x in inner_nows)
                        # descendant combinator: whitespace between two non-comma tokens
                        for k in range(1, len(inner) - 1):
                            if inner[k].type == "whitespace":
                                a, b = inner[k-1], inner[k+1]
                                if not (a.type == "literal" and a.value == ",") and not (b.type == "literal" and b.value == ","):
                                    has_comb = True
                        if has_comb: found.add("pseudo:%s() with combinator inside" % name)
                        if name == "not" and any(x.type == "literal" and x.value == "," for x in inner_nows):
                            found.add("pseudo:not() with selector list")
                        # unknown pseudo inside :not()
                        if name == "not":
                            for m in PSEUDO_RE.finditer(itext):
                                if m.group(1).lower() not in LIBCSS_PSEUDO and not m.group(1).lower().startswith("-webkit-"):
                                    found.add("pseudo:not(<unknown pseudo>)")
                    if name == "nth-child" or name == "nth-last-child" or name == "nth-of-type":
                        pass
                i = j + 1
                continue
        elif t.type == "[] block":
            inner = [x for x in t.content if x.type not in ("whitespace", "comment")]
            ops = [x.value for x in inner if x.type == "literal"]
            op = "".join(ops)
            if "=" in op:
                if "^" in op: found.add("attr[^=]")
                elif "$" in op: found.add("attr[$=]")
                elif "*" in op: found.add("attr[*=]")
                elif "~" in op: found.add("attr[~=]")
                elif "|" in op: found.add("attr[|=]")
                else: found.add("attr[=]")
            else:
                found.add("attr[presence]")
            if inner and inner[-1].type == "ident" and inner[-1].lower_value in ("i", "s") and len(inner) >= 3 and "=" in op:
                found.add("attr[... i/s flag]")
        elif t.type == "literal" and t.value == "&":
            found.add("nesting &")
        elif t.type == "literal" and t.value == "|":
            found.add("namespace |")
        elif t.type == "literal" and t.value in (">", "+", "~"):
            found.add("combinator " + t.value)
        elif t.type == "whitespace":
            # descendant combinator if surrounded by compound parts
            if 0 < i < len(toks) - 1 and toks[i-1].type not in ("literal",) and toks[i+1].type not in ("literal",):
                found.add("combinator descendant")
            elif 0 < i < len(toks) - 1 and toks[i+1].type in ("ident", "hash", "[] block") or (0 < i < len(toks)-1 and toks[i+1].type == "literal" and toks[i+1].value in (".", ":", "*")):
                if toks[i-1].type != "literal" or toks[i-1].value in (")", "]", "*"):
                    found.add("combinator descendant")
        i += 1
    for f in found:
        feats[f] += 1
    return found

def selector_refusal_reason(found):
    """Static model of language.c's refusal rules, per selector. Returns the
    first reason or None. Checked against the REAL parser's SELDROP count as a
    control; the real count is authoritative, this only attributes."""
    for f in found:
        if f.startswith("pseudo:") and f.endswith("()"):
            name = f[len("pseudo:"):-2]
            if name not in LIBCSS_PSEUDO: return "unknown pseudo-class :%s()" % name
        elif f.startswith("pseudo:") and "(" not in f and " " not in f:
            name = f[len("pseudo:"):]
            if name not in LIBCSS_PSEUDO: return "unknown pseudo-class :%s" % name
        elif f.startswith("pseudo-element::"):
            name = f[len("pseudo-element::"):].rstrip("()")
            if name not in LIBCSS_PSEUDO and not name.startswith("-webkit-"): return "unknown pseudo-element ::%s" % name
    if "pseudo:nth-child(An+B of S)" in found: return ":nth-child(An+B of S)"
    if "attr[... i/s flag]" in found: return "attribute selector i/s flag"
    for f in found:
        if f.endswith("with combinator inside") and (f.startswith("pseudo:is") or f.startswith("pseudo:where")):
            return ":is()/:where() with a combinator inside"
    if "pseudo:not(<unknown pseudo>)" in found: return ":not(<unknown pseudo>)"
    if "nesting &" in found: return "CSS nesting (&)"
    return None

# ---------------------------------------------------------------- values
def walk_value(tokens, acc):
    for t in tokens:
        if t.type == "function":
            acc["func"][t.lower_name] += 1
            walk_value(t.arguments, acc)
        elif t.type in ("() block", "[] block", "{} block"):
            walk_value(t.content, acc)
        elif t.type == "dimension":
            acc["unit"][t.lower_unit] += 1
        elif t.type == "percentage":
            acc["unit"]["%"] += 1
        elif t.type == "ident":
            acc["ident"][t.lower_value] += 1
        elif t.type == "hash":
            acc["hash"] += 1
        elif t.type == "url":
            acc["func"]["url"] += 1

class SiteCensus:
    def __init__(self, name):
        self.name = name
        self.rules = 0; self.decls = 0; self.nested_rules = 0; self.nested_decls = 0
        self.sources = 0; self.css_bytes = 0
        self.prop = collections.Counter()           # property name -> decls
        self.prop_rules = collections.Counter()
        self.custom_defs = 0
        self.important = 0
        self.vendor_prefixed = collections.Counter()  # prefix -> decls
        self.func = collections.Counter()            # function name -> decls containing (per decl once)
        self.unit = collections.Counter()
        self.func_decls = collections.Counter()
        self.unit_decls = collections.Counter()
        self.selfeat = collections.Counter()         # feature -> selectors
        self.selfeat_rules = collections.Counter()   # feature -> rules
        self.selfeat_decls = collections.Counter()   # feature -> decls carried
        self.atrule = collections.Counter()          # at-rule name -> count
        self.atrule_rules = collections.Counter()    # rules inside
        self.atrule_decls = collections.Counter()    # decls inside
        self.media_feat = collections.Counter()      # media feature name -> @media count
        self.media_range = 0
        self.supports_kind = collections.Counter()
        self.kf_props = collections.Counter()        # property animated in @keyframes -> stops
        self.kf_rules = 0
        self.fontface = 0; self.fontface_src_formats = collections.Counter()
        self.prop_values = collections.defaultdict(collections.Counter)   # prop -> value text -> n
        self.decl_of_prop_with = collections.defaultdict(collections.Counter)  # prop -> feature -> decls
        self.display_values = collections.Counter()
        self.position_values = collections.Counter()
        self.anim_longhand_only_rules = 0  # rules with animation-* longhand but no `animation` shorthand
        self.trans_longhand_only_rules = 0
        self.transition_props = collections.Counter()  # transition-property values (first ident)
        self.rule_records = []
        self.rulekeys = {}    # per source index: key -> (ndecls, features)  for the SELDROP join
        self.seldrop_matched = 0; self.seldrop_unmatched = 0
        self.seldrop_rules = 0; self.seldrop_decls = 0
        self.seldrop_reason = collections.Counter(); self.seldrop_reason_decls = collections.Counter()
        self.seldrop_texts_unmatched = []
        self.seldrop_texts_unattributed = []
        self.atblock_lost_rules = collections.Counter(); self.atblock_lost_decls = collections.Counter()
        self.real = collections.defaultdict(lambda: [0, 0, 0, 0])   # name -> [unknown, bad, trailing, accepted]
        self.real_at = collections.Counter()

    def decl(self, d, rule_feats, in_at):
        name = d.lower_name
        self.decls += 1
        if in_at: self.nested_decls += 0
        if name.startswith("--"):
            self.custom_defs += 1
            return
        self.prop[name] += 1
        if d.important: self.important += 1
        m = re.match(r"-(webkit|moz|ms|o)-", name)
        if m: self.vendor_prefixed["-" + m.group(1) + "-"] += 1
        acc = {"func": collections.Counter(), "unit": collections.Counter(), "ident": collections.Counter(), "hash": 0}
        walk_value(d.value, acc)
        for f in acc["func"]:
            self.func_decls[f] += 1
            self.decl_of_prop_with[name]["func:" + f] += 1
        for u in acc["unit"]:
            self.unit_decls[u] += 1
            self.decl_of_prop_with[name]["unit:" + u] += 1
        for f, n in acc["func"].items(): self.func[f] += n
        for u, n in acc["unit"].items(): self.unit[u] += n
        vtext = ser(d.value).strip()
        self.prop_values[name][vtext] += 1
        if name == "display": self.display_values[vtext.lower()] += 1
        if name == "position": self.position_values[vtext.lower()] += 1
        if name == "transition-property" or name == "transition":
            ids = [t.lower_value for t in d.value if t.type == "ident"]
            if ids: self.transition_props[ids[0]] += 1
        for kw in ("initial", "inherit", "unset", "revert", "revert-layer"):
            if acc["ident"].get(kw): self.decl_of_prop_with[name]["kw:" + kw] += 1

    def style_rule(self, rule, ctx, src_index):
        self.rules += 1
        if ctx.get("nested"): self.nested_rules += 1
        for a in ctx.get("at", []): self.atrule_rules[a] += 1
        prelude = [t for t in rule.prelude if t.type != "comment"]
        sels = split_commas(prelude)
        feats_this = set()
        reasons = []
        for s in sels:
            s2 = [t for t in s]
            while s2 and s2[0].type == "whitespace": s2 = s2[1:]
            while s2 and s2[-1].type == "whitespace": s2 = s2[:-1]
            if not s2: continue
            found = selector_features(s2, self.selfeat)
            feats_this |= found
            r = selector_refusal_reason(found)
            if r: reasons.append(r)
        if len(sels) > 1: feats_this.add("selector list (comma)")
        for f in feats_this: self.selfeat_rules[f] += 1
        # declarations (+ nested rules)
        ndecl = 0
        items = tinycss2.parse_blocks_contents(rule.content, skip_comments=True, skip_whitespace=True)
        has_anim_sh = has_anim_lh = has_tr_sh = has_tr_lh = False
        for it in items:
            if it.type == "declaration":
                self.decl(it, feats_this, ctx.get("at"))
                ndecl += 1
                n = it.lower_name
                if n == "animation": has_anim_sh = True
                elif n.startswith("animation-"): has_anim_lh = True
                if n == "transition": has_tr_sh = True
                elif n.startswith("transition-"): has_tr_lh = True
                for a in ctx.get("at", []): self.atrule_decls[a] += 1
            elif it.type == "qualified-rule":
                self.selfeat_rules["nesting (nested style rule)"] += 1
                c2 = dict(ctx); c2["nested"] = True
                self.style_rule(it, c2, src_index)
            elif it.type == "at-rule":
                self.selfeat_rules["nesting (nested at-rule)"] += 1
                c2 = dict(ctx); c2["nested"] = True
                self.at_rule(it, c2, src_index)
        if has_anim_lh and not has_anim_sh: self.anim_longhand_only_rules += 1
        if has_tr_lh and not has_tr_sh: self.trans_longhand_only_rules += 1
        for f in feats_this: self.selfeat_decls[f] += ndecl
        key = norm_key(ser(prelude))
        rk = self.rulekeys.setdefault(src_index, {})
        rk.setdefault(key, []).append((ndecl, reasons, ser(prelude).strip()))
        self.rule_records.append({"src": "%s#%d" % src_index, "sel": ser(prelude).strip(), "nd": ndecl,
                                  "reasons": reasons, "at": ctx.get("at", []), "nested": bool(ctx.get("nested")),
                                  "feats": sorted(feats_this)})
        return ndecl

    def at_rule(self, rule, ctx, src_index):
        name = rule.lower_at_keyword
        self.atrule[name] += 1
        for a in ctx.get("at", []): pass
        pre = ser(rule.prelude).strip()
        c2 = dict(ctx); c2["at"] = ctx.get("at", []) + [name]
        if name == "media":
            for m in re.finditer(r"\(\s*([-a-zA-Z0-9]+)\s*[:)]", pre): self.media_feat[m.group(1).lower()] += 1
            if re.search(r"\(\s*[-a-zA-Z0-9]+\s*(<=|>=|<|>|=)|(<=|>=|<|>)\s*[-a-zA-Z]+\s*(<=|>=|<|>)", pre):
                self.media_range += 1; self.media_feat["<range syntax>"] += 1
            for kw in ("print", "screen", "not ", "only ", " and ", " or ", ","):
                if kw in pre.lower(): self.media_feat["kw:" + kw.strip()] += 1
        if name == "supports":
            if "selector(" in pre: self.supports_kind["selector()"] += 1
            if re.search(r"\bnot\b", pre): self.supports_kind["not"] += 1
            if re.search(r"\bor\b", pre): self.supports_kind["or"] += 1
            for m in re.finditer(r"\(\s*(-?[a-zA-Z-]+)\s*:", pre): self.supports_kind["prop:" + m.group(1).lower()] += 1
        if name == "layer" and rule.content is None:
            self.atrule["layer (statement)"] += 1
        if name == "import":
            self.atrule["import"] += 0
        if rule.content is None:
            return
        if name in GROUP_AT:
            for r in tinycss2.parse_rule_list(rule.content, skip_comments=True, skip_whitespace=True):
                if r.type == "qualified-rule": self.style_rule(r, c2, src_index)
                elif r.type == "at-rule": self.at_rule(r, c2, src_index)
            return
        if name in ("keyframes", "-webkit-keyframes", "-moz-keyframes"):
            self.kf_rules += 1
            for r in tinycss2.parse_rule_list(rule.content, skip_comments=True, skip_whitespace=True):
                if r.type == "qualified-rule":
                    for d in tinycss2.parse_declaration_list(r.content, skip_comments=True, skip_whitespace=True):
                        if d.type == "declaration": self.kf_props[d.lower_name] += 1
            return
        if name == "font-face":
            self.fontface += 1
            for d in tinycss2.parse_declaration_list(rule.content, skip_comments=True, skip_whitespace=True):
                if d.type == "declaration" and d.lower_name == "src":
                    for t in d.value:
                        if t.type == "function" and t.lower_name == "format":
                            self.fontface_src_formats[ser(t.arguments).strip().strip("'\"").lower()] += 1
                        if t.type == "function" and t.lower_name == "local": self.fontface_src_formats["local()"] += 1
            return
        # any other at-rule with a block: count what is inside it (would be lost)
        lost_r = lost_d = 0
        try:
            for r in tinycss2.parse_rule_list(rule.content, skip_comments=True, skip_whitespace=True):
                if r.type == "qualified-rule":
                    lost_r += 1
                    for d in tinycss2.parse_declaration_list(r.content, skip_comments=True, skip_whitespace=True):
                        if d.type == "declaration": lost_d += 1
        except Exception:
            pass
        if name not in LIBCSS_AT:
            self.atblock_lost_rules[name] += lost_r; self.atblock_lost_decls[name] += lost_d
            if name == "property":
                pass

    def source(self, text, src_index):
        self.sources += 1; self.css_bytes += len(text)
        for r in tinycss2.parse_stylesheet(text, skip_comments=True, skip_whitespace=True):
            if r.type == "qualified-rule": self.style_rule(r, {}, src_index)
            elif r.type == "at-rule": self.at_rule(r, {}, src_index)

    def join_drops(self, drops_path):
        cur = None
        rk = None
        for line in open(drops_path, encoding="utf-8", errors="replace"):
            p = line.rstrip("\n").split("\t")
            if p[0] == "SRC":
                cur = (os.path.basename(p[1]), int(p[2])); rk = self.rulekeys.get(cur, {})
            elif p[0] == "SELDROP":
                text = p[1] if len(p) > 1 else ""
                if text.startswith("@"):
                    self.real_at["@" + text[1:].split("(")[0].split(" ")[0].lower()] += 1
                    continue
                key = norm_key(text)
                hit = None
                if key in rk and rk[key]:
                    hit = rk[key].pop(0)
                elif len(text) >= 185:
                    for k in list(rk.keys()):
                        if k.startswith(key[:170]) and rk[k]:
                            hit = rk[k].pop(0); break
                self.seldrop_rules += 1
                if hit:
                    self.seldrop_matched += 1
                    nd, reasons, pretty = hit
                    self.seldrop_decls += nd
                    reason = reasons[0] if reasons else "UNATTRIBUTED"
                    self.seldrop_reason[reason] += 1; self.seldrop_reason_decls[reason] += nd
                    if not reasons and len(self.seldrop_texts_unattributed) < 40:
                        self.seldrop_texts_unattributed.append(pretty[:160])
                else:
                    self.seldrop_unmatched += 1
                    if len(self.seldrop_texts_unmatched) < 20: self.seldrop_texts_unmatched.append(text[:160])
            elif p[0] == "DECL":
                name, reason = p[1], int(p[2])
                if name.startswith("@"):
                    self.real_at[name.lower()] += 1
                else:
                    self.real[name.lower()][reason] += 1

    def result(self):
        modeled_refused = sum(1 for src in self.rulekeys.values() for lst in src.values() for (nd, reasons, _) in lst if reasons)
        real_acc = sum(v[3] for v in self.real.values())
        real_unk = sum(v[0] for v in self.real.values())
        real_bad = sum(v[1] for v in self.real.values())
        return {
            "site": self.name, "sources": self.sources, "css_bytes": self.css_bytes,
            "rules": self.rules, "decls": self.decls, "custom_defs": self.custom_defs,
            "nested_rules": self.nested_rules, "important": self.important,
            "prop": dict(self.prop.most_common()),
            "vendor_prefixed": dict(self.vendor_prefixed),
            "func_decls": dict(self.func_decls.most_common()),
            "unit_decls": dict(self.unit_decls.most_common()),
            "selfeat": dict(self.selfeat.most_common()),
            "selfeat_rules": dict(self.selfeat_rules.most_common()),
            "selfeat_decls": dict(self.selfeat_decls.most_common()),
            "atrule": dict(self.atrule.most_common()),
            "atrule_rules": dict(self.atrule_rules), "atrule_decls": dict(self.atrule_decls),
            "media_feat": dict(self.media_feat.most_common()), "media_range": self.media_range,
            "supports_kind": dict(self.supports_kind.most_common()),
            "kf_rules": self.kf_rules, "kf_props": dict(self.kf_props.most_common()),
            "fontface": self.fontface, "fontface_src_formats": dict(self.fontface_src_formats),
            "display_values": dict(self.display_values.most_common()),
            "position_values": dict(self.position_values.most_common()),
            "anim_longhand_only_rules": self.anim_longhand_only_rules,
            "trans_longhand_only_rules": self.trans_longhand_only_rules,
            "transition_props": dict(self.transition_props.most_common(30)),
            "decl_of_prop_with": {k: dict(v) for k, v in self.decl_of_prop_with.items()},
            "prop_values": {k: dict(v.most_common(200)) for k, v in self.prop_values.items()},
            "atblock_lost_rules": dict(self.atblock_lost_rules), "atblock_lost_decls": dict(self.atblock_lost_decls),
            "real": {k: v for k, v in self.real.items()}, "real_at": dict(self.real_at),
            "real_accepted": real_acc, "real_unknown": real_unk, "real_bad": real_bad,
            "seldrop_rules": self.seldrop_rules, "seldrop_matched": self.seldrop_matched,
            "seldrop_unmatched": self.seldrop_unmatched, "seldrop_decls": self.seldrop_decls,
            "seldrop_reason": dict(self.seldrop_reason), "seldrop_reason_decls": dict(self.seldrop_reason_decls),
            "seldrop_texts_unmatched": self.seldrop_texts_unmatched,
            "seldrop_texts_unattributed": self.seldrop_texts_unattributed,
            "modeled_refused_rules": modeled_refused,
        }

STYLE_RE = re.compile(r"<style\b[^>]*>(.*?)</style", re.I | re.S)

def run_site(corpus, drops, out, site):
    d = os.path.join(corpus, site)
    man = open(os.path.join(d, "MANIFEST"), encoding="utf-8", errors="replace").read()
    if "\nREFUSED" in man: return None
    sc = SiteCensus(site)
    # the same sources dropdump.c walks: every *.css and every <style> in *.html
    for fn in sorted(os.listdir(d)):
        p = os.path.join(d, fn)
        if fn.endswith(".css"):
            sc.source(open(p, encoding="utf-8", errors="replace").read(), (fn, 0))
        elif fn.endswith(".html"):
            html = open(p, encoding="utf-8", errors="replace").read()
            for i, m in enumerate(STYLE_RE.finditer(html)):
                sc.source(m.group(1), (fn, i))
    dp = os.path.join(drops, site + ".tsv")
    if os.path.exists(dp): sc.join_drops(dp)
    r = sc.result()
    json.dump(r, open(os.path.join(out, site + ".json"), "w"), indent=0)
    with open(os.path.join(out, "rules-" + site + ".jsonl"), "w") as f:
        for rec in sc.rule_records: f.write(json.dumps(rec) + "\n")
    with open(os.path.join(out, "values-" + site + ".jsonl"), "w") as f:
        for prop, cnt in sc.prop_values.items():
            for v, n in cnt.items(): f.write(json.dumps([prop, v, n]) + "\n")
    return r

def main():
    corpus, drops, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    sites = sorted(x for x in os.listdir(corpus) if os.path.isdir(os.path.join(corpus, x)))
    res = []
    for s in sites:
        r = run_site(corpus, drops, out, s)
        if r:
            res.append(r)
            print("%-13s src=%-3d rules=%-6d decls=%-7d real(acc/unk/bad)=%d/%d/%d seldrop=%d(matched %d, unmatched %d, model %d) decls-in-seldrop=%d" % (
                s, r["sources"], r["rules"], r["decls"], r["real_accepted"], r["real_unknown"], r["real_bad"],
                r["seldrop_rules"], r["seldrop_matched"], r["seldrop_unmatched"], r["modeled_refused_rules"], r["seldrop_decls"]))
    json.dump(res, open(os.path.join(out, "all.json"), "w"))

if __name__ == "__main__":
    main()
