#!/usr/bin/env python3
"""Source-byte and display-column contract for the original compiler CLI."""
import json
from pathlib import Path
import subprocess
import sys
compiler=Path(sys.argv[1]).resolve()
cases=[('print(42)\n',0,[]),('x =\ny =\n',1,[(3,1,4),(7,2,4)]),
       ('print("中文"); x = )\ny =\n',1,[(21,1,18),(26,2,4)]),('x=1\0y=2',1,[(3,1,4)]),
       ('import nonexistent_module\nprint("MUST NOT EXECUTE")\n',0,[])]
for source,code,spans in cases:
    r=subprocess.run([str(compiler),'check','--json','--stdin','中文.as'],input=source.encode(),capture_output=True,timeout=10)
    result=json.loads(r.stdout)
    assert r.returncode==code and not r.stderr,(r.returncode,r.stderr,result)
    assert result['source_bytes']==len(source.encode()) and result['file']=='中文.as',result
    assert [(d['start'],d['line'],d['column']) for d in result['diagnostics']]==spans,result
print('PASS 5 compiler check cases; user code and imports are not executed')
