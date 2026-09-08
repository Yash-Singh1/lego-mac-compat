#!/usr/bin/env python3
"""Check that an actual illegal instruction records fault details under Rosetta."""
import os
from pathlib import Path
import subprocess


def main():
    native = Path(__file__).resolve().parents[1]
    for setting, signal in (("SIGILL", 4), ("1", 11)):
        result = subprocess.run(
            ["arch", "-x86_64", str(native / "build/game_loader")],
            env=dict(os.environ, LP32_CRASH_DIAGNOSTIC_SELFTEST=setting),
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=20,
        )
        assert result.returncode == 128 + signal, result.stdout
        for expected in (f"compat32: signal {signal} ", "rip=0x", "crash rax=", "crash cs="):
            assert expected in result.stdout, result.stdout
    print("PASS: SIGILL instruction trap and existing SIGSEGV diagnostic")


if __name__ == "__main__":
    main()
