#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real DeepSeek report through the ordinary session launcher and existing disk."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import time
import sys

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result)
    return result


def main(default_run=False):
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
    ui = module('default_session_ui', ROOT/'tests/boot/run-project-work.py') if default_run else None
    source = ROOT/'tests/fixtures/agent/source.md'
    disk = runtime.prepare(build, defaults, out, include_key=False,
        extras=[f'{source}:/unusual-saved-note.txt'])
    launcher = [sys.executable, str(ROOT/'tools/agent_session.py'), '--disk', str(disk),
        '--broker', str(build/'agentd.aex'), '--snapshot-helper', str(build/'lfs_snapshot'),
        '--env', str(args.env.resolve()), '--state-dir', str(out/'sessions'), '--limit', '8', '--']
    session_root = out/'sessions'
    if default_run:
        # Exercise make's actual ordinary recipe; do not replace it with the
        # specialized run-agent launcher that previously hid the missing wire.
        launcher = [sys.executable, str(ROOT/'tests/boot/run-default-session.py'), '--launch-make',
                    '--disk', str(disk), '--build', str(build), '--env', str(args.env.resolve()), '--']
        session_root = build/'agent-sessions'
    existing_sessions = set(session_root.glob('session-*'))
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
        if default_run:
            position, _, _ = ui.frame(guest, 'Finder')
            def click(x, y):
                ui.move(guest, *position(x, y)); ui.button(guest, True); ui.button(guest, False)
            guest.wait(rb'PROJECT_VIEW path=/docs tasks=0', 30)
            click(310, 62)  # select the actual source row, then the work field
            click(940, 128)
            guest.type('Summarize Project Cedar with the budget and book count. Cite sources.')
            guest.key('ret')
            guest.wait(rb'AGENT_WORKER task=1 ', 60)
            check('native Finder input submits a task through make run', True)
        else:
            created = guest.capture('/bin/agentctl create "Summarize Project Cedar with the budget and book count. Cite sources." /docs /docs/source.md')
            check('normal launcher accepts task', 'TASK_CREATED id=1' in created)
        done = guest.complete(1)
        report = guest.capture('/bin/agentctl document 1')
        (out/'report.md').write_text(report)
        check('real report contains source facts and citations',
              '4200' in report.replace(',', '') and '120' in report and re.search(r'\[S1:\d+-\d+\]', report))
        check('real report is saved byte for byte', 'DOCUMENT_VERIFIED' in guest.capture('/bin/agentctl verify 1'))
        check('source material remains unchanged', guest.capture('/bin/cat /docs/source.md').encode() == source.read_bytes())
        if default_run:
            guest.wait(rb'PROJECT_VIEW path=/docs tasks=1 first=1 phase=6', 30)
            ui.gate.screenshot(guest, 'finder-sent')
            click(952, 338)  # Finder's native Open in TextEdit action
            position, width, height = ui.frame(guest, '/docs/report-1.md')
            time.sleep(1)
            side = min(360, max(290, width*31//100)); left = width-side+24
            click(left+80, height-49)
            guest.type('Append a short Next steps section. Preserve all source facts and citations.')
            guest.key('ret')
            revised = guest.complete(1, allow_conflict=True)
            check('native TextEdit input produces a reviewable revision', revised[1] == 'document conflict')
            candidate = guest.capture('/bin/agentctl candidate 1')
            check('TextEdit candidate preserves source facts and citations',
                  '4200' in candidate.replace(',', '') and '120' in candidate and '[S1:' in candidate)
            click(left+(side-48)//2, height-127)
            time.sleep(1)
            check('native TextEdit Apply saves the exact proposal',
                  guest.capture('/bin/agentctl document 1') == candidate and
                  'DOCUMENT_VERIFIED' in guest.capture('/bin/agentctl verify 1'))
            done = guest.complete(1)
            ui.gate.screenshot(guest, 'textedit-sent')
    finally:
        if guest:
            try: guest.screenshot()
            except Exception: pass
            guest.close()
    sessions = list(set(session_root.glob('session-*')) - existing_sessions)
    check('one gateway belongs to this session', len(sessions) == 1)
    metrics = json.loads((sessions[0]/'metrics.json').read_text())
    expected_calls = 4 if default_run else 2
    check('all domain requests reached real DeepSeek', metrics['calls'] == expected_calls and all(r['http'] == 200 for r in metrics['results']))
    (out/'result.json').write_text(json.dumps({'status': 'passed', 'provider': 'DeepSeek', 'model': 'deepseek-flash',
        'simulated_model': False, 'default_make_run': default_run, 'mode': args.mode, 'checks': checks, 'task': done, 'metrics': metrics}, indent=2))
    print('AGENT_SESSION_REAL_PASS', args.mode, flush=True)


if __name__ == '__main__': main()
