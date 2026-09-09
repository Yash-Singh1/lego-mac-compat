#!/usr/bin/env python3
"""Check that an actual illegal instruction records fault details under Rosetta."""
import os
from pathlib import Path
import subprocess
import re
import tempfile


def main():
    native = Path(__file__).resolve().parents[1]
    loader = native / "build/game_loader"
    for setting, signal in (("SIGILL", 4), ("1", 11), ("WORKER_SIGILL", 4), ("BAD_FP", 4)):
        with tempfile.TemporaryDirectory(prefix="tfu-crash-test-") as tmp:
            report = Path(tmp) / "crash.log"
            result = subprocess.run(
                ["arch", "-x86_64", str(loader)],
                env=dict(os.environ, LP32_CRASH_DIAGNOSTIC_SELFTEST=setting,
                         LP32_CRASH_REPORT_SELFTEST_PATH=str(report)),
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=20,
            )
            assert result.returncode == 128 + signal, result.stdout
            for expected in (f"compat32: signal {signal} ", "rip=0x", "crash rax=", "crash cs="):
                assert expected in result.stdout, result.stdout
            saved = report.read_text()
            assert "crash-trace-complete" in saved
            assert "crash-stack-word" in saved
            assert "crash-image" in saved
            assert report.stat().st_mode & 0o777 == 0o600
            pcs = re.findall(r"crash-frame native index=\S+ pc=(\S+)", saved)
            assert pcs
            fault = re.search(r"rip=(\S+)", result.stdout)[1]
            assert int(pcs[0], 16) == int(fault, 16)
            if setting == "BAD_FP":
                assert "crash-unwind-stop invalid-frame-pointer" in saved
                assert len(pcs) == 1
            else:
                assert len(pcs) >= 3, saved
                symbols = subprocess.check_output(
                    ["atos", "-arch", "x86_64", "-o", str(loader), "-l", "0x100000000", *pcs],
                    text=True,
                )
                assert "crash_diagnostic_test_middle" in symbols, symbols
                if setting != "1":
                    assert "crash_diagnostic_test_leaf" in symbols, symbols
            # Simulate another launch with the same artifact path: exclusive
            # creation must retain the first report and fall back to stderr.
            again = subprocess.run(
                ["arch", "-x86_64", str(loader)],
                env=dict(os.environ, LP32_CRASH_DIAGNOSTIC_SELFTEST="SIGILL",
                         LP32_CRASH_REPORT_SELFTEST_PATH=str(report)),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20,
            )
            assert again.returncode == 132
            assert report.read_text() == saved
    print("PASS: native SIGILL/SIGSEGV stacks, worker crash, damaged frame pointer, symbolication, persistent reports")


if __name__ == "__main__":
    main()
