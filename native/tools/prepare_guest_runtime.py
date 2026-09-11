#!/usr/bin/env python3
"""Extract private i386 C++ libraries from Apple's public Lion download.

Only reads mounted disk images and extracts selected archive members. Does not
run Installer, any package scripts, or change the host's system libraries.
Source: https://support.apple.com/en-us/106383
"""
import argparse
from contextlib import contextmanager
import hashlib
from pathlib import Path
import subprocess
import tempfile

URL = ("https://updates.cdn-apple.com/2021/macos/"
       "041-7683-20210614-E610947E-C7CE-46EB-8860-D26D71F0D3EA/InstallMacOSX.dmg")
INSTALLER_SHA256 = "db0b2300de719fa3e4ee132b55afd4e689211ad5332760fe5fe7a30351c9e75c"
LIBRARIES = {
    "libstdc++.6.dylib": ("libstdc++.6.0.9.dylib", "0e68f2c931e50ecf461059a52480610dbb4c8b02a2f28c1f76efee854976fa00"),
    "libc++abi.dylib": ("libc++abi.dylib", "f2c1ebc33f979e7d48a223babcfa679f54f3040390dc022f2beb732ac4acc9af"),
}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


@contextmanager
def mounted(image, directory):
    run("hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", directory, image)
    try:
        yield directory
    finally:
        run("hdiutil", "detach", directory)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installer", type=Path, help="existing Apple Lion InstallMacOSX.dmg")
    parser.add_argument("--output", type=Path, default=Path("build/guest-runtime"))
    parser.add_argument("--cache", type=Path, default=Path("build/runtime-downloads"))
    args = parser.parse_args()
    output = args.output.resolve()
    if all((output / name).is_file() and sha256(output / name) == digest
           for name, (_, digest) in LIBRARIES.items()):
        print(f"Guest C++ runtime already verified: {output}")
        return
    if args.installer:
        installer = args.installer.resolve()
    else:
        args.cache.mkdir(parents=True, exist_ok=True)
        installer = (args.cache / "InstallMacOSX-Lion.dmg").resolve()
        if not installer.exists():
            partial = installer.with_suffix(".dmg.partial")
            print("Downloading Apple's 4.72 GB Lion archive to extract two runtime libraries.", flush=True)
            run("curl", "--fail", "--location", "--retry", "3", "--output", partial, URL)
            if sha256(partial) != INSTALLER_SHA256:
                raise RuntimeError(f"Downloaded archive checksum mismatch: {partial}")
            partial.replace(installer)
    if sha256(installer) != INSTALLER_SHA256:
        raise RuntimeError(f"Unexpected Lion archive checksum: {installer}")
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lp32-runtime-", dir=output.parent) as temp:
        temp = Path(temp)
        with mounted(installer, temp / "installer") as volume:
            run("xar", "-xf", volume / "InstallMacOSX.pkg",
                "InstallMacOSX.pkg/InstallESD.dmg", cwd=temp)
        with mounted(temp / "InstallMacOSX.pkg/InstallESD.dmg", temp / "esd") as volume:
            run("xar", "-xf", volume / "Packages/BaseSystemBinaries.pkg", "Payload", cwd=temp)
        run("tar", "-xf", temp / "Payload", *["./usr/lib/" + source
            for source, _ in LIBRARIES.values()], cwd=temp)
        for name, (source, digest) in LIBRARIES.items():
            staged = temp / name
            run("lipo", temp / "usr/lib" / source, "-thin", "i386", "-output", staged)
            if sha256(staged) != digest:
                raise RuntimeError(f"Extracted runtime checksum mismatch: {name}")
        for name in LIBRARIES:
            (temp / name).replace(output / name)
    (output / "SOURCE.txt").write_text(
        "Local i386 runtime extracted from Apple's Lion archive; not part of the repository.\n"
        f"{URL}\nArchive SHA-256: {INSTALLER_SHA256}\n")
    print(f"Guest C++ runtime prepared: {output}")


if __name__ == "__main__":
    main()
