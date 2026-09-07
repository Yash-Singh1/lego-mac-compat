#!/usr/bin/env python3
"""Preserve copied classic Mac resources in data forks before ad-hoc signing.

Only operate on a generated compatibility bundle. The source app is untouched.
Resource Manager bridges read these ordinary files on modern macOS.
"""
from pathlib import Path
import subprocess
import sys

bundle = Path(sys.argv[1])
if not bundle.name.endswith('-Compat.app') or not (bundle / 'Contents').is_dir():
    raise SystemExit('expected a generated *-Compat.app bundle')
for path in [bundle, *bundle.rglob('*')]:
    if path.is_symlink():
        continue
    attrs = subprocess.check_output(['xattr', str(path)], text=True).splitlines()
    if 'com.apple.ResourceFork' in attrs:
        payload = bytes.fromhex(subprocess.check_output(
            ['xattr', '-px', 'com.apple.ResourceFork', str(path)], text=True))
        if payload:
            destination = path if path.is_file() and path.stat().st_size == 0 else Path(str(path) + '.lp32-rsrc')
            if destination != path and destination.exists() and destination.read_bytes() != payload:
                raise SystemExit(f'resource sidecar already exists with different contents: {destination}')
            destination.write_bytes(payload)
        subprocess.run(['xattr', '-d', 'com.apple.ResourceFork', str(path)], check=True)
    if 'com.apple.FinderInfo' in attrs:
        subprocess.run(['xattr', '-d', 'com.apple.FinderInfo', str(path)], check=True)
