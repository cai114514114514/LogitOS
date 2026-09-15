#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise ordinary CLI behavior from frozen AEX artifacts in a private guest.

Serial output can contain kernel diagnostics inside a word. Completion markers
only order commands; assertions read the actual saved files after QEMU stops.
No model task is submitted, and the fixture key is intentionally not a secret.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mode', choices=('bios', 'uefi'), default='bios')
    parser.add_argument('--ram', default='512M')
    args = parser.parse_args()
    source, out = args.build.resolve(), args.out.resolve()
    # Refuse reuse: stale output files must never satisfy a later command.
    out.mkdir(parents=True, exist_ok=False)
    build, fixture = out/'build', out/'fixture'
    build.mkdir(); fixture.mkdir()
    harness_files = [Path(__file__).resolve(), ROOT/'tests/boot/run-agent.py',
                     ROOT/'tools/mkfs.py', ROOT/'tools/license_audit.py']
    harness_hashes = {str(path): sha(path) for path in harness_files}
    shutil.copyfile(Path(__file__).resolve(), out/'runner-source.py')
    catalog = json.loads((ROOT/'c/apps/agent/catalog.json').read_text())
    names = {app['name'] for app in catalog}
    required = {'sh', 'echo', 'cat', 'cp', 'mv', 'rm', 'dir', 'ls', 'true', 'false', 'wc'}
    if required - names:
        raise RuntimeError('Required catalog tools absent: ' + repr(sorted(required - names)))
    absent_optional = sorted({'grep', 'sort'} - names)
    provenance = []
    for name in [app['name']+'.aex' for app in catalog] + [
            'agent-runtime-test.aex', 'kernel.elf', 'logit.iso', 'esp.img']:
        src, dest = source/name, build/name
        shutil.copyfile(src, dest)
        digest = sha(dest)
        if digest != sha(src):
            raise RuntimeError('Source changed while copying ' + name)
        provenance.append(dict(source=str(src), copy=str(dest), sha256=digest))
    (out/'provenance.json').write_text(json.dumps(dict(
        utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        source_build=str(source), artifacts=provenance,
        harness_sha256=harness_hashes,
        catalog_sha256=sha(ROOT/'c/apps/agent/catalog.json'),
        absent_optional=absent_optional), indent=2)+'\n')

    binary = bytes(range(256))*257 + b'CLI_BINARY_TAIL\0'
    (fixture/'binary.in').write_bytes(binary)
    (fixture/'count.in').write_bytes(b'alpha beta\nalpha\n')
    (fixture/'list').mkdir()
    (fixture/'list/alpha.txt').write_bytes(b'alpha\n')
    (fixture/'list/beta.txt').write_bytes(b'beta\n')
    gateway = out/'dummy-gateway'; gateway.mkdir()
    (gateway/'agent.conf').write_text(
        'host=127.0.0.1\nport=9\npath=/v1/chat/completions\nmodel=cli-fixture\n'
        'key_file=/etc/agent.key\ntls=0\nallow_insecure=1\n')
    (gateway/'agent.key').write_text('not-a-provider-credential\n')
    runtime = module('agent_runtime', ROOT/'tests/boot/run-agent.py')
    disk = runtime.prepare(build, gateway, out, catalog=True,
                           extras=[str(fixture)+':/cli'])
    reader = module('cli_filesystem_reader', ROOT/'tools/license_audit.py')
    fs = reader._LogitFS(disk)
    try:
        for app in catalog:
            inode = fs.lookup(app['path'])
            if inode is None or hashlib.sha256(fs.read_file(inode)).hexdigest() != sha(build/(app['name']+'.aex')):
                raise RuntimeError('Packed AEX differs: ' + app['name'])
        if fs.lookup('/cli/echo.out') is not None:
            raise RuntimeError('Unexpected pre-existing output')
    finally:
        fs.close()
    cases = [
        ('sh_arguments', '/bin/sh -c \'/bin/echo "$0" "two words" plain > /cli/args.out\' named-shell'),
        ('pipe_binary', '/bin/cat /cli/binary.in | /bin/cat | /bin/cat > /cli/pipe.out'),
        ('echo_bytes', '/bin/echo "alpha beta" gamma > /cli/echo.out'),
        ('cat_binary', '/bin/cat /cli/binary.in > /cli/cat.out'),
        ('cp_binary', '/bin/cp /cli/binary.in /cli/copied.bin\n/bin/cat /cli/copied.bin > /cli/cp.out'),
        ('mv_binary', '/bin/mv /cli/copied.bin /cli/moved.bin\n/bin/cat /cli/moved.bin > /cli/mv.out'),
        ('rm_file', '/bin/rm /cli/moved.bin\n/bin/echo $? > /cli/rm.status'),
        ('directory_entries', '/bin/ls /cli/list > /cli/ls.out\n/bin/dir /cli/list > /cli/dir.out'),
        ('exit_status', '/bin/true\n/bin/echo $? > /cli/true.status\n/bin/false\n/bin/echo $? > /cli/false.status\n/bin/sh -c /bin/false\n/bin/echo $? > /cli/sh-false.status'),
        ('wc_counts', '/bin/wc /cli/count.in > /cli/wc.out'),
    ]
    (out/'commands.json').write_text(json.dumps(cases, indent=2)+'\n')
    guest = None
    try:
        guest = runtime.Guest(build, disk, out, args.mode, args.ram)
        guest.wait(b'LogitOS shell', 180)
        guest.wait(b'AGENTD_READY', 90)
        for name, command in cases:
            result = guest.command(command, 90)
            (out/(name+'.command.log')).write_text(result)
            print('EXECUTED', name, flush=True)
    finally:
        if guest is not None:
            guest.close()

    fs = reader._LogitFS(disk)
    checks = []; extracted = out/'extracted'; extracted.mkdir()
    def expect(name, path, expected):
        inode = fs.lookup(path)
        if inode is None:
            raise AssertionError(name + ': missing ' + path)
        data = fs.read_file(inode)
        (extracted/Path(path).name).write_bytes(data)
        if data != expected:
            raise AssertionError(name + ': incorrect bytes at ' + path +
                                 f' (got {len(data)}, expected {len(expected)})')
        checks.append(dict(case=name, path=path, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
    try:
        expect('sh_arguments', '/cli/args.out', b'named-shell two words plain\n')
        for case, leaf in [('pipe_binary', 'pipe.out'), ('cat_binary', 'cat.out'),
                           ('cp_binary', 'cp.out'), ('mv_binary', 'mv.out')]:
            expect(case, '/cli/'+leaf, binary)
        expect('echo_bytes', '/cli/echo.out', b'alpha beta gamma\n')
        expect('rm_file', '/cli/rm.status', b'0\n')
        if fs.lookup('/cli/copied.bin') is not None or fs.lookup('/cli/moved.bin') is not None:
            raise AssertionError('move/remove left a source or destination entry')
        expect('rm_file', '/cli/binary.in', binary)
        for leaf in ('ls.out', 'dir.out'):
            inode = fs.lookup('/cli/'+leaf)
            if inode is None:
                raise AssertionError('directory_entries: missing '+leaf)
            data = fs.read_file(inode); (extracted/leaf).write_bytes(data)
            entries = [line.split()[0] for line in data.splitlines() if line.split()]
            if sorted(entries) != [b'alpha.txt', b'beta.txt']:
                raise AssertionError('directory_entries: actual list differs: '+repr(data))
            checks.append(dict(case='directory_entries', path='/cli/'+leaf, entries=['alpha.txt','beta.txt']))
        for leaf, value in [('true.status', b'0\n'), ('false.status', b'1\n'), ('sh-false.status', b'1\n')]:
            expect('exit_status', '/cli/'+leaf, value)
        expect('wc_counts', '/cli/wc.out', b'2 3 17 /cli/count.in\n')
    finally:
        fs.close()
    changes = [record['source'] for record in provenance if sha(Path(record['source'])) != record['sha256']]
    copy_changes = [record['copy'] for record in provenance if sha(Path(record['copy'])) != record['sha256']]
    harness_changes = [name for name, digest in harness_hashes.items() if sha(Path(name)) != digest]
    results = dict(boot=args.mode, ram=args.ram, functional_cases=10,
                   functional_pass=True, checks=checks, absent_optional=absent_optional,
                   source_changes=changes, copy_changes=copy_changes, harness_changes=harness_changes,
                   completed=datetime.datetime.now(datetime.timezone.utc).isoformat())
    (out/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    if changes or copy_changes or harness_changes:
        raise RuntimeError('Snapshot provenance changed; retain functional result but fail full gate')
    print('AGENT_CLI_PASS cases=10 packed_catalog='+str(len(catalog))+
          ' absent_optional='+','.join(absent_optional), flush=True)


if __name__ == '__main__':
    main()
