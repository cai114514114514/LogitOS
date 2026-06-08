export const meta = {
  name: 'files-app-impl',
  description: 'Implement the Aether Files manager per the committed plan, in sequential build/test-gated stages',
  phases: [
    { title: 'Kernel', detail: 'vfs_rename + SYS_RENAME/SYS_OPEN_PATH + EV_MOUSE_R (plan steps 1-3)' },
    { title: 'App', detail: 'the files.c ring-3 app (plan step 4)' },
    { title: 'CLI', detail: 'cp + mv coreutils (plan step 5)' },
    { title: 'Tests', detail: 'shell round-trip + qmp_files.py (plan steps 6-7)' },
  ],
}
const PLAN = '/Users/wangzhe/ststem/docs/superpowers/plans/2026-06-06-files-manager.md'
const SPEC = '/Users/wangzhe/ststem/docs/superpowers/specs/2026-06-06-files-manager.md'
const SCHEMA = {
  type: 'object', additionalProperties: false,
  properties: {
    ok: { type: 'boolean', description: 'true ONLY if every gate command for this stage passed' },
    summary: { type: 'string' },
    files_changed: { type: 'array', items: { type: 'string' } },
    gate_output: { type: 'string', description: 'literal tail of the gate command output (build + tests) as proof; verbatim errors if not ok' },
  },
  required: ['ok', 'summary', 'files_changed', 'gate_output'],
}
const base = `You are implementing "Aether Files" in /Users/wangzhe/ststem (cwd = repo root). The authoritative,
line-anchored plan is at ${PLAN} and the spec at ${SPEC}. READ THE PLAN FIRST. It was grounded against
the real code; its symbols/line anchors are verified. Follow it exactly; if you must deviate, do the
correct thing and record it in summary. The system builds clean and ALL tests pass today -- do not
regress anything. Report ok ONLY if your stage's gate commands actually pass (run them, observe real
output, paste the tail in gate_output).`

async function stage(phaseName, prompt, label) {
  let r = await agent(`${base}\n\n${prompt}`, { schema: SCHEMA, label, phase: phaseName, agentType: 'general-purpose' })
  for (let k = 0; k < 2 && !r.ok; k++) {
    r = await agent(`${base}\n\nThe previous attempt at this stage left the gate RED. The partial work is on disk; diagnose and FIX it (read current state, do not start over). Failing output:\n----\n${r.gate_output}\n----\nFix the root cause, re-run the gate, report.`,
      { schema: SCHEMA, label: `${label}:fix${k + 1}`, phase: phaseName, agentType: 'general-purpose' })
  }
  return r
}

phase('Kernel')
const s1 = await stage('Kernel',
  `Implement PLAN STEPS 1, 2, and 3 only (vfs_rename in fs/{vfs.h,vfs.c,aetherfs.c}; SYS_RENAME in syscall.c + SYS_OPEN_PATH in wm.c + the EV_MOUSE_R/SYS_* defines in aether_abi.h + wrappers in aether.h + path_join in clib.h; right-button in mouse.c + wm.h + wm.c). Honor the aetherfs_rename hazard rules in the plan (resolve-all-first, add-then-remove with rollback, reject existing dest, reject dir-move-cycle, reject root). GATE: run \`make\` (clean), \`make test\` (boot PASS), \`make test-shell\` (PASS). All three must pass.`,
  'kernel')
if (!s1.ok) return { failed_at: 'Kernel', detail: s1 }

phase('App')
const s2 = await stage('App',
  `Implement PLAN STEP 4 only: create src/apps/gui/files.c (the ring-3 Files app) and wire it into the Makefile (APP_RULE + APPS list, base 0x47000000). Model the structure on src/apps/gui/widgets.c and textedit.c; use the aui toolkit (read src/apps/gui/aui.h) + gui_* + the fd API (sys_open/read/write/close from aether.h) + dir_count/dir_name + sys_rename/sys_open_path/make_dir. Implement: path bar + Up, toolbar (New Folder/Rename/Delete/Copy/Cut/Paste), list view (icon+name+size), multi-select (click/shift/ctrl), right-click (EV_MOUSE_R) context menu, in-app clipboard (copy/cut/paste with recursive copy_tree + delete_tree), inline rename/new-folder textfield, Get Info, double-click open-file(sys_open_path)/enter-folder. GATE: \`make build/disk.img\` builds files.aex with NO new warnings/errors and packs it. (Functional GUI verified later in the Tests stage; here the gate is a clean build + the app links.)`,
  'app')
if (!s2.ok) return { failed_at: 'App', detail: s2, kernel: s1.summary }

phase('CLI')
const s3 = await stage('CLI',
  `Implement PLAN STEP 5 only: create src/apps/coreutils/cp.c (cp [-r], fd copy loop + recursive dir copy via make_dir + dir listing, using clib.h path_join) and mv.c (mv src dst via sys_rename), and add \`cp mv\` to the Makefile CLI list. Model on src/apps/coreutils/{cat.c,rm.c,net.c}. GATE: \`make build/disk.img\` builds cp.aex + mv.aex packed under /bin, no new warnings/errors.`,
  'cli')
if (!s3.ok) return { failed_at: 'CLI', detail: s3 }

phase('Tests')
const s4 = await stage('Tests',
  `Implement PLAN STEPS 6 and 7: (6) extend scripts/run-shell-test.sh with the deterministic cp/mv/mkdir/rm round-trip in a self-created /cptest dir + a "cpmvprobe"-from-cat assertion (keep existing markers). (7) create tools/qmp_files.py modeled on tools/qmp_fs.py that boots headless, launches the Files app from the Dock, optionally injects a right-button + new-folder, screendumps to the out .ppm, and prints PASS. GATE: \`make test-shell\` PASS (incl. the new cpmvprobe assertion). Run qmp_files.py too if feasible and report whether it completed, but the hard gate is make test-shell.`,
  'tests')

return {
  ok: !!(s1.ok && s2.ok && s3.ok && s4.ok),
  kernel: s1.summary, app: s2.summary, cli: s3.summary, tests: s4.summary,
  tests_ok: s4.ok, tests_gate: s4.gate_output,
  all_files: [s1, s2, s3, s4].flatMap(s => s.files_changed || []),
}
