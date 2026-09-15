#!/usr/bin/env python3
"""Repeatable source/build inventory, deliberately NOT runtime conformance.

Make expands its own source lists after continuations are joined for inspection.
The ELF symbol table proves a provider was linked; explicit source edges prove
that a call/binding was written. Neither proves that a page executes that edge.
Known orphans stay visible even when every required wiring edge passes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

B = 'c/apps/browser/'
VARIABLES = ('BROWSER_PIPE', 'BROWSER_OBJ', 'BROWSER_JS_SRC', 'BROWSER_JS_OBJ', 'CSS_OBJ')
# A small reviewed manifest, not a claim to infer a complete C/JS call graph.
# Full call expressions distinguish calls from extern declarations/weak stubs.
FEATURES = [
    ('digest', ['js_subtle_install', 'sha256', 'sha384', 'sha512'], [
        ('js_page.c', r'js_subtle_install\s*\(g_ctx\)'),
        ('js_subtle.c', r'JS_NewCFunction\s*\(ctx,\s*subtle_digest_native,')]),
    ('crypto_ops', ['hmac', 'aes128_gcm_seal_iv', 'aes128_gcm_open_iv'], [
        ('js_subtle.c', r'JS_NewCFunction\s*\(ctx,\s*subtle_gcm_native,')]),
    ('live_range', ['dom_range_init', 'dom_range_set'], [
        ('js_page.c', r'js_forms_install\s*\(g_ctx\)'),
        ('js_forms.c', r'range_native_install\s*\(ctx\)'),
        ('js_live_range_native.inc', r'dom_range_init\s*\(r,'),
        ('dom_mutation.inc', r'dom_subscribe\s*\(d,\s*&r->sub,\s*dom_range_mutate,')]),
    ('native_mutation_observer', ['dom_subscribe', 'js_platform_mutations_flush'], [
        ('js_page.c', r'js_platform_install\s*\(g_ctx\)'),
        ('js_platform.c', r'dom_subscribe\s*\(mo_root->doc,\s*&mo_subscription,\s*native_mo_notify,')]),
    ('generated_content', ['css_apply', 'layout_page', 'browser_paint'], [
        ('css_engine.c', r'generated_text\s*\(pseudo\[pi\],\s*n,\s*g->text.text\)'),
        ('css_engine.c', r'o->generated\[pi\]\s*=\s*&g->box'),
        ('layout.c', r's->generated\[0\]'),
        ('layout.c', r'it->generated_style\s*=\s*os'),
        ('browser_paint.c', r'return\s+e->generated_style')]),
    ('modal', ['top_layer_push', 'top_layer_escape', 'top_layer_reset'], [
        ('js_page.c', r'js_semantics_install\s*\(g_ctx\)'),
        ('js_semantics.c', r'top_layer_push\s*\(n\)'),
        ('layout.c', r'top_layer_at\s*\(ti\)'),
        ('browser_paint.c', r'if\s*\(owner\s*!=\s*modal\)\s*continue'),
        ('browser.c', r'top_layer_escape\s*\(\)'),
        ('browser.c', r'top_layer_reset\s*\(\)')]),
    ('native_focus_inert', ['focus_is_inert', 'focus_set', 'css_ensure_styled', 'js_dom_mutation_generation'], [
        ('css_engine.c', r'js_dom_mutation_generation\s*\(\)'),
        ('js_forms.c', r'css_ensure_styled\s*\(n\)'),
        ('focus.c', r'top_layer_allows_input\s*\(n\)'),
        ('browser.c', r'focus_is_inert\s*\(n\)'),
        ('browser_paint.c', r'focus_is_inert\s*\(e->node\)')]),
    ('content_supports', ['logit_css_generated_supports'], [
        ('css_engine.c', r'logit_css_generated_supports\s*\(st->bytecode,\s*st->used\)'),
        ('../../../third_party/css/libcss/src/parse/language.c', r'logit_css_generated_supports\s*\(style->bytecode,\s*style->used\)')]),
    ('live_matchmedia', ['css_media_matches', 'js_webapi_set_viewport'], [
        ('js_webapi.c', r'css_media_matches\s*\(q,\s*-1\)'),
        ('js_cssom.c', r'if\s*\(!JS_IsFunction\s*\(ctx,\s*media_fn\)\)'),
        ('browser.c', r'js_webapi_set_viewport\s*\(win_w,\s*VIEW_H\)')]),
    ('cssom_reflow', ['js_cssom_set_reflow'], [
        ('browser.c', r'js_cssom_set_reflow\s*\(browser_cssom_reflow\)'),
        ('js_cssom.c', r'g_reflow\s*\(\)'),
        ('js_cssom.c', r'g_saw_dirty\s*=\s*js_dom_dirty\s*\(\)')]),
]
# Correction to the initial five-orphan audit: four now have native consumers.
# Behavioral gates and guest pages establish the supported subsets separately.
FEATURES += [
    ('text_formatter', ['ltx_layout_runs', 'ltx_measure_run'], [
        ('layout.c', r'ltx_layout_runs\s*\(b->runs,'),
        ('browser_paint.c', r'paint_text_run_spaced\s*\(e,')]),
    ('element_scroll', ['js_cssom_project_item', 'js_cssom_dispatch_element_scroll'], [
        ('browser_paint.c', r'js_cssom_project_item\s*\(&scrolled\)'),
        ('js_cssom.c', r'js_cssom_project_item\s*\(&projected\)'),
        ('browser.c', r'js_cssom_dispatch_element_scroll\s*\(\)'),
        ('browser.c', r'js_cssom_scroll_element_by\s*\(wheel_target,')]),
    ('popover', ['top_layer_push_popover', 'top_layer_is_hidden_popover'], [
        ('js_semantics.c', r'top_layer_push_popover\s*\('),
        ('js_semantics.c', r'js_dom_top_layer_changed\s*\(n\)'),
        ('browser.c', r'js_semantics_activate_invoker\s*\(n\)'),
        ('layout.c', r'top_layer_is_hidden_popover\s*\('),
        ('browser_paint.c', r'top_layer_is_modal\s*\(modal\)'),
        ('browser.c', r'top_layer_pointer_up\s*\(n\)')]),
    ('waapi', ['js_anim_install', 'js_anim_close', 'css_anim_tick'], [
        ('js_anim.c', r'wa_render\s*\(\)'),
        ('js_page.c', r'js_anim_close\s*\(g_ctx\)')]),
]
ORPHANS = [
    ('vertical_writing', 'css_engine.c', r'o->writing_mode\s*=',
     'writing_mode is converted, but vertical line layout/font metrics/glyph orientation remain absent. Horizontal text wiring does not establish vertical support.'),
]



def scrub(text):
    """Blank comments and literals, preserving offsets/newlines for C edges."""
    pattern = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), text)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--elf', type=Path, default=Path('build/browser.elf'))
    p.add_argument('--json', type=Path, default=Path('build/wiring/wiring-audit.json'))
    p.add_argument('--source-override', action='append', default=[], metavar='REPO_PATH=TEMP_FILE',
                   help='test-only source overlay; the actual tree/ELF remain untouched')
    args = p.parse_args(argv)
    root = args.root.resolve()
    overrides = dict(pair.split('=', 1) for pair in args.source_override)
    cache = {}
    def read(path):
        path = os.path.normpath(path)
        if path not in cache:
            cache[path] = Path(overrides[path]).read_text() if path in overrides else (root/path).read_text()
        return cache[path]
    def locs(path, pattern, c_code=True):
        path = os.path.normpath(path)
        text = read(path)
        hay = scrub(text) if c_code else text
        return [{'path': path, 'line': text.count('\n', 0, m.start())+1}
                for m in re.finditer(pattern, hay)]

    errors = []
    # Join FIRST. Looking at raw assignment lines loses continuation-owned TUs.
    joined = re.sub(r'\\\r?\n[ \t]*', ' ', read('Makefile'))
    for name in VARIABLES:
        if not re.search(r'^'+name+r'\s*[:+?]?=', joined, re.M):
            errors.append('MISSING_MAKE_VARIABLE '+name)
    with tempfile.TemporaryDirectory(prefix='logitos-wiring-vars-') as tmp:
        fragment = Path(tmp)/'vars.mk'
        fragment.write_text('.PHONY: wiring-audit-vars\nwiring-audit-vars:\n'+''.join(
            "\t@printf '%s\\t%s\\n' '"+name+"' '$("+name+")'\n" for name in VARIABLES))
        out = subprocess.run(['make', '-s', '--no-print-directory', '-f', 'Makefile', '-f', str(fragment),
                              'wiring-audit-vars'], cwd=root, text=True, capture_output=True, check=True)
    expanded = {}
    for line in out.stdout.splitlines():
        if '\t' in line:
            name, value = line.split('\t', 1)
            if name in VARIABLES: expanded[name] = value.split()
    for name in VARIABLES:
        if not expanded.get(name): errors.append('EMPTY_MAKE_VARIABLE '+name)
    for sources, objects, prefix in [('BROWSER_PIPE', 'BROWSER_OBJ', 'browserobj'), ('BROWSER_JS_SRC', 'BROWSER_JS_OBJ', 'jsobj')]:
        for source in expanded.get(sources, []):
            suffix = '/'+prefix+'/'+source[:-2]+'.o'
            if not any(o.endswith(suffix) for o in expanded.get(objects, [])):
                errors.append('MISSING_OBJECT '+source)

    # .inc ownership is recursive and associated with linked translation units,
    # not a fake requirement that every implementation owns a standalone .c TU.
    source_files = set(expanded.get('BROWSER_PIPE', []) + expanded.get('BROWSER_JS_SRC', []))
    source_files.add(B+'css_engine.c')
    includes = {}
    def visit(path, owner, seen):
        if path in seen: return
        seen.add(path)
        text = read(path)
        for m in re.finditer(r'^\s*#\s*include\s*"([^"\n]+\.inc)"', text, re.M):
            child = os.path.normpath(str(Path(path).parent/m[1]))
            includes.setdefault(child, []).append({'translation_unit': owner,
                'included_from': path, 'line': text.count('\n', 0, m.start())+1})
            visit(child, owner, seen)
    for path in sorted(source_files): visit(path, path, set())

    elf = args.elf if args.elf.is_absolute() else root/args.elf
    symbols = {}
    nm_tool = shutil.which('llvm-nm') or shutil.which('nm')
    if not elf.exists() or not nm_tool:
        errors.append('ELF_UNAVAILABLE '+str(elf))
    else:
        proc = subprocess.run([nm_tool, '-g', str(elf)], text=True, capture_output=True)
        if proc.returncode: errors.append('NM_FAILED '+proc.stderr.strip())
        for line in proc.stdout.splitlines():
            m = re.search(r'\b([A-Za-z?])\s+(\S+)$', line)
            if m: symbols[m[2]] = m[1]
    features = []
    for name, needed, edges in FEATURES:
        rows = []
        for short, pattern in edges:
            path = os.path.normpath(B+short)
            matches = locs(path, pattern)
            rows.append({'path': path, 'pattern': pattern, 'locations': matches,
                         'evidence': 'source_edge_present' if matches else 'missing_source_edge'})
            if not matches: errors.append('MISSING_CALL '+name+' '+path+' '+pattern)
        providers = {sym: symbols.get(sym) for sym in needed}
        for sym, kind in providers.items():
            if kind not in ('T', 'D', 'B', 'R'):
                errors.append('MISSING_STRONG_SYMBOL '+name+' '+sym)
        features.append({'feature': name, 'linked_symbols': providers, 'source_edges': rows,
                         'runtime_verified_by_this_audit': False})
    orphans = [{'feature': name, 'locations': locs(B+path, pattern, False),
                'finding': reason, 'status': 'reviewed_unwired_or_incomplete'}
               for name, path, pattern, reason in ORPHANS]
    newer = sorted(path for path in source_files | set(includes)
                   if elf.exists() and (root/path).stat().st_mtime_ns > elf.stat().st_mtime_ns)
    installer_inventory = []
    page_code = scrub(read(B+'js_page.c'))
    for m in re.finditer(r'\b(js_\w+_install)\s*\(g_ctx\b', page_code):
        symbol = m[1]
        installer_inventory.append({'symbol': symbol, 'linked_symbol_type': symbols.get(symbol),
            'path': B+'js_page.c', 'line': page_code.count('\n', 0, m.start())+1,
            'evidence': 'installer call written; runtime execution unmeasured'})
    report = {'schema_version': 1, 'scope': 'Reviewed browser expansion seams; lexical source edges plus actual ELF symbols, not execution coverage.',
              'make_continuations_joined_before_inspection': True,
              'make_source_lists': expanded, 'include_owners': includes,
              'elf': {'path': str(elf), 'sha256': hashlib.sha256(elf.read_bytes()).hexdigest() if elf.exists() else None,
                      'nm': nm_tool, 'sources_newer_than_elf': newer,
                      'freshness_note': 'Timestamps are advisory only; a symbol is not proof that this source revision ran.'},
              'source_overrides': overrides, 'source_sha256': {path: hashlib.sha256(text.encode()).hexdigest() for path, text in sorted(cache.items())},
              'installer_inventory': installer_inventory, 'features': features, 'known_orphans': orphans,
              'semantic_gaps': ['Ordinary fixed/sticky positioning remains incomplete; the separately wired modal viewport path does not establish it.',
                                'Pseudo getComputedStyle targeting and complete media-query syntax/listener semantics are outside this audit.'],
              'runtime_evidence': 'Run feature host gates and ordinary guest fixtures separately. This tool never marks a feature rendered.',
              'errors': errors}
    dest = args.json if args.json.is_absolute() else root/args.json
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(report, indent=2, ensure_ascii=False)+'\n')
    for error in errors: print(error)
    print('browser-wiring-audit: '+('FAIL' if errors else 'PASS')+
          f' {len(features)} reviewed features; {len(includes)} include owners; {len(orphans)} known gaps; JSON {dest}')
    if newer: print(f'ELF_FRESHNESS_NOTE {len(newer)} sources newer than artifact; rebuild/guest evidence remains separate')
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
