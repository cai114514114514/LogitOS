#!/usr/bin/env python3
"""Require real renderer mutations to fail for the expected reason."""
import os, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
mutations=[('studio_render.inc','case ST_INK_KEYWORD:return rgb(198,120,221);','case ST_INK_KEYWORD:return AUI_TEXT;','highlight'),
           ('studio_render.inc','if(widths[slot].n==n&&','if(0&&widths[slot].n==n&&','warm cache'),('studio_code.inc','gui_clip(left,y,right-left,ST_ROW);','gui_clip(left,edit_y,right-left,out_y-edit_y);','row clip')]
# A stub compares palette values, so missing ink must be observed against the
# expected purple, not against a second call to the same mutated colour helper.
for filename,old,new,label in mutations:
    src=ROOT/'c/apps/studio'/filename
    with tempfile.TemporaryDirectory(prefix='studio-render-') as tmp:
        p=Path(tmp);text=src.read_text();assert text.count(old)==1
        (p/'render.inc').write_text(text.replace(old,new))
        test=(ROOT/'tests/unit/studio_render_test.c').read_text()
        test=test.replace('"../../c/', '"'+str(ROOT/'c')+'/')
        test=test.replace(str(src),str(p/'render.inc'))
        test=test.replace('if(c==ink_color(i))','if(c==(i==ST_INK_KEYWORD?rgb(198,120,221):ink_color(i)))')
        (p/'test.c').write_text(test)
        sources=list((ROOT/'c/apps/studio').glob('*.c'))+[ROOT/x for x in ('c/apps/as/editor/completion.c','c/lib/agent/json.c','c/lib/agent/task.c','c/drivers/block/crc32.c')]
        subprocess.run([os.environ.get('CC','clang'),'-std=c11','-D_DEFAULT_SOURCE','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),*map(str,sources),'-o',str(p/'test')],check=True)
        result=subprocess.run([str(p/'test')],text=True,capture_output=True)
        if not result.returncode or 'FAIL '+label+' at ' not in result.stderr:
            raise RuntimeError('wrong control failure: '+result.stdout+result.stderr)
        print('PASS render negative control:',label,'observed failing')
