#!/usr/bin/env python3
"""Keep the publisher's app-and-Data layout, without a mounted-image dependency."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    required_files = ["GAME.DAT"] + [
        f"EPISODE_{episode}.DAT" for episode in ("I", "II", "III", "IV", "V", "VI")
    ]
    missing = [name for name in required_files if not (args.source / name).is_file()]
    missing += [name for name in ("Audio", "Movies") if not (args.source / name).is_dir()]
    if missing:
        parser.error(
            f"missing game data in {args.source}: {', '.join(missing)}; "
            "pass SAGA_DATA_SOURCE=/path/to/Data (for example /Volumes/SagaData/Data)"
        )
    if args.source.resolve() != args.destination.resolve():
        subprocess.run(
            ["ditto", "--noextattr", "--noqtn", str(args.source), str(args.destination)],
            check=True,
        )
    print(f"Saga game data ready: {args.destination}; keep it beside the compatibility app.")


if __name__ == "__main__":
    main()
