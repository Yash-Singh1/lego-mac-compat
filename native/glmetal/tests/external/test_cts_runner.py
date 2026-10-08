"""CPU-only regression for CTS ordering and crash recovery."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class RunnerRecovery(unittest.TestCase):
    def test_reordered_crash_retries_unstarted_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "cases.txt").write_text("case.a\ncase.b\ncase.c\ncase.d\n")
            (work / "glcts").write_text("#!" + sys.executable + "\n" + r'''
import pathlib, sys
args = dict(x[2:].split("=", 1) for x in sys.argv[1:])
cases = pathlib.Path(args["deqp-caselist-file"]).read_text().splitlines()
with pathlib.Path(args["deqp-log-filename"]).open("w") as log:
    for case in reversed(cases):
        log.write("#beginTestCaseResult " + case + "\n")
        log.flush()
        if case == "case.b":
            sys.exit(1)
        log.write('<Result StatusCode="Pass"/>\n#endTestCaseResult\n')
''')
            (work / "glcts").chmod(0o755)
            env = dict(os.environ, GLCTS_CHUNK_SIZE="4", GLCTS_CHUNK_DELAY_MS="0")
            subprocess.run([sys.executable, str(Path(__file__).with_name("cts_runner.py")),
                            directory, str(work / "cases.txt"), str(work / "out.tsv")],
                           env=env, check=True, capture_output=True)
            self.assertEqual((work / "out.tsv").read_text(),
                             "case.a\tPass\ncase.b\tCrash\ncase.c\tPass\ncase.d\tPass\n")


if __name__ == "__main__":
    unittest.main()
