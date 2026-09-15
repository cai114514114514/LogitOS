#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate build and runtime views from the owned application catalogue."""
import argparse,json,pathlib
ROOT=pathlib.Path(__file__).resolve().parents[1]
CATALOG=ROOT/'c/apps/agent/catalog.json'
def entries():
    rows=json.loads(CATALOG.read_text());names=set();ids=set()
    for a in rows:
        if a['name'] in names or a['id'] in ids or 'browser' in a['source'] or a['source'].startswith('third_party/'):
            raise ValueError('duplicate or protected application')
        names.add(a['name']);ids.add(a['id'])
        if not (ROOT/a['source']).is_file(): raise ValueError(a['source'])
    return rows
def manifest(a):
    return dict(abi=1,capability_version=1,state_version=1,objects=7,
                actions=7 if a['name']=='textedit' else 1 if a['name']=='files' else 8,
                contexts=1 if a['name']=='files' else 2 if a['name']=='textedit' else 4,
                document_max=1048576 if a['name']=='textedit' else 0,reserved=0)
def main():
    p=argparse.ArgumentParser();p.add_argument('--names',action='store_true');p.add_argument('--header');a=p.parse_args();rows=entries()
    if a.names: print(' '.join(x['name'] for x in rows))
    if a.header:
        out=pathlib.Path(a.header);out.parent.mkdir(parents=True,exist_ok=True)
        text='/* Generated from c/apps/agent/catalog.json; do not edit. */\n'
        for x in rows:text+='AG_APP('+','.join(json.dumps(x[k]) for k in ('name','id','path','domain'))+')\n'
        if not out.exists() or out.read_text()!=text:out.write_text(text)
if __name__=='__main__':main()
