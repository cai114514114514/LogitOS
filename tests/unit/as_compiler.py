"""Use the build's compiler inventory when compiling private test mutations.

Link callers with -lm as the ordinary host compiler does: the diagnostic
evaluator uses pow/fmod, which libc does not provide on every supported host.
"""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def compiler_sources():
    # Asking Make avoids a second directory list that can silently drop new
    # translation units. Runtime sources have a separate manifest and ABI.
    result = subprocess.run(
        ["make", "--no-print-directory", "-s", "as-host-sources"],
        cwd=ROOT, check=True, capture_output=True, text=True, timeout=30,
    )
    return [ROOT / line for line in result.stdout.splitlines()]
