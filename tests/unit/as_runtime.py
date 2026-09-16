"""Read the runtime's authoritative manifest for host builds and private controls."""

from pathlib import Path
import re
import shutil

RUNTIME = Path(__file__).resolve().parents[2] / "c/apps/as/runtime"


def runtime_sources(directory=RUNTIME):
    directory = Path(directory)
    names = []
    for line in (directory / "sources.def").read_text().splitlines():
        match = re.fullmatch(r"AT_RUNTIME_SOURCE\(([a-z_]+)\)", line)
        if match:
            names.append(match.group(1))
        elif line.strip() and not line.startswith("/*"):
            raise AssertionError(f"Unknown runtime manifest entry: {line}")
    assert names and len(names) == len(set(names)), "Empty or duplicate runtime manifest"
    sources = [directory / (name + ".c") for name in names]
    assert all(source.is_file() for source in sources), sources
    return sources


def copy_runtime(destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    for source in [*runtime_sources(), *RUNTIME.glob("*.h"), RUNTIME / "sources.def"]:
        shutil.copyfile(source, destination / source.name)
