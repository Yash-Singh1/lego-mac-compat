#!/usr/bin/env python3
"""Embed opt-in runtime settings before the generated bundle is signed."""
import argparse
import plistlib
from pathlib import Path


def configure(path, continue_when_inactive=False):
    info = plistlib.loads(path.read_bytes())
    info["LP32ContinueWhenInactive"] = bool(continue_when_inactive)
    path.write_bytes(plistlib.dumps(info))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--continue-when-inactive", choices=("0", "1"), default="0")
    args = parser.parse_args()
    configure(args.bundle / "Contents/Info.plist", args.continue_when_inactive == "1")
