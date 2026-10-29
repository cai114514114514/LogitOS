#!/usr/bin/env python3
"""Source-byte and display-column contract for the original compiler CLI."""
import json
from pathlib import Path
import subprocess
import sys
compiler=Path(sys.argv[1]).resolve()
# These five cases were A2 source until 2026-10-26, and from the A3 cutover
# (2026-09-16) the compiler refused every one with AS3302 before reaching what
# it tested. Each is now the original with the A3 header, so offsets move by
# its 14 bytes and lines by one ((3,1,4) -> (17,2,4)). Two cases changed shape:
#  - the CJK line dropped `;` (not A3 syntax) but keeps CJK before the error,
#    so column 12 (code points) differs from bytes (16) and display cells (14);
#  - the import case names a real module: A3 resolves imports while checking,
#    so an unknown one is AS3300 rather than silence. Nothing may run either
#    way -- stdout must still parse as the JSON alone.
# The second case is also the end-of-line recovery contract: before the fix in
# c/apps/as/frontend/expression.c, A3 reported (17,2,4) and then a false
# "expected end of statement" at (18,3,1), and y's own error never appeared.
H='# aether: 3.0\n'
cases=[(H+'print(42)\n',0,[]),(H+'x =\ny =\n',1,[(17,2,4),(21,3,4)]),
       (H+'x = "中文" + )\ny =\n',1,[(29,2,12),(34,3,4)]),(H+'x=1\0y=2',1,[(17,2,4)]),
       (H+'import math\nprint("MUST NOT EXECUTE")\n',0,[])]
for source,code,spans in cases:
    r=subprocess.run([str(compiler),'check','--json','--stdin','中文.as'],input=source.encode(),capture_output=True,timeout=10)
    result=json.loads(r.stdout)
    assert r.returncode==code and not r.stderr,(r.returncode,r.stderr,result)
    assert result['source_bytes']==len(source.encode()) and result['file']=='中文.as',result
    assert [(d['start'],d['line'],d['column']) for d in result['diagnostics']]==spans,result
print('PASS 5 compiler check cases; user code and imports are not executed')
