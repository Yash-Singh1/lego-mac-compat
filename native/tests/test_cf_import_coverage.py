#!/usr/bin/env python3
"""Check that every linked Core Foundation function has a dispatch case.

This checks static coverage, not the behavior of every game code path.
"""

import pathlib
import re
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit("usage: test_cf_import_coverage.py SOURCE_DIR IMAGE...")
    source_dir = pathlib.Path(sys.argv[1])
    source = "\n".join(
        path.read_text(errors="replace")
        for path in source_dir.iterdir()
        if path.suffix in {".c", ".m", ".cpp"}
    )
    cases = set()
    for pattern in (
        r'LP32_NAME_IS\(\s*import_name\s*,\s*import_length\s*,\s*"(_CF\w+)"',
        r'\bIS\("(_CF\w+)"\)',
        r'import_is\(\s*name\s*,\s*"(_CF\w+)"',
    ):
        cases.update(re.findall(pattern, source))
    failed = False
    for image_name in sys.argv[2:]:
        image = pathlib.Path(image_name)
        imports = {
            line.strip()
            for line in subprocess.check_output(["nm", "-u", str(image)], text=True).splitlines()
            if line.strip().startswith("_CF")
        }
        missing = sorted(imports - cases)
        if missing:
            failed = True
            print(f"{image}: {len(missing)} Core Foundation imports without a dispatch case")
            print("\n".join(missing))
        else:
            print(f"{image}: PASS ({len(imports)} linked Core Foundation functions)")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
