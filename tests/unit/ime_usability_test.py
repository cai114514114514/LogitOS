#!/usr/bin/env python3
"""Positive and mutation controls for the actual IME engine and production data."""
import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path
sys.dont_write_bytecode = True
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root',type=Path,required=True)
p.add_argument('--engine',type=Path)
p.add_argument('--build',type=Path,required=True)
a=p.parse_args(); e=a.engine or a.root/'c/lib/ime'; b=a.build; b.mkdir(parents=True,exist_ok=True)
def run(cmd,**kw):
    return subprocess.run([str(x) for x in cmd],check=True,**kw)
run([sys.executable,e/'build_dictionary.py','--root',a.root,'--tsv',e/'qwen35-zh.tsv.gz','--output',b/'pinyin-qwen.dat'])
shipped=a.root/'fsroot/ime/pinyin-qwen.dat'
if shipped.exists() and shipped.read_bytes() != (b/'pinyin-qwen.dat').read_bytes():
    sys.exit('FAIL: shipped Qwen dictionary differs from deterministic rebuild')
flags=['cc','-O2','-g','-Wall','-Wextra','-Werror','-DIME_STATS','-fsanitize=address,undefined','-I'+str(e),'-x','c',a.root/'tests/unit/ime_usability_test.c']
run(flags+[e/'pinyin.c','-o',b/'engine_test'])
for dat in [a.root/'fsroot/ime/pinyin.dat',b/'pinyin-qwen.dat']:
    run([b/'engine_test',dat])
source=(e/'pinyin.c').read_text()
needle='int used = gi < 0 ? st->raw_len : st->cand[gi].raw_used;'
if source.count(needle)!=1:sys.exit('FAIL: partial-consumption mutation site drifted')
mut=b/'consume_all.c';mut.write_text(source.replace(needle,'int used = st->raw_len;'))
for name,src,defines,witness in [
 ('consume_all',mut,[],'choosing 你好 leaves m editable'),
 ('no_words',e/'pinyin.c',['-DIME_NO_WORDS'],'woaizhongguo')]:
    run(flags+defines+[src,'-o',b/name])
    proc=subprocess.run([str(b/name),str(a.root/'fsroot/ime/pinyin.dat')],capture_output=True,text=True)
    (b/(name+'.log')).write_text(proc.stdout+proc.stderr)
    if proc.returncode!=1 or 'FAIL: '+witness not in proc.stdout:
        sys.exit('FAIL: negative control did not detect '+name+'\n'+proc.stdout+proc.stderr)
    print('CONTROL DETECTED:',name)
# Strict BPE byte reconstruction and special-token filtering, including a
# deliberately incomplete UTF-8 token. No transformers/model download in CI.
spec=importlib.util.spec_from_file_location('builder',e/'build_dictionary.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
forward={v:k for k,v in m.decoder_map().items()}
def enc(raw):return ''.join(forward[v] for v in raw)
good=enc('输入法'.encode()); special=enc('特殊'.encode())
d={'model':{'type':'BPE','vocab':{good:0,special:1,enc(b'\xe4\xbd'):2,'abc':3}},
   'decoder':{'type':'ByteLevel'},'added_tokens':[{'content':special}]}
assert m.han_tokens(d)=={'输入法'}
print('PASS: strict BPE decoding, engine regressions and both negative controls')
