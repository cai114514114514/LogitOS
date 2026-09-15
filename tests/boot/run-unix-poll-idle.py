#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure a quiet broker window from real guest CPU/poll counters.

Only three status requests run in the guest; no model request, synthetic
wake, or repeated status sampling occurs between them. Host time bounds that
deliberate quiet interval. CPU percentage uses guest process CPU / guest elapsed
time, never host runtime, because concurrent TCG guests distort host timing.
"""
import argparse
import datetime
import hashlib
import importlib.util
import json
import shutil
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def counters(data):
    result = {}
    for prefix in ('SERVICE', 'SERVICE_COUNTERS'):
        rows = [row for row in data.decode().splitlines() if row.startswith(prefix+' ')]
        if len(rows) != 1:
            raise AssertionError('Missing or duplicate '+prefix+' counter row')
        result.update({key: int(value) for key, value in
                       (field.split('=', 1) for field in rows[0].split()[1:])})
    for name in ('elapsed_ms', 'idle_wakes', 'cpu_ns', 'poll_returns', 'poll_errors'):
        if name not in result:
            raise AssertionError('Missing guest counter '+name)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mode', choices=('bios', 'uefi'), default='bios')
    parser.add_argument('--ram', default='512M')
    args = parser.parse_args()
    source, out = args.build.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    build = out/'build'; build.mkdir()
    inputs = [Path(__file__).resolve(), ROOT/'tests/boot/run-agent.py',
              ROOT/'tools/mkfs.py', ROOT/'tools/license_audit.py',
              ROOT/'c/apps/agent/catalog.json']
    hashes = {str(path): sha(path) for path in inputs}
    shutil.copyfile(Path(__file__).resolve(), out/'runner-source.py')
    catalog = json.loads(inputs[-1].read_text())
    provenance = []
    for name in [app['name']+'.aex' for app in catalog]+[
            'agent-runtime-test.aex', 'kernel.elf', 'logit.iso', 'esp.img']:
        src, dst = source/name, build/name
        shutil.copyfile(src, dst)
        digest = sha(dst)
        if sha(src) != digest:
            raise RuntimeError('Source changed while copying '+name)
        provenance.append(dict(source=str(src), copy=str(dst), sha256=digest))
    (out/'provenance.json').write_text(json.dumps(dict(artifacts=provenance,
        harness_sha256=hashes, source_build=str(source)), indent=2)+'\n')
    gateway = out/'dummy-gateway'; gateway.mkdir()
    (gateway/'agent.conf').write_text('host=127.0.0.1\nport=9\n'
        'path=/v1/chat/completions\nmodel=idle-fixture\n'
        'key_file=/etc/agent.key\ntls=0\nallow_insecure=1\n')
    (gateway/'agent.key').write_text('not-a-provider-credential\n')
    runtime = module('idle_runtime', ROOT/'tests/boot/run-agent.py')
    disk = runtime.prepare(build, gateway, out, catalog=True)
    commands = ['/bin/agentctl status > /idle-control.txt',
                '/bin/agentctl status > /idle-before.txt',
                '/bin/agentctl status > /idle-after.txt']
    (out/'commands.json').write_text(json.dumps(
        [commands[0], {'control_seconds': 0}, commands[1],
         {'quiet_seconds': 5}, commands[2]], indent=2)+'\n')
    guest = None
    try:
        guest = runtime.Guest(build, disk, out, args.mode, args.ram)
        guest.wait(b'LogitOS shell', 180); guest.wait(b'AGENTD_READY', 90)
        (out/'control.command.log').write_text(guest.command(commands[0]))
        control_end = time.monotonic_ns()
        before_start = time.monotonic_ns()
        (out/'before.command.log').write_text(guest.command(commands[1]))
        start_utc = datetime.datetime.now(datetime.timezone.utc).isoformat()
        start = time.monotonic_ns()
        time.sleep(5)
        end = time.monotonic_ns()
        end_utc = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (out/'after.command.log').write_text(guest.command(commands[2]))
    finally:
        if guest is not None:
            guest.close()
    reader = module('idle_filesystem', ROOT/'tools/license_audit.py')
    fs = reader._LogitFS(disk)
    values = []
    try:
        for name in ('control', 'before', 'after'):
            inode = fs.lookup('/idle-'+name+'.txt')
            if inode is None:
                raise AssertionError('Missing saved '+name+' status')
            data = fs.read_file(inode)
            (out/('idle-'+name+'.txt')).write_bytes(data)
            values.append(counters(data))
    finally:
        fs.close()
    control, before, after = values
    control_delta = {key: before[key]-control[key] for key in control}
    delta = {key: after[key]-before[key] for key in before}
    errors = []
    if end-start < 5_000_000_000 or delta['elapsed_ms'] < 5000:
        errors.append('five-second quiet interval')
    if delta['idle_wakes'] < 4:
        errors.append('at least four actual one-second poll timeouts')
    # AG_STATUS currently sends all eight task slots even when none is used.
    # Its 75 KiB reply needs several POLLOUT iterations through the 4 KiB Unix
    # channel. A zero-delay status pair measures that protocol cost, which must
    # not be mistaken for idle spinning. Keep both raw samples in the report.
    quiet_returns = delta['poll_returns']-control_delta['poll_returns']
    if control_delta['idle_wakes'] > 1:
        errors.append('control was not a near-zero-delay status pair')
    if not 0 <= quiet_returns <= 20:
        errors.append('bounded idle poll returns after measured status cost')
    if control['poll_errors'] or before['poll_errors'] or after['poll_errors']:
        errors.append('zero poll errors')
    fraction = delta['cpu_ns']/(max(1, delta['elapsed_ms'])*1_000_000)
    if not 0 <= fraction <= .05:
        errors.append('broker CPU at most five percent of one guest CPU')
    changed = [str(path) for name, digest in hashes.items()
               if sha(path := Path(name)) != digest]
    changed += [row[key] for row in provenance for key in ('source', 'copy')
                if sha(Path(row[key])) != row['sha256']]
    if changed:
        errors.append('frozen inputs unchanged')
    result = dict(passed=not errors, errors=errors, boot=args.mode, ram=args.ram,
        control=control, before=before, after=after, delta=delta,
        control_delta=control_delta, control_host_gap_ns=before_start-control_end,
        idle_poll_returns_after_control=quiet_returns, guest_cpu_fraction=fraction,
        quiet_host_start_utc=start_utc, quiet_host_end_utc=end_utc,
        quiet_host_ns=end-start, changed_inputs=changed,
        interpretation='idle_wakes counts timed-out broker poll calls; cpu_ns is '
        'SYS_RUSAGE self CPU accounting. Neither is total system CPU utilization.')
    (out/'results.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2), flush=True)
    if errors:
        raise SystemExit('FAIL: '+', '.join(errors))


if __name__ == '__main__':
    main()
