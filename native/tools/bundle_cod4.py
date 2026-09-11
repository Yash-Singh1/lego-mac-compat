#!/usr/bin/env python3
"""Build a COD4 compatibility app from the user's original Steam Mac installation."""
import argparse
import hashlib
import plistlib
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

EXECUTABLES = {
    "sp": ("Call of Duty 4", "43629c7f2f1b6f891e93c134f3101dbfb9fc4b46cabb1491e90437a450208870"),
    "mp": ("Call of Duty 4 Multiplayer", "f90ec11a0822628aac0c2f788d980967ad333405b2fec5f3ecfbca1e3d8d1374"),
}


def steam_source(steam=None):
    steam = steam or Path.home() / "Library/Application Support/Steam"
    libraries = [steam]
    folders = steam / "steamapps/libraryfolders.vdf"
    if folders.is_file():
        libraries += [Path(p.replace("\\\\", "\\")) for p in
                      re.findall(r'"path"\s+"([^"\n]+)"', folders.read_text())]
    for library in dict.fromkeys(libraries):
        manifest = library / "steamapps/appmanifest_7940.acf"
        if not manifest.is_file():
            continue
        match = re.search(r'"installdir"\s+"([^"\n]+)"', manifest.read_text())
        if match and Path(match[1]).name == match[1]:
            source = library / "steamapps/common" / match[1] / "Call of Duty 4.app"
            if source.is_dir():
                return source.resolve()
    raise ValueError("Steam's Mac installation of app 7940 was not found; pass SOURCE_APP=/path/to/Call of Duty 4.app")


def validate_source(source, mode):
    contents = source / "Contents"
    selected = contents if mode == "sp" else contents / "Call of Duty 4 Multiplayer.app/Contents"
    name, digest = EXECUTABLES[mode]
    image = selected / "MacOS" / name
    required = [selected / "Info.plist", selected / ("Resources/Game.icns" if mode == "sp" else "Resources/Game_mp.icns"), image,
                selected / "MacOS/libBinkMachOx86.dylib", selected / "MacOS/libsteam_api.dylib",
                contents / "Call of Duty 4 Data/main/iw_00.iwd"]
    missing = [str(p) for p in required if not p.is_file()]
    if missing:
        raise ValueError("Missing Steam Mac files: " + ", ".join(missing))
    info = plistlib.loads((selected / "Info.plist").read_bytes())
    if info.get("CFBundleIdentifier") != f"com.aspyr.callofduty4.{mode}.steam" or info.get("CFBundleShortVersionString") != "1.7.2":
        raise ValueError("Expected the Aspyr Steam Mac 1.7.2 release")
    if hashlib.sha256(image.read_bytes()).hexdigest() != digest:
        raise ValueError(f"Unsupported {name} executable; its fingerprint differs from the tested Steam build")
    return selected, image, info


def validate_destination(source, bundle):
    source, destination = source.resolve(), bundle.resolve()
    if source == destination or source in destination.parents or destination in source.parents:
        raise ValueError("Build destination must be separate from the Steam source installation")
    if bundle.exists():
        info_path = bundle / "Contents/Info.plist"
        info = plistlib.loads(info_path.read_bytes()) if info_path.is_file() else {}
        if info.get("LP32GeneratedGame") != "cod4":
            raise ValueError("Refusing to replace a directory that is not a generated COD4 compatibility app")


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def ditto(source, target):
    run("ditto", "--noextattr", "--noqtn", source, target)


def build(source, mode, loader, bundle, inactive, runtime):
    selected, image, info = validate_source(source, mode)
    validate_destination(source, bundle)
    if not loader.is_file():
        raise ValueError(f"Missing loader: {loader}")
    from prepare_guest_runtime import LIBRARIES, sha256
    for name, (_, digest) in LIBRARIES.items():
        if not (runtime / name).is_file() or sha256(runtime / name) != digest:
            raise ValueError("Missing or unrecognized private C++ runtime; run make cod4-runtime")
    run("lipo", selected / "MacOS/libsteam_api.dylib", "-verify_arch", "x86_64")
    suffix = "MP" if mode == "mp" else ""
    bundle.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".cod4-stage-", dir=bundle.parent) as tmp:
        staged = Path(tmp) / bundle.name
        contents = staged / "Contents"
        for name in ("MacOS", "SharedSupport", "Resources"):
            (contents / name).mkdir(parents=True)
        ditto(selected / "Resources", contents / "Resources")
        ditto(source / "Contents/Call of Duty 4 Data", contents / "Call of Duty 4 Data")
        shutil.copy2(loader, contents / f"MacOS/COD4{suffix}Compat")
        target = contents / f"SharedSupport/COD4{suffix}.image"
        shutil.copy2(image, target)
        target.chmod(0o644)
        # The original i386 Bink runs as guest code; the Steam SDK runs through
        # the host bridge using the publisher's unmodified x86_64 slice.
        shutil.copy2(selected / "MacOS/libBinkMachOx86.dylib", contents / "SharedSupport/libBinkMachOx86.dylib")
        ditto(runtime, contents / "SharedSupport/compat-runtime")
        steam = contents / "Resources/libsteam_api.dylib"
        if steam.exists():
            steam.unlink()
        run("lipo", selected / "MacOS/libsteam_api.dylib", "-thin", "x86_64", "-output", steam)
        run("codesign", "--force", "--sign", "-", steam)
        info.update(CFBundleExecutable=f"COD4{suffix}Compat", CFBundleIdentifier=info["CFBundleIdentifier"] + ".compat",
                    CFBundleName=f"Call of Duty 4{(' Multiplayer' if suffix else '')} (Compatibility)",
                    CFBundleDisplayName=f"Call of Duty 4{(' Multiplayer' if suffix else '')} (Compatibility)",
                    LSMinimumSystemVersion="11.0", NSHighResolutionCapable=False,
                    LP32GeneratedGame="cod4", LP32ContinueWhenInactive=bool(inactive))
        if mode == "mp":
            # The original NSURLDownload code fetches server-selected mods
            # over HTTP. Linking our host against a modern SDK otherwise adds
            # ATS restrictions and fails these downloads with error -1022.
            info["NSAppTransportSecurity"] = {"NSAllowsArbitraryLoads": True}
        (contents / "Info.plist").write_bytes(plistlib.dumps(info))
        run("codesign", "--force", "--sign", "-", staged)
        run("codesign", "--verify", "--strict", staged)
        previous = Path(tmp) / "previous.app"
        if bundle.exists():
            bundle.rename(previous)
        try:
            staged.rename(bundle)
        except BaseException:
            if previous.exists():
                previous.rename(bundle)
            raise
    print(f"COD4 {mode} compatibility bundle: {bundle}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-app", default="")
    parser.add_argument("--mode", choices=EXECUTABLES, default="sp")
    parser.add_argument("--loader", type=Path, default=Path("build/game_loader"))
    parser.add_argument("--bundle", type=Path, default=Path("build/COD4-Compat.app"))
    parser.add_argument("--runtime", type=Path, default=Path("build/guest-runtime"))
    parser.add_argument("--continue-when-inactive", type=int, choices=(0, 1), default=0)
    args = parser.parse_args()
    try:
        source = Path(args.source_app).resolve() if args.source_app else steam_source()
        build(source, args.mode, args.loader.resolve(), args.bundle.absolute(), args.continue_when_inactive, args.runtime.resolve())
    except ValueError as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
