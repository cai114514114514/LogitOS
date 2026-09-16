#!/usr/bin/env python3
"""Run both native launcher modes through Preview's real association gate.

The viewer and kernel are prebuilt dependencies with recorded hashes. Each
private disk contains the seven newly compiled launchers, media, fonts and the
ordinary shell, but no AetherScript VM or legacy compiler caches.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/unit"))
sys.path.insert(0, str(ROOT / "tools"))
from as_gui_test import guest_assets
from as_preview_launcher_test import FIXTURES, MEDIA
from as_examples import guest_command
from license_audit import _LogitFS

MEDIA_SOURCES = {
    "/media/sample.mp3": "tests/fixtures/audio/sample.mp3",
    "/media/sample.flac": "tests/fixtures/audio/sample.flac",
    "/media/sample.wav": "tests/fixtures/audio/sample.wav",
    "/media/clip.mp4": "tests/fixtures/media/h264-mp3-nobf.mp4",
    "/media/clip.mkv": "tests/fixtures/media/h264-flac.mkv",
    "/media/clip.webm": "tests/fixtures/media/vp9-opus.webm",
    "/media/img/still.webp": "tests/fixtures/image/still.webp",
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--base", type=Path, default=ROOT / "build")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    build, base, out = args.build.resolve(), args.base.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"passed": False, "compiler_sha256": digest(build / "asc"),
              "kernel_sha256": digest(base / "logit.iso"),
              "viewer_sha256": digest(base / "preview.aex"), "modes": []}
    try:
        assert set(MEDIA.values()) == set(MEDIA_SOURCES)
        assets = guest_assets()
        for mode in ("debug", "release"):
            directory = out / mode
            directory.mkdir()
            files = [f"{base}/{name}.aex:/bin/{name}" for name in ("login", "sh", "echo", "cat")]
            files.append(f"{base}/preview.aex:/preview.aex")
            files.extend(f"{source}:{destination}" for source, destination in assets)
            files.extend(f"{ROOT}/{source}:{destination}" for destination, source in MEDIA_SOURCES.items())
            current = {"mode": mode, "programs": [], "passed": False, "cpus": 1}
            report["modes"].append(current)
            for name in MEDIA:
                source = FIXTURES / (name + ".as")
                artifact = directory / (name + ".aex")
                command = [str(build / "asc"), "build", str(source), "--target", "logitos-x86_64",
                           "--stdlib", str(ROOT / "fsroot/as/lib"), "--json", "-o", str(artifact)]
                if mode == "debug":
                    command.append("--debug")
                result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                (directory / (name + "-build.json")).write_text(result.stdout)
                assert result.returncode == 0, result.stdout + result.stderr
                built = json.loads(result.stdout)
                assert built["ok"] and artifact.read_bytes()[:4] == b"AEX1", built
                destination = "/usr/as/bin/" + artifact.name
                assert guest_command(source) == destination
                files.append(f"{artifact}:{destination}")
                current["programs"].append({"name": name, "sha256": digest(artifact),
                                             "destination": destination, "snapshot": built["snapshot"]})
            disk = directory / "disk.img"
            with (directory / "mkfs.log").open("w") as log:
                subprocess.run(["python3", "tools/mkfs.py", str(disk), *files], cwd=ROOT,
                               stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
            filesystem = _LogitFS(disk)
            try:
                for retired in ("/bin/as", "/usr/as/lib/asc.la", "/usr/as/lib/aslex.la"):
                    assert filesystem.lookup(retired) is None, retired
            finally:
                filesystem.close()
            current["vm_present"] = False
            # The original gate observes the association receiver, the exact
            # filename/format and rendered content, and now also the native
            # launcher's exit. Its decoder/refusal checks remain unchanged.
            with (directory / "association.log").open("w") as log:
                result = subprocess.run([
                    "python3", "tests/qmp/qmp_preview.py", "--iso", str(base / "logit.iso"),
                    "--disk", str(disk), "--out", str(directory / "screenshots"), "--assoc", "--keep",
                ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=900)
            current["exit_code"] = result.returncode
            # --keep preserves the raw receiver log and captured audio in a
            # temporary QEMU directory. Copy them into the durable report even
            # on failure, rather than leaving evidence only in /tmp.
            log_text = (directory / "association.log").read_text()
            kept = re.search(r"^kept (.+) \(serial log, wav capture\)$", log_text, re.MULTILINE)
            if kept:
                evidence = directory / "receiver-evidence"
                evidence.mkdir()
                for path in Path(kept[1]).iterdir():
                    if path.is_file() and path.suffix in (".log", ".wav"):
                        shutil.copyfile(path, evidence / path.name)
            assert result.returncode == 0, f"{mode}: see {directory / 'association.log'}"
            current["passed"] = True
            print(f"PASS seven A3 Preview associations: {mode}", flush=True)
        report["passed"] = True
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        (out / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
