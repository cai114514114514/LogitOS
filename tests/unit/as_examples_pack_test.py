#!/usr/bin/env python3
"""Guard native example selection and explicit launch routing during cutover."""

import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/as_examples.py"


def load(path):
    spec = importlib.util.spec_from_file_location("as_examples_pack", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def exercise(tool, work):
    native = work / "native.as"
    native.write_text("# aether: 3.0\ndef main() -> None:\n    print(42)\n")
    old = work / "old.as"
    old.write_text("# aether: 2\nprint(42)\n")
    assert tool.guest_command(native) == "/usr/as/bin/native.aex", "native routing"
    assert tool.guest_command(native, True) == "/usr/as/bin/native.aex &"
    assert tool.guest_command(old) == "as /usr/as/examples/old.as"
    # There are no artifacts in this temporary directory. Missing native
    # output must never make the launcher choose the VM source path.
    assert tool.source_version(native) == 3
    for declaration in ("# aether: 1", "# unrelated\n# aether: 3.0", ""):
        invalid = work / "invalid.as"
        invalid.write_text(declaration + "\n")
        try:
            tool.source_version(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError("example packer accepted an unversioned/retired source")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="as-examples-pack-") as temporary:
        work = Path(temporary)
        tool = load(TOOL)
        exercise(tool, work)
        sources = sorted((ROOT / "fsroot/as/examples").glob("*.as"))
        result = subprocess.run(["python3", str(TOOL), "sources", *map(str, sources)],
                                check=True, capture_output=True, text=True, timeout=30)
        selected = set(result.stdout.splitlines())
        assert selected and selected == {str(p) for p in sources if tool.source_version(p) == 3}
        # GNU Make ignores the exit status of $(shell ...). A classifier error
        # must stop the build instead of turning a partial list into a disk
        # that silently omits native programs.
        invalid = work / "invalid.as"
        result = subprocess.run(
            ["make", "--no-print-directory", "-n", f"BUILD={work / 'build'}",
             f"AS_EXAMPLES={invalid}", "as-native-examples"],
            cwd=ROOT, capture_output=True, text=True, timeout=30,
        )
        assert result.returncode != 0 and "Could not classify AetherScript examples" in result.stderr, result
        if args.negative_control:
            path = work / "broken.py"
            text = TOOL.read_text()
            anchor = 'command = shlex.quote("/usr/as/bin/" + path.stem + ".aex")'
            assert text.count(anchor) == 1
            path.write_text(text.replace(anchor, 'command = "as /usr/as/examples/" + path.name'))
            try:
                exercise(load(path), work)
            except AssertionError as error:
                assert str(error) == "native routing", error
            else:
                raise AssertionError("packer accepted routing A3 to the VM")
            print("PASS pack control: routing an A3 example to source execution is rejected")
        else:
            print(f"PASS native packaging selection: {len(selected)} shipped A3 examples")


if __name__ == "__main__":
    main()
