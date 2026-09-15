# SPDX-License-Identifier: MIT
"""Reap only the Popen instances a guest test created.

USB used kill() on dma-qemu-wrapper.py, leaving its actual QEMU orphaned.
A wrapper needs SIGTERM and time to reap its child before its owner escalates.
The inner wrapper uses a shorter grace period than the outer harness.
"""
import signal
import subprocess


def stop_owned(process, timeout=10, sig=signal.SIGTERM):
    if process is None:
        return
    if process.poll() is None:
        process.send_signal(sig)
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def run_owned(cmd, timeout, **kwargs):
    """Like run for redirected-output USB gates, but cancel before killing.

    subprocess.run(timeout=...) sends SIGKILL to the instrument script; its
    finally block then cannot stop QEMU. The extra 15-second grace lets that
    script drain its 10-second cleanup and the wrapper's 3-second cleanup.
    """
    process = subprocess.Popen(cmd, **kwargs)
    try:
        return subprocess.CompletedProcess(cmd, process.wait(timeout=timeout))
    finally:
        stop_owned(process, timeout=15)
