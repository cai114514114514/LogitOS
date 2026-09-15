#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real DeepSeek report through the ordinary session launcher and existing disk."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--env', type=Path, required=True)
    parser.add_argument('--mode', choices=('bios', 'uefi'), default='bios')
    args = parser.parse_args()
    build = args.build.resolve(); out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False); out.chmod(0o700)
    defaults = out/'defaults'; defaults.mkdir()
    (defaults/'agent.conf').write_bytes((ROOT/'fsroot/etc/agent.conf').read_bytes())
    runtime = module('session_runtime', ROOT/'tests/boot/run-agent.py')
    source = ROOT/'tests/fixtures/agent/source.md'
    disk = runtime.prepare(build, defaults, out, include_key=False,
        extras=[f'{source}:/unusual-saved-note.txt'])
    launcher = [sys.executable, str(ROOT/'tools/agent_session.py'), '--disk', str(disk),
        '--broker', str(build/'agentd.aex'), '--snapshot-helper', str(build/'lfs_snapshot'),
        '--env', str(args.env.resolve()), '--state-dir', str(out/'sessions'), '--limit', '8', '--']
    guest = None; checks = []

    def check(name, condition):
        if not condition: raise AssertionError(name)
        checks.append(name); print('AGENT_SESSION_PASS', name, flush=True)

    try:
        guest = runtime.Guest(build, disk, out, args.mode, '512M', launcher=launcher)
        guest.wait(b'LogitOS shell', 180); guest.wait(b'AGENTD_READY', 90)
        check('normal launcher starts task service', 'SERVICE elapsed_ms=' in guest.capture('/bin/agentctl status'))
        check('arbitrary existing document survives session setup',
              guest.capture('/bin/cat /unusual-saved-note.txt').encode() == source.read_bytes())
        created = guest.capture('/bin/agentctl create "Summarize Project Cedar with the budget and book count. Cite sources." /docs /docs/source.md')
        check('normal launcher accepts task', 'TASK_CREATED id=1' in created)
        done = guest.complete(1)
        report = guest.capture('/bin/agentctl document 1')
        (out/'report.md').write_text(report)
        check('real report contains source facts and citations',
              '4200' in report.replace(',', '') and '120' in report and re.search(r'\[S1:\d+-\d+\]', report))
        check('real report is saved byte for byte', 'DOCUMENT_VERIFIED' in guest.capture('/bin/agentctl verify 1'))
        check('source material remains unchanged', guest.capture('/bin/cat /docs/source.md').encode() == source.read_bytes())
    finally:
        if guest:
            try: guest.screenshot()
            except Exception: pass
            guest.close()
    sessions = list((out/'sessions').glob('session-*'))
    check('one gateway belongs to this session', len(sessions) == 1)
    metrics = json.loads((sessions[0]/'metrics.json').read_text())
    check('both domain requests reached real DeepSeek', metrics['calls'] == 2 and all(r['http'] == 200 for r in metrics['results']))
    (out/'result.json').write_text(json.dumps({'status': 'passed', 'provider': 'DeepSeek', 'model': 'deepseek-flash',
        'simulated_model': False, 'mode': args.mode, 'checks': checks, 'task': done, 'metrics': metrics}, indent=2))
    print('AGENT_SESSION_REAL_PASS', args.mode, flush=True)


if __name__ == '__main__': main()
