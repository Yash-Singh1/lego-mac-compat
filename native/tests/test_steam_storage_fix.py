"""Native directory-walker regressions; only generated temporary files are used."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

executable, library = (str(Path(p).resolve()) for p in sys.argv[1:])
env = dict(os.environ, LP32_MUTE_AUDIO="1")


def run(root, mode):
    result = subprocess.run(
        ["arch", "-x86_64", executable, library, str(root), mode],
        env=env, capture_output=True, text=True, timeout=10, check=True,
    )
    lines = result.stdout.splitlines()
    return [s[5:] for s in lines if s.startswith("file=")], int(lines[0][6:])


files = {
    "profiles/alpha/data.bin": b"first",
    "profiles/beta/data.bin": b"second",
    "profiles/beta/more/deep.dat": b"nested",
    "another branch/child/café.dat": b"unicode and spaces",
    "another branch/second/data.bin": b"sibling",
    "root.dat": b"root",
    "zero.bin": b"",
}
for reverse in (False, True):
    with tempfile.TemporaryDirectory(prefix="lp32-storage-") as root:
        for name, data in list(files.items())[:: -1 if reverse else 1]:
            path = Path(root, name)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        Path(root, "empty directory").mkdir()
        for name in (".DS_Store", "Thumbs.db", "profiles/alpha/.DS_Store"):
            Path(root, name).write_bytes(b"ignored")
        before = {str(p.relative_to(root)): p.read_bytes()
                  for p in Path(root).rglob("*") if p.is_file()}
        original, _ = run(root, "original")
        assert set(original) < set(files), original
        for _ in range(2):  # fresh-process discovery must agree
            repaired, size = run(root, "repaired")
            assert len(repaired) == len(files) and set(repaired) == set(files), repaired
            assert size == sum(map(len, files.values())), size
        after = {str(p.relative_to(root)): p.read_bytes()
                 for p in Path(root).rglob("*") if p.is_file()}
        assert before == after
with tempfile.TemporaryDirectory(prefix="lp32-storage-empty-") as root:
    assert run(root, "repaired") == ([], 0)
    Path(root, "single.bin").write_bytes(b"unchanged")
    assert run(root, "original") == run(root, "repaired") == (["single.bin"], 9)
print("PASS: sibling/nested/root files, creation orders, empty files/directories, "
      "filters, quota bytes, fresh processes, no writes, idempotence and unknown-code guards")
