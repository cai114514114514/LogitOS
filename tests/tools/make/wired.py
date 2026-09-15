#!/usr/bin/env python3
"""Exercise discovery against a temporary repository, including watched failures.

Using the real checker on missing and restored include edges makes a silently
empty inventory fail this gate. Fixtures also keep same-name leaf fragments
separate and verify the CI/negative-control inventories see the moved files.
"""
import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class NestedMakeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.wired = load('mk_wired')
        self.write('Makefile', '-include tests/top.mk\n')
        self.write('tests/top.mk', '')
        self.write('tests/schedneg.mk', '# declared wrapper\n')
        self.write('tests/gpu/amd/stage.mk', 'test-nested: test-control\n\t@true\n')

    def write(self, name, body):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(body)

    def check(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = self.wired.main(self.root)
        return status, output.getvalue()

    def test_missing_then_restored_edge(self):
        status, output = self.check()
        self.assertEqual(status, 1)
        self.assertIn('tests/gpu/amd/stage.mk  1 gate(s): test-nested', output)
        print('negative control: nested fragment without include is UNREACHABLE')
        self.write('tests/top.mk', '-include tests/gpu/amd/stage.mk\n')
        status, output = self.check()
        self.assertEqual(status, 0)
        self.assertIn('3 fragments, 2 reachable', output)

    def test_declared_exception_is_path_specific(self):
        self.write('tests/top.mk', '-include tests/gpu/amd/stage.mk\n')
        self.write('tests/gpu/amd/schedneg.mk', 'test-not-exempt:\n')
        status, output = self.check()
        self.assertEqual(status, 1)
        self.assertIn('tests/gpu/amd/schedneg.mk  1 gate(s)', output)

    def test_same_leaf_is_not_same_fragment(self):
        self.write('tests/other/stage.mk', 'test-other:\n')
        self.write('tests/top.mk', '-include tests/gpu/amd/stage.mk\n')
        status, output = self.check()
        self.assertEqual(status, 1)
        self.assertIn('tests/other/stage.mk', output)
        self.assertNotIn('tests/gpu/amd/stage.mk  ', output)

    def test_continuations_multiple_paths_comments_and_cycle(self):
        self.write('tests/next.mk', '-include tests/top.mk\n')
        self.write('tests/top.mk', '-include tests/next.mk \\\r\n tests/gpu/amd/stage.mk # tail\n')
        self.assertEqual(self.check()[0], 0)

    def test_comment_recipe_and_partial_filename_are_not_edges(self):
        self.write('tests/top.mk', '# -include tests/gpu/amd/stage.mk\n'
                   '\t-include tests/gpu/amd/stage.mk\n'
                   '-include tests/gpu/amd/stage.mk.backup\n')
        self.assertEqual(self.check()[0], 1)

    def test_ci_and_control_inventories(self):
        self.write('tests/top.mk', 'ci-host: test-nested\n')
        audit = load('audit_tests')
        audit.ROOT = str(self.root)
        texts = audit.makefile_texts()
        self.assertIn('tests/gpu/amd/stage.mk', texts)
        targets = audit.collect_targets(texts)
        self.assertIn('test-nested', audit.reachable(targets, ['ci-host']))
        drift = load('negctl_drift')
        drift.ROOT = str(self.root)
        self.assertIn(str(self.root / 'tests/gpu/amd/stage.mk'), drift.makefiles())

    def test_nested_link_prerequisites_are_checked(self):
        # Run the actual entry point from a copied checker, so changing only
        # its helper parser cannot hide a regressed top-level file inventory.
        import subprocess
        self.write('tools/mk_prereq.py', (ROOT / 'tools/mk_prereq.py').read_text())
        self.write('tests/gpu/amd/stage.mk', '$(BUILD)/nested.elf: $(GPU_OBJ)\n'
                   '\t$(LD) -o $@ $(GPU_OBJ) $(MISSING_OBJ)\n')
        result = subprocess.run(['python3', str(self.root / 'tools/mk_prereq.py')],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('tests/gpu/amd/stage.mk', result.stdout)
        self.assertIn('MISSING_OBJ', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
