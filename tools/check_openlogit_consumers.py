#!/usr/bin/env python3
"""Fence drawing entry points; inventory direct and one-way compatibility use.

This is a source-route gate, not proof of display behavior. It deliberately
reports raw pixel preparations as review candidates, not 'all consumers done'.
Codec output, font parsing and image storage preparation remain their owners'
work. Guest tests supply evidence for the real paths that they exercise.
"""
import argparse,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);p.add_argument('--inject-legacy',action='store_true');a=p.parse_args()
def code(s):
    # Preserve newlines for diagnostic locations while removing comments/strings.
    return re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m:'\n'*m[0].count('\n'),s,flags=re.S)
legacy=re.compile(r'\bgfx_(?:fill(?:_mask(?:_subs|_clipped)?|_clipped|_workspace|_with_workspace|_subs)?|surface_clear|over)\s*\(')
private=re.compile(r'\bol_sw_\w+\s*\(')
oldimpl=re.compile(r'\b(?:for|while)\s*\(')
entries=re.compile(r'\b(ol_(?:window|cmd|raster|display|bitmap)_\w+|gui_(?:clear|rect|rrect|blit|text\w*|icon|glass|flush\w*)|fb_(?:put|clear|fill_\w+|round_rect\w*|blit_\w+|shadow|blend_\w+|liquid_glass\w*))\s*\(')
violations=[];inventory=[]
for path in sorted(list((ROOT/'c').rglob('*'))+list((ROOT/'examples/openlogit').rglob('*'))):
    if path.suffix not in ('.c','.h','.inc'):continue
    rel=str(path.relative_to(ROOT));src=code(path.read_text(errors='replace'))
    if a.inject_legacy and rel=='c/kernel/gui/wm.c':src+='\nvoid forbidden_draw(void){gfx_fill(0,0,0,0,0);}'
    sdk=rel.startswith(('c/lib/gfx/','c/lib/gfx3d/'))
    if not sdk:
        for m in legacy.finditer(src):violations.append(f'{rel}:{src[:m.start()].count(chr(10))+1}: legacy raster bypass {m[0]}')
        for m in private.finditer(src):violations.append(f'{rel}: private backend used outside SDK')
    calls=sorted(set(entries.findall(src)))
    if calls:
        route='sdk-backend' if sdk else 'normal-consumer'
        if rel in ('c/kernel/gui/fb/fb/fb.c','c/apps/logit.h','c/kernel/gui/fb/fb/glass.h'):route='compatibility-or-driver'
        inventory.append({'path':rel,'role':route,'entry_points':calls})
# A framebuffer adapter can transfer final display pixels but its normal draw
# entries must remain loop-free one-way calls. Braces parse enough of C here;
# there are no strings/comments left to confuse nesting.
fb=code((ROOT/'c/kernel/gui/fb/fb/fb.c').read_text())
for name in ('put','clear','fill_rect','fill_circle','round_rect','blit_glyph','blit_rgba',
             'blit_surface','blit_surface_scaled','blit_surface_scaled_bl','shadow','blur_rect',
             'blend_rect','blend_round_rect','fill_vgrad','round_rect_vgrad','liquid_glass','liquid_glass_cut'):
    match=re.search(r'\bfb_'+name+r'\s*\([^;]*?\)\s*\{',fb)
    if not match:violations.append('missing compatibility adapter fb_'+name);continue
    start=match.end();end=start;depth=1
    while depth and end<len(fb):
        depth+=(fb[end]=='{')-(fb[end]=='}');end+=1
    body=fb[start:end-1]
    if 'ol_display_' not in body or oldimpl.search(body):violations.append('drawing implementation returned to fb_'+name)
wm=code((ROOT/'c/kernel/gui/wm.c').read_text())
if re.search(r'\b(?:drow|back|cursor_plane)\s*\[[^;]*?\]\s*=(?!=)',wm):
    violations.append('WM writes drawing pixels directly instead of SDK')
report={'passed':not violations,'scope':'entry-point fence; not whole-system runtime acceptance',
        'exceptions':['early boot and panic emergency framebuffer output','display driver final pixel transfer'],
        'inventory':inventory,'violations':violations}
if a.output:a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n')
for item in violations:print('FAIL '+item)
print(f'OpenLogit consumer routes: {len(inventory)} files, {len(violations)} violations')
raise SystemExit(bool(violations))
