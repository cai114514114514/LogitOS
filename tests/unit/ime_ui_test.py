#!/usr/bin/env python3
"""Link real IME UI + engine + learning with host-only VFS/drawing stubs."""
import argparse, subprocess, sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,required=True);p.add_argument('--build',type=Path,required=True);p.add_argument('--ui',type=Path);p.add_argument('--engine',type=Path);a=p.parse_args()
r=a.root.resolve();b=a.build.resolve();e=a.engine or r/'c/lib/ime';ui=a.ui or r/'c/kernel/gui';b.mkdir(parents=True,exist_ok=True)
incs=[ui,e]+[r/x for x in ['c/kernel/gui','c/kernel/mm','c/kernel/core','c/fs','c/lib/text','include','include/abi']]
flags=['cc','-O2','-g','-Wall','-Wextra','-Werror','-DIME_LEARN_HOST','-fsanitize=address,undefined']+['-I'+str(i) for i in incs]+['-x','c',str(r/'tests/unit/ime_ui_test.c'),str(e/'pinyin.c'),str(r/'c/kernel/gui/ime_learn.c')]
def compile(src,out):subprocess.run(flags+[str(src),'-o',str(out)],check=True,cwd=r)
compile(ui/'ime_ui.c',b/'ui_test')
for mode in ['1','0','2']:subprocess.run([str(b/'ui_test'),str(b/'pinyin-qwen.dat'),mode],check=True,cwd=r)
s=(ui/'ime_ui.c').read_text();start=s.index('static int finish_punctuation(');end=s.index('\n/* SPLIT IN TWO',start)
s=s[:start]+'''static int finish_punctuation(int c, uint32_t *out, int max)
{ (void)c; (void)out; (void)max; drop(g_owner); return -1; }

'''+s[end:]
mut=b/'drop_punctuation.c';mut.write_text(s);compile(mut,b/'drop_punctuation')
result=subprocess.run([str(b/'drop_punctuation'),str(b/'pinyin-qwen.dat'),'1'],capture_output=True,text=True,cwd=r)
(b/'drop_punctuation.log').write_text(result.stdout+result.stderr)
if result.returncode!=1 or 'FAIL: punctuation commits preedit instead of dropping it' not in result.stdout:sys.exit('FAIL: punctuation-loss control was not detected')
print('CONTROL DETECTED: punctuation-loss in real UI')
