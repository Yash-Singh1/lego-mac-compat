#!/usr/bin/env python3
"""Set MW3 campaign's three texture resolutions to Extra without opening the game."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time


TEXTURE_DVARS = ("r_picmip", "r_picmip_bump", "r_picmip_spec")


def set_extra(config: Path, sys_mb: int | None = None) -> bool:
    original = config.read_bytes()
    updated = original
    values = {name: b"0" for name in TEXTURE_DVARS}
    if sys_mb is not None:
        if sys_mb <= 0:
            raise ValueError("sys_mb must be positive")
        values["sys_sysMB"] = str(sys_mb).encode()
    for name, value in values.items():
        pattern = rb'(?m)^(seta ' + name.encode() + rb' ")[^"]*(")'
        if len(re.findall(pattern, updated)) != 1:
            raise ValueError(f"expected one {name} entry in {config}")
        updated = re.sub(pattern, lambda match: match.group(1) + value + match.group(2), updated)
    if not re.search(rb'(?m)^seta r_picmip_manual "1"$', updated):
        raise ValueError(f"manual texture quality is not enabled in {config}")
    if updated == original:
        return False

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    backup = config.with_name(f"{config.name}.backup-{timestamp}")
    shutil.copy2(config, backup)
    mode = config.stat().st_mode & 0o777
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=config.parent, prefix=".mw3-textures-",
                                         delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(updated)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, config)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f"Set {', '.join(TEXTURE_DVARS)} to Extra in {config}; backup: {backup}", flush=True)
    return True


def wait_for_exit(pid: int) -> None:
    while True:
        result = subprocess.run(["ps", "-p", str(pid), "-o", "stat="],
                                capture_output=True, text=True, check=False)
        state = result.stdout.strip()
        if result.returncode != 0 or not state or state.startswith("Z"):
            break
        time.sleep(1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("--wait-pid", type=int)
    parser.add_argument("--sys-mb", type=int)
    args = parser.parse_args()
    if args.wait_pid:
        print(f"Waiting for MW3 pid {args.wait_pid} to exit before updating settings", flush=True)
        wait_for_exit(args.wait_pid)
    set_extra(args.config, args.sys_mb)


if __name__ == "__main__":
    main()
