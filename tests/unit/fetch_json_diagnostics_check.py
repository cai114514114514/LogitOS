"""Validate fixed metadata, complete-HTTP timing, and absence of private text."""
import pathlib
import re
import sys

off,on=[pathlib.Path(p).read_text() for p in sys.argv[1:3]]
for log in (off,on):
    assert 'fetch-json-diagnostics: 145 checks, 0 failures' in log
    assert not any(word in log for word in ('PRIVATE_', '[js exception]', 'FAIL:'))
assert '[runtime-diag]' not in off, 'flag-off build emitted diagnostics'
parts=re.findall(r'fetch-json-case=([^\n]+)\n(.*?)fetch-json-case-end',on,re.S)
assert len(parts)==16
pattern=re.compile(r'\[runtime-diag\] fetch-json fetch=(\d+) status=(4\d\d) detail_kind=(missing|null|array|object|string|number|boolean|other|invalid_json) detail_count=(\d+) type=(missing|string_type|string_pattern_mismatch|json_invalid|value_error|other) loc=(body|query|path|header|other) count=(\d+)')
rows={}
for label,block in parts:
    before,after=block.split('fetch-json-release=complete\n',1)
    assert '[runtime-diag]' not in before, f'{label}: classified an incomplete body'
    records=[line for line in after.splitlines() if line.startswith('[runtime-diag]')]
    parsed=[]
    for record in records:
        m=pattern.fullmatch(record)
        assert m, f'{label}: diagnostic schema contains unexpected fields'
        _,status,kind,total,typ,loc,count=m.groups()
        parsed.append((int(status),kind,int(total),typ,loc,int(count)))
    rows[label]=parsed
assert any(rows.values()), 'missing Response.json validation diagnostics'
expected_mixed={
    (422,'array',8,'missing','body',2),
    (422,'array',8,'string_type','query',1),
    (422,'array',8,'string_pattern_mismatch','path',1),
    (422,'array',8,'json_invalid','body',1),
    (422,'array',8,'value_error','header',1),
    (422,'array',8,'other','other',2),
}
assert len(rows['mixed'])==6 and set(rows['mixed'])==expected_mixed
expected={
    'missing_poison':(422,'missing',0,'other','other',0),
    'object':(400,'object',1,'value_error','body',1),
    'string':(401,'string',1,'other','other',1),
    'null':(403,'null',0,'other','other',0),
    'number':(404,'number',1,'other','other',1),
    'boolean':(409,'boolean',1,'other','other',1),
    'empty_array':(422,'array',0,'other','other',0),
    'invalid_json':(422,'invalid_json',0,'json_invalid','other',1),
    'plain_mime':(422,'array',0,'other','other',0),
    'clone':(422,'array',1,'string_type','query',1),
    'exact_cap':(422,'array',0,'other','other',0),
}
for label,row in expected.items():assert rows[label]==[row],label
for label in ('success','server_error','over_cap','utf8_byte_cap'):assert rows[label]==[],label
print('fetch-json diagnostics: 16 actual HTTP cases; fixed categories/counts, stream timing, clone and private sentinels verified')
