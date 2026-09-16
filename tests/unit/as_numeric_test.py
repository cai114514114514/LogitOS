#!/usr/bin/env python3
"""Language-2 integer contract through both compilers and bytecode loading.

The original compiler is exercised directly and through the self-host compiler;
Python integers are an independent arithmetic oracle. No timing implies native
performance: these programs still execute on the VM.
"""
import argparse
import json
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
from as_migration_test import copy_libraries

ROOT=Path(__file__).resolve().parents[2]
MIN=-(1<<63);MAX=(1<<63)-1

def gate(compiler,work):
    compiler=Path(compiler).resolve();work=Path(work);count=0
    def require(name,condition,details=''):
        nonlocal count
        if not condition:raise AssertionError(name+': '+details)
        count+=1
    def run(args,source=None):
        return subprocess.run([str(compiler),*map(str,args)],cwd=work,input=source,text=True,capture_output=True,timeout=30)
    # A3 aslex no longer runs in the VM. Use the same frozen migration
    # dependencies as the bytecode parity gate while checking A2 arithmetic.
    copy_libraries(work)
    (work/'driver.as').write_text('from asc import compile_file\na = args()\ncompile_file(a[1], a[2])\n')
    def compile_pair(source,name):
        src=work/(name+'.as');src.write_text(source);c=work/(name+'.c.la');sh=work/(name+'.sh.la')
        r=run(['-c',src,'-o',c]);require(name+' C compile',r.returncode==0,r.stdout+r.stderr)
        r=run([work/'driver.as',src,sh]);require(name+' self-host compile',r.returncode==0,r.stdout+r.stderr)
        require(name+' compiler byte parity',c.read_bytes()==sh.read_bytes())
        return src,c,sh
    lines=['# aether: 2'];expected=[]
    def value(expression,result):lines.append('print('+expression+')');expected.append(str(result))
    value(str(MIN),MIN);value('-0x8000000000000000',MIN)
    value('-'+'0'*80+'9223372036854775808',MIN)
    for a,b,op,f in [(MAX,0,'+',lambda a,b:a+b),(MIN,1,'*',lambda a,b:a*b),(-2,63,'**',pow),(-1,63,'<<',lambda a,b:a<<b)]:
        value(f'({a}) {op} ({b})',f(a,b))
    value('(-2) ** 63',MIN);value('0 ** 0',1)
    value('wrapping_add(9223372036854775807, 1)',MIN)
    value(f'wrapping_sub({MIN}, 1)',MAX)
    value(f'wrapping_mul({MAX}, 2)',-2)
    value('wrapping_shl(1, 63)',MIN)
    value('parse_int("-0x8000000000000000")',MIN)
    errors=[f'{MAX}+1',f'({MIN})-1',f'{MAX}*2','2 ** 63','1 << 63',f'-({MIN})',f'({MIN}) / -1',f'({MIN}) % -1',
            '1 / 0','1 << 64','wrapping_add(1.0, 2)','wrapping_shl(1, -1)','parse_int("9223372036854775808")','parse_int("1x")']
    rng=random.Random(806)
    for _ in range(40):
        a=rng.choice([MIN,MAX,rng.randrange(MIN,MAX)]);b=rng.choice([-2,-1,0,1,2]);op=rng.choice(['+','-','*'])
        result={'+':lambda:a+b,'-':lambda:a-b,'*':lambda:a*b}[op]()
        expression=f'({a}) {op} ({b})'
        if MIN<=result<=MAX:value(expression,result)
        else:errors.append(expression)
    for i,expression in enumerate(errors):
        lines+=['try:','    v = '+expression,'    print("MISSED")','except e:',f'    print("caught-{i}")'];expected.append('caught-'+str(i))
    lines+=['def nested():','    return 9223372036854775807 + 1','try:','    nested()','    print("MISSED")','except e:','    print("nested-caught")'];expected.append('nested-caught')
    source='\n'.join(lines)+'\n';src,c,sh=compile_pair(source,'checked')
    require('checked-header',c.read_bytes()[:12]==b'LAQ1\x06\0\0\0\x02\0\0\0')
    output='\n'.join(expected)+'\n'
    for args,label in [([src],'source'),(['-run',c],'bytecode-policy'),(['-run',sh],'selfhost-bytecode-policy')]:
        r=run(args);require(label,r.returncode==0 and r.stdout==output,r.stdout+r.stderr)
    # A module retains its own language policy regardless of caller language.
    _,lib,_=compile_pair('# aether: 2\ndef f():\n    return 9223372036854775807 + 1\n','checked_lib')
    shutil.copyfile(lib,work/'checked_lib.la');(work/'checked_lib.as').unlink()
    r=run([], 'import checked_lib\ntry:\n    checked_lib.f()\n    print("MISSED")\nexcept e:\n    print("module-caught")\n')
    require('module-policy',r.returncode==0 and r.stdout=='module-caught\n',r.stdout)
    _,c,_=compile_pair('print(9223372036854775807 + 1)\n','unversioned')
    require('unversioned-is-A2',c.read_bytes()[:12]==b'LAQ1\x06\0\0\0\x02\0\0\0')
    r=run(['-run',c]);require('unversioned-overflow-traps',r.returncode!=0 and 'integer overflow' in r.stdout,r.stdout)
    retired = work / 'retired.as'
    retired.write_text('# aether: 1\nprint(1)\n')
    for command in ([retired], ['-c', retired, '-o', work/'retired.la'],
                    [work/'driver.as', retired, work/'retired.la']):
        r = run(command)
        require('A1-source-rejected', r.returncode != 0 and 'removed' in r.stdout, r.stdout+r.stderr)
    r = run(['check', '--json', retired])
    report = json.loads(r.stdout)
    require('A1-check-diagnostic', r.returncode == 1 and 'removed' in report['diagnostics'][0]['message'])
    # Construct a real v5 layout from v6 by removing its language-policy word.
    # The removed A1 format must fail loading, even for code with no overflow.
    _,simple,_ = compile_pair('print(1)\n', 'simple')
    blob = simple.read_bytes()
    old = work/'retired-v5.la'
    old.write_bytes(blob[:4] + (5).to_bytes(4, 'little') + blob[12:])
    for mode in ('-run', '-dis'):
        r = run([mode, old])
        require('A1-bytecode-rejected', r.returncode != 0 and
                'removed' in r.stdout and 'recompile' in r.stdout, r.stdout+r.stderr)
    native = run(['check', '--json', '--stdin', 'native.as'], '# aether: 3.0\ndef main() -> None:\n    pass\n')
    require('3.0-selects-native', native.returncode == 0 and json.loads(native.stdout)['language'] == 3)
    future = run(['check', '--json', '--stdin', 'future.as'], '# aether: 3.1\ndef main() -> None:\n    pass\n')
    require('future-minor-rejected', future.returncode == 1)
    for n,bad in enumerate(['9223372036854775808','0x8000000000000000','18446744073709551616','-9223372036854775809','-9223372036854775808 ** 0']):
        path=work/f'bad{n}.as';path.write_text('# aether: 2\nx = '+bad+'\n')
        r=run(['check','--json',path]);d=json.loads(r.stdout)
        require('literal-range',r.returncode==1 and d['diagnostics'][0]['line']==2,r.stdout)
        r=run([work/'driver.as',path,work/'bad.la']);require('selfhost-literal-range',r.returncode!=0 and 'integer literal too large' in r.stdout,r.stdout)
    r=run(['check','--json','--stdin','bad-version.as'],'# aether: 20\nx=1\n');d=json.loads(r.stdout)
    require('unsupported-language',r.returncode==1 and d['diagnostics'][0]['line']==1)
    # An unrecognized wire policy must not be relabeled as A2.
    broken=bytearray((work/'checked.c.la').read_bytes());broken[8]=3;(work/'bad-policy.la').write_bytes(broken)
    r=run(['-run',work/'bad-policy.la']);require('unknown-bytecode-policy',r.returncode!=0)
    print(f'PASS {count} integer contract checks, {len(expected)} program outputs per compiler path')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('compiler',type=Path);a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='as-numeric-') as tmp:gate(a.compiler,tmp)
