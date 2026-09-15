#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Ordinary unconfigured boot, unsaved TextEdit context and later model setup.

The negative control rebuilds the real broker with its former fatal chmod
branch. The positive uses actual GUI input and guest filesystem writes. The
default model is deterministic; --gateway uses an already running real gateway.
Neither mode places a provider credential in a guest image.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


faults = module('startup_faults', ROOT/'tests/boot/run-agent-faults.py')
runtime = faults.runtime


def mutant(source, out):
    build = out/'build'
    build.mkdir()
    # Default evidence is below BUILD. Copying BUILD recursively into it would
    # copy the destination into itself (and unrelated multi-gigabyte disks).
    shutil.copytree(source/'agent/c', build/'agent/c')
    for name in ('entry.o', 'sdk.a', 'libc.a', 'agent_catalog.inc', 'link-contract'):
        shutil.copy2(source/'agent'/name, build/'agent'/name)
    for name in ('logit.iso', 'esp.img', 'agentd.aex', 'agentctl.aex', 'login.aex',
                 'sh.aex', 'cat.aex', 'echo.aex', 'files.aex', 'textedit.aex',
                 'assistant.aex', 'agent-runtime-test.aex'):
        shutil.copy2(source/name, build/name)
    original = 'if(chmod("/etc/agent.key",0600)<0)\n        printf("AGENTD model credential unavailable; task service remains available\\n");'
    text = (ROOT/'c/apps/agent/agentd.c').read_text()
    if text.count(original) != 1:
        raise RuntimeError('startup control anchor changed')
    variant = out/'fatal-agentd.c'
    variant.write_text(text.replace(original, 'if(chmod("/etc/agent.key",0600)<0&&errno!=ENOENT)return 1;'))
    # Derive target flags from make instead of maintaining another compiler
    # command. The copied build is private; no source or caller artifact changes.
    obj = build/'agent/c/apps/agent/agentd.o'
    planned = subprocess.run(['make', '-Bn', f'BUILD={build}', str(obj)],
                             cwd=ROOT, check=True, capture_output=True, text=True)
    commands = [shlex.split(line) for line in planned.stdout.splitlines()
                if ' -c c/apps/agent/agentd.c -o ' in line]
    if len(commands) != 1:
        raise RuntimeError('cannot derive broker compile command')
    command = commands[0]
    command[command.index('c/apps/agent/agentd.c')] = str(variant)
    # Relative includes in agentd.c resolve from its real source directory.
    command += ['-iquote', str(ROOT/'c/apps/agent')]
    with (out/'build.log').open('w') as log:
        subprocess.run(command, cwd=ROOT, check=True, stdout=log, stderr=subprocess.STDOUT)
        subprocess.run(['make', f'BUILD={build}', str(build/'agentd.aex')],
                       cwd=ROOT, check=True, stdout=log, stderr=subprocess.STDOUT)
    return build


def frame(guest, title):
    pattern = rb'\[wm\] win \d+ frame (\d+) (\d+) (\d+) (\d+) content (\d+) (\d+) pt zoom 0 min 0 ' + re.escape(title.encode())
    guest.wait(pattern, 20)
    x, y, w, h, cw, ch = map(int, re.findall(pattern, bytes(guest.log))[-1])
    scale = w/cw
    return lambda px, py: (round(x+px*scale), round(y+h-(ch-py)*scale))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--gateway', type=Path)
    parser.add_argument('--negative', action='store_true')
    parser.add_argument('--mode', choices=('bios', 'uefi'), default='bios')
    parser.add_argument('--ram', default='512M')
    args = parser.parse_args()
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=False); out.chmod(0o700)
    build = args.build.resolve()
    model = None; guest = None; checks = []

    def check(name, condition):
        if not condition: raise AssertionError(name)
        checks.append(name); print('AGENT_STARTUP_PASS', name, flush=True)

    try:
        if args.negative: build = mutant(build, out)
        if args.gateway: gateway = args.gateway.resolve()
        else:
            model = faults.FaultModel(out/'model'); gateway = out/'model'
        extras = [f'{gateway}/{name}:/fixture/{name}' for name in ('agent.conf', 'agent.key')]
        disk = runtime.prepare(build, gateway, out, extras=extras, include_key=False)
        reader = module('startup_fs', ROOT/'tools/license_audit.py')
        fs = reader._LogitFS(disk)
        try: check('boot image has no installed credential', fs.lookup('/etc/agent.key') is None)
        finally: fs.close()
        guest = runtime.Guest(build, disk, out, args.mode, args.ram)
        guest.wait(b'LogitOS shell', 180)
        if args.negative:
            try: guest.wait(b'AGENTD_READY', 12)
            except RuntimeError:
                # This failure precedes creation of /state, which capture()
                # normally uses for its oracle. Read the harmless status RPC
                # from serial here, rather than failing on a missing directory.
                status = guest.command('/bin/agentctl status')
                check('former fatal startup fails readiness assertion',
                      'Task service refused operation' in status and bytes(guest.log).count(b'load /bin/agentd:') >= 2)
                (out/'result.json').write_text(json.dumps({'control': 'fatal-missing-key', 'detected': True, 'checks': checks}, indent=2))
                print('CONTROL DETECTED: service ready without credential', flush=True)
                return
            raise AssertionError('startup control unexpectedly ready')
        guest.wait(b'AGENTD_READY', 90)
        before = guest.capture('/bin/agentctl status')
        check('task service ready without credential', 'SERVICE elapsed_ms=' in before)
        guest.capture('/bin/agent-runtime-test open-document')
        at = frame(guest, '/untitled.txt')
        guest.click(*at(40, 40)); guest.type('hello world unsaved source')
        guest.click(*at(365, 348))  # Actual TextEdit Ask Logit control, in points.
        assistant = frame(guest, 'Logit Assistant')
        guest.click(*assistant(80, 106)); guest.type('summarize this text with source citations')
        guest.click(*assistant(95, 171))
        waiting = faults.phase(guest, 1, 'waiting for model')
        check('unconfigured task waits without spending budget', waiting[3:5] == ('0', '0'))
        source = guest.capture('/bin/cat /state/agents/u0/t1/source0')
        check('Ask Logit published the actual unsaved document', source == 'hello world unsaved source')
        time.sleep(2)  # Tasks refreshes state every 1.5 s; capture that repaint.
        guest.screenshot(); shutil.copyfile(out/'desktop.ppm', out/'waiting.ppm')
        check('memory saved without inference', 'revision=2' in guest.capture('/bin/agentctl memory os.logit.textedit 1 "Keep my draft."'))
        guest.capture('/bin/agentctl pause 1'); faults.phase(guest, 1, 'paused')
        guest.capture('/bin/agentctl resume 1'); waiting = faults.phase(guest, 1, 'waiting for model')
        check('resume while unconfigured still spends no calls', waiting[3:5] == ('0', '0'))
        faults.create(guest, 2); faults.phase(guest, 2, 'waiting for model')
        guest.capture('/bin/agentctl cancel 2'); cancelled = faults.phase(guest, 2, 'cancelled')
        check('unconfigured task can be cancelled', cancelled[3:5] == ('0', '0'))
        after = guest.capture('/bin/agentctl status')
        elapsed = lambda s: int(re.search(r'SERVICE elapsed_ms=(\d+)', s)[1])
        if elapsed(after)-elapsed(before) < 5000:
            time.sleep(5); after = guest.capture('/bin/agentctl status')
        check('broker survives the former restart interval',
              elapsed(after)-elapsed(before) >= 5000 and bytes(guest.log).count(b'AGENTD_READY') == 1)
        if model: check('no provider request before setup', model.calls == 0)
        snapshot = faults.rows(guest); guest.close(); guest = None
        restart = out/'reboot'; restart.mkdir()
        guest = runtime.Guest(build, disk, restart, args.mode, args.ram)
        guest.wait(b'AGENTD_READY', 180)
        check('unconfigured reboot retains waiting and cancelled tasks', faults.rows(guest) == snapshot)
        check('unconfigured reboot retains memory', 'Keep my draft.' in guest.capture('/textedit.aex --aex-memory'))
        check('later model configuration installed durably', 'AGENT_MODEL_FIXTURE_INSTALLED' in guest.capture('/bin/agent-runtime-test install-model-fixture'))
        guest.capture('/bin/agentctl resume 1'); done = guest.complete(1)
        check('configured resume completes the original task', done[2:4] == ('2', '2'))
        check('resumed report matches its on-disk artifact', 'DOCUMENT_VERIFIED' in guest.capture('/bin/agentctl verify 1'))
        check('configuration does not resurrect cancelled work', faults.rows(guest)[2] == cancelled)
        if model: check('only resumed task calls model', model.calls == 2)
        (out/'result.json').write_text(json.dumps({'status': 'passed', 'simulated_model': model is not None,
            'mode': args.mode, 'ram': args.ram, 'checks': checks, 'task': done}, indent=2))
        print('AGENT_STARTUP_PASS_ALL', len(checks), flush=True)
    finally:
        if guest:
            try: guest.screenshot()
            except Exception: pass
            guest.close()
        if model: model.close()


if __name__ == '__main__': main()
